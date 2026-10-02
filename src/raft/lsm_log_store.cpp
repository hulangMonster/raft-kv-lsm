// LsmLogStore 实现（M6）。规格：docs/m6-design.md §2/§3。
//
// 与 FileLogStore 的关系：**逐方法语义等价**（设计 §2.4 的等价性表），差异只有三处、
// 且都是「更强」：
//   ① 冲突覆盖 = 一个原子 WriteBatch（Delete 与 Put 在同一条 WAL record 里），而不是
//      ftruncate + write 两次动作；
//   ② 中间空洞 / 前缀丢失 = 拒绝启动（lsm 的 WAL 保证下空洞只可能是介质级损坏）；
//   ③ 写失败 = 粘性毒化（与 lsm 的 fail-stop 一致），绝不在失败后用内存态撒谎。
//
// 锁纪律（与 FileLogStore 的 mu_ 同形）：
//   * mu_ 保护**全部**内存状态（term_/terms_/lastIndex_/...）与 DB 的结构性变更；
//   * sync() **不持 mu_**（它只在 lsm 内部串行），因此 fsync 不会挡 appendNoSync；
//   * 持 mu_ 时**不得**调用公共访问器（lastIndex()/firstIndex() 等会再次加锁）——
//     内部一律用 *Locked 版本。
//
// 未配置 lsm（RAFTK_HAVE_LSM 未定义）时：每个方法都显式失败（ctor 抛异常、其余 return false），
// **绝不**退化成别的引擎。
#include "raft/lsm_log_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "raft/log_entry_codec.h"

#ifdef RAFTK_HAVE_LSM
#include "db.h"
#include "db_impl.h"   // RecoveryStats（尾部截断字节数，设计 §2.5 / §5.2 O8）
#include "write_batch.h"
#endif

namespace raftkv::raft {

namespace {

constexpr const char* kNoLsmReason =
    "LsmLogStore: this build was configured without lsm support "
    "(no lsm source tree at configure time; re-run cmake with "
    "-DRAFTKV_LSM_DIR=<pinned lsm tree>)";

#ifdef RAFTK_HAVE_LSM
// 分块上界：lsm 的 WriteBatch 上限是 count <= 1<<20 且 ByteSize <= 64 MiB
// （write_batch.h L31-32）。raft 的常规批远小于此，但 append()/compact() 可能被
// 外部以大区间调用 ⇒ 必须分块，否则 Validate() 直接拒绝。
constexpr size_t kMaxOpsPerBatch = 4096;
constexpr size_t kMaxBytesPerBatch = 8u * 1024u * 1024u;

lsm::Slice LsmSliceOf(const std::string& s) {
  return lsm::Slice(s.data(), s.size());
}
#endif

}  // namespace

bool LsmLogStore::haveLsm() {
#ifdef RAFTK_HAVE_LSM
  return true;
#else
  return false;
#endif
}

const char* LsmLogStore::unavailableReason() { return kNoLsmReason; }

// ---- 键编码（设计 §2.1）-----------------------------------------------------

std::string LsmLogStore::entryKey(Index index) {
  std::string k;
  k.resize(kEntryKeyLen);
  k[0] = static_cast<char>(kLogPrefix);
  for (int i = 0; i < 8; ++i) {
    k[1 + i] = static_cast<char>((index >> ((7 - i) * 8)) & 0xff);
  }
  return k;
}

bool LsmLogStore::isLogKey(const std::string& key) {
  return key.size() == kEntryKeyLen &&
         static_cast<uint8_t>(key[0]) == kLogPrefix;
}

bool LsmLogStore::decodeEntryKey(const std::string& key, Index* index) {
  if (index == nullptr || !isLogKey(key)) return false;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | static_cast<uint8_t>(key[1 + i]);
  }
  *index = v;
  return true;
}

// ---- 生命周期 ---------------------------------------------------------------

#ifdef RAFTK_HAVE_LSM
void LsmLogStore::LsmDbDeleter::operator()(lsm::DB* db) const noexcept {
  if (db == nullptr) return;
  // Close 隐含 Sync（lsm I20/A30）；析构期失败无上报通道，只能吞掉。
  (void)db->Close();
  delete db;
}
#else
void LsmLogStore::LsmDbDeleter::operator()(lsm::DB* db) const noexcept {
  // 未配置 lsm 时 db_ 恒为 nullptr（ctor 在 Open 之前就抛异常）。
  (void)db;
}
#endif

LsmLogStore::LsmLogStore(std::string dir) : dir_(std::move(dir)) {
#ifndef RAFTK_HAVE_LSM
  throw std::runtime_error(kNoLsmReason);
#else
  // 设计 §2.4.1：目录 <dir>/raft-lsm（与 FileLogStore 的 <dir>/raft 物理隔离）。
  const std::string sub = dir_ + "/raft-lsm";
  std::error_code ec;
  std::filesystem::create_directories(sub, ec);
  if (ec) {
    throw std::runtime_error("LsmLogStore: cannot create directory " + sub +
                             ": " + ec.message());
  }
  lsm::Options opts;
  // M6.10.4 N1 diagnostic knob: RAFTK_LSM_WRITE_BUFFER_BYTES overrides the
  // memtable size to exercise the flush/stall path in a bounded measurement.
  // Unset or <= 0 keeps the engine default (production behaviour unchanged).
  if (const char* wb = ::getenv("RAFTKV_LSM_WRITE_BUFFER_BYTES")) {
    const long long v = std::atoll(wb);
    if (v > 0) opts.write_buffer_size = static_cast<size_t>(v);
  }
  lsm::DB* raw = nullptr;
  const lsm::Status s = lsm::DB::Open(opts, sub, &raw);
  if (!s.ok()) {
    throw std::runtime_error("LsmLogStore: lsm DB::Open(" + sub +
                             ") failed: " + s.ToString());
  }
  db_.reset(raw);
  // O8/D8：把恢复期的尾部截断字节数记录下来（设计 §3.3 的对齐点 1）。
  if (auto* p = dynamic_cast<lsm::PersistentDBImpl*>(db_.get())) {
    stat_wal_tail_truncated_bytes_.store(p->GetRecoveryStats().tail_truncated_bytes);
  }
#endif
}

LsmLogStore::~LsmLogStore() { db_.reset(); }

// ---- load（设计 §2.4.1）-----------------------------------------------------

bool LsmLogStore::load(Term& term, int& votedFor, Index& lastIndex) {
#ifndef RAFTK_HAVE_LSM
  (void)term;
  (void)votedFor;
  (void)lastIndex;
  return false;
#else
  std::lock_guard<std::mutex> lk(mu_);
  if (db_ == nullptr) return false;
  stat_loads_.fetch_add(1);

  // 1. meta：NotFound 或长度/编码不符 ⇒ 退化为 kNoTerm/-1（与 FileLogStore 的坏 CRC 同构）；
  //    其他错误（IOError/Corruption）是不可恢复的 ⇒ 返回 false。
  term_ = kNoTerm;
  votedFor_ = -1;
  {
    std::string mv;
    const lsm::Status s = db_->Get(LsmSliceOf(metaKey()), &mv);
    if (!s.ok() && !s.IsNotFound()) return false;
    if (s.ok()) {
      Term t = kNoTerm;
      int v = -1;
      if (mv.size() == kMetaPayloadLen &&
          decodeMeta(reinterpret_cast<const Byte*>(mv.data()), mv.size(), t, v)) {
        term_ = t;
        votedFor_ = v;
      }
    }
  }

  // 2. 日志：只扫 [first, ...)，first = setBoundary 之后（RaftNode 在 load 之前调用）已知的边界。
  terms_.clear();
  const Index first = firstIndexLocked();
  lastIndex_ = lastIncluded_;
  lastTerm_ = (lastIncluded_ == kNoIndex) ? kNoTerm : lastIncludedTerm_;
  bool sawFirst = false;
  Index prev = kNoIndex;
  {
    std::unique_ptr<lsm::Iterator> it(db_->NewIterator());
    it->Seek(LsmSliceOf(entryKey(first)));
    for (; it->Valid(); it->Next()) {
      const lsm::Slice k = it->key();
      const std::string key(k.data(), k.size());
      if (!isLogKey(key)) break;  // 离开日志命名空间（meta key 或更大）
      Index idx = kNoIndex;
      if (!decodeEntryKey(key, &idx)) return false;
      if (idx < first) continue;  // 边界之下的残留：跳过（R3，对应 L219-222）
      LogEntry e;
      const lsm::Slice v = it->value();
      if (!decodeEntry(reinterpret_cast<const Byte*>(v.data()), v.size(), e)) {
        return false;  // value 解不开 ⇒ 介质级损坏
      }
      if (e.index != idx) return false;  // key/value 错配自校验（设计 §2.2 的理由②）
      if (!sawFirst) {
        // 一条高于边界的记录却没有它的前驱 ⇒ 被压缩掉的前缀不见了：拒绝启动，
        // 而不是把整段日志静默截掉（逐字保留 FileLogStore L223-228 的判据）。
        if (idx != first) return false;
        sawFirst = true;
      } else if (idx != prev + 1) {
        // 空洞：lsm 的 WAL 保证「一条 record = 一个原子批」且 Open 已判完尾部残骸 vs
        // 中间损坏（m2-design §5.3）⇒ 到这里还能看到空洞只可能是介质级损坏。设计 D5：拒绝。
        return false;
      }
      terms_.push_back(e.term);
      prev = idx;
    }
    if (!it->status().ok()) return false;
  }
  if (sawFirst) {
    lastIndex_ = prev;
    lastTerm_ = terms_.back();
  }
  // else：空日志 ⇒ lastIndex_ = lastIncluded_、lastTerm_ = lastIncludedTerm_（R5）。
  // 撕裂尾不需要我们处理：DB::Open 已在恢复期截断并记入 RecoveryStats（设计 §3.3 的对齐点 1）。
  if (auto* p = dynamic_cast<lsm::PersistentDBImpl*>(db_.get())) {
    stat_wal_tail_truncated_bytes_.store(p->GetRecoveryStats().tail_truncated_bytes);
  }

  term = term_;
  votedFor = votedFor_;
  lastIndex = lastIndex_;
  return true;
#endif
}

// ---- meta（设计 §2.4.2）-----------------------------------------------------

bool LsmLogStore::persistMeta(Term term, int votedFor) {
#ifndef RAFTK_HAVE_LSM
  (void)term;
  (void)votedFor;
  return false;
#else
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (poisoned_ || db_ == nullptr) return false;
  }
  // 单条 Put + Write{sync=true}：一条 WAL record = 一个原子批（I15）⇒ term/votedFor 成对原子；
  // 返回即 fsync 成功（m2-design §7.1 的三段式论证）⇒ 「durable-before-ack」成立。
  // 不持 mu_ 做 fsync：RaftNode 已用 metaPersistMu_ 串行化本方法（raft_node.cpp L443/L468）。
  const Bytes payload = encodeMeta(term, votedFor);
  lsm::WriteBatch wb;
  wb.Put(LsmSliceOf(metaKey()),
         lsm::Slice(reinterpret_cast<const char*>(payload.data()), payload.size()));
  lsm::WriteOptions wo;
  wo.sync = true;
  const lsm::Status s = db_->Write(wo, &wb);
  if (!s.ok()) {
    std::lock_guard<std::mutex> lk(mu_);
    poisoned_ = true;
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    term_ = term;
    votedFor_ = votedFor;
  }
  stat_meta_persists_.fetch_add(1);
  return true;
#endif
}

// ---- 追加（设计 §2.4.3/§2.4.4）---------------------------------------------

bool LsmLogStore::append(const std::vector<LogEntry>& entries) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!appendNoSyncLocked(entries)) return false;
  }
  // 与 FileLogStore::append（L275-282）逐字同构：先写、再在不持 mu_ 的情况下 flush。
  return sync();
}

bool LsmLogStore::appendNoSync(const std::vector<LogEntry>& entries) {
  std::lock_guard<std::mutex> lk(mu_);
  return appendNoSyncLocked(entries);
}

bool LsmLogStore::appendNoSyncLocked(const std::vector<LogEntry>& entries) {
#ifndef RAFTK_HAVE_LSM
  (void)entries;
  return false;
#else
  if (poisoned_ || db_ == nullptr) return false;
  if (entries.empty()) return true;
  stat_appends_.fetch_add(1);

  // ---- 阶段 1：规划（**不改任何内存状态、不碰 DB**）---------------------------
  // 任何失败都在这里原样返回 ⇒ 「校验失败不产生半应用状态」，比 FileLogStore 的
  // 边写边改（中途失败会把前半批留在文件里）更强。
  struct Op {
    bool del;
    Index index;
    std::string key;
    Bytes value;
    size_t bytes;
  };
  std::vector<Op> ops;
  ops.reserve(entries.size() + 8);
  size_t put_count = 0;

  Index simLast = lastIndex_;   // 模拟尾
  Index ovBase = kNoIndex;      // 覆盖层起点；kNoIndex = 还没有覆盖层（全部取自 terms_）
  std::vector<Term> ov;         // 覆盖层（[ovBase, simLast] 的 term）
  auto simTermAt = [&](Index i) -> Term {
    if (i == kNoIndex) return kNoTerm;
    if (i == lastIncluded_ && lastIncluded_ != kNoIndex) return lastIncludedTerm_;
    if (i < firstIndexLocked() || i > simLast) return kNoTerm;
    if (ovBase != kNoIndex && i >= ovBase) {
      return ov[static_cast<size_t>(i - ovBase)];
    }
    return terms_[static_cast<size_t>(i - firstIndexLocked())];
  };

  for (const LogEntry& e : entries) {
    if (e.index <= simLast) {
      if (simTermAt(e.index) == e.term) continue;  // 幂等：同 index 同 term 已在（L320-322）
      // 冲突覆盖：把 [e.index, simLast] 删掉，再追加（同一批里 Delete 先、Put 后）
      if (e.index == kNoIndex || e.index <= lastIncluded_) return false;  // 不能切到边界以下
      for (Index i = e.index; i <= simLast; ++i) {
        ops.push_back(Op{true, i, entryKey(i), Bytes(), kEntryKeyLen});
      }
      stat_truncates_.fetch_add(1);
      stat_truncated_entries_.fetch_add(simLast - e.index + 1);
      simLast = e.index - 1;
      if (ovBase != kNoIndex && e.index >= ovBase) {
        ov.resize(static_cast<size_t>(e.index - ovBase));
      } else {
        ov.clear();
        ovBase = e.index;
      }
    }
    if (e.index != simLast + 1) return false;  // 连续性（D3，L327）
    const Bytes payload = encodeEntry(e);
    ops.push_back(Op{false, e.index, entryKey(e.index), payload,
                     kEntryKeyLen + payload.size()});
    ++put_count;
    if (ovBase == kNoIndex) ovBase = e.index;
    ov.push_back(e.term);
    simLast = e.index;
  }
  if (ops.empty()) return true;

  // ---- 阶段 2：写（分块；失败即毒化）---------------------------------------
  for (size_t i = 0; i < ops.size();) {
    lsm::WriteBatch wb;
    size_t n = 0;
    size_t bytes = 0;
    for (; i < ops.size() && n < kMaxOpsPerBatch && bytes < kMaxBytesPerBatch;
         ++i, ++n) {
      const Op& op = ops[i];
      if (op.del) {
        wb.Delete(LsmSliceOf(op.key));
      } else {
        wb.Put(LsmSliceOf(op.key),
               lsm::Slice(reinterpret_cast<const char*>(op.value.data()),
                          op.value.size()));
      }
      bytes += op.bytes;
    }
    lsm::WriteOptions wo;  // sync = false：由调用方随后 sync()（组提交）
    const lsm::Status s = db_->Write(wo, &wb);
    if (!s.ok()) {
      poisoned_ = true;
      return false;
    }
  }

  // ---- 阶段 3：提交内存状态（DB 已返回 kOk，条目已对 slice/lastIndex 可见）----
  // 不变量：keep == ovBase - firstIndexLocked() <= terms_.size()（ovBase 只可能是
  // lastIndex_+1 或某个冲突索引）。
  if (ovBase != kNoIndex) {
    const size_t keep = static_cast<size_t>(ovBase - firstIndexLocked());
    if (keep < terms_.size()) terms_.resize(keep);
    terms_.insert(terms_.end(), ov.begin(), ov.end());
  }
  lastIndex_ = simLast;
  lastTerm_ = (simLast == lastIncluded_)
                  ? (lastIncluded_ == kNoIndex ? kNoTerm : lastIncludedTerm_)
                  : terms_.back();
  stat_appended_entries_.fetch_add(put_count);
  return true;
#endif
}

// ---- sync（设计 §2.4.5）----------------------------------------------------

bool LsmLogStore::sync() {
#ifndef RAFTK_HAVE_LSM
  return false;
#else
  if (db_ == nullptr) return false;
  stat_syncs_.fetch_add(1);
  // **不持 mu_**：lsm 的 DB::Sync() 自身线程安全，契约是「此前所有已返回 kOk 的写都
  // 已 durable」（db.h L54）。R1 在 lsm 内部（持 commit_mu_ 做 fsync，见设计 §6 R1 /
  // docs/m6-bench.md 的负结果节）——但那是 lsm 的行为，本实现不再叠加锁。
  const lsm::Status s = db_->Sync();
  if (!s.ok()) {
    std::lock_guard<std::mutex> lk(mu_);
    poisoned_ = true;
    return false;
  }
  return true;
#endif
}

// ---- 截断（设计 §2.4.6）----------------------------------------------------

bool LsmLogStore::truncateSuffix(Index fromIndex) {
  return truncateLocked(fromIndex, /*flush=*/true);
}

// ★ 必须覆写（log_store.h L48-49）：truncateSuffix() 会 fsync，所以 NoSync 变体不能
// 走基类的默认实现（那会退化成 fsync）。调用方必须在同一批里随后 sync()。
bool LsmLogStore::truncateSuffixNoSync(Index fromIndex) {
  return truncateLocked(fromIndex, /*flush=*/false);
}

bool LsmLogStore::deleteRangeLocked(Index from, Index to, bool sync) {
#ifndef RAFTK_HAVE_LSM
  (void)from;
  (void)to;
  (void)sync;
  return false;
#else
  for (Index i = from; i <= to;) {
    lsm::WriteBatch wb;
    size_t n = 0;
    for (; i <= to && n < kMaxOpsPerBatch; ++i, ++n) {
      wb.Delete(LsmSliceOf(entryKey(i)));
    }
    lsm::WriteOptions wo;
    wo.sync = sync;
    const lsm::Status s = db_->Write(wo, &wb);
    if (!s.ok()) {
      poisoned_ = true;
      return false;
    }
  }
  return true;
#endif
}

bool LsmLogStore::truncateLocked(Index fromIndex, bool flush) {
#ifndef RAFTK_HAVE_LSM
  (void)fromIndex;
  (void)flush;
  return false;
#else
  if (poisoned_ || db_ == nullptr) return false;
  if (fromIndex == kNoIndex) return true;          // E1: no-op
  if (fromIndex <= lastIncluded_) return false;    // E2: 不能切到边界以下
  if (fromIndex > lastIndex_ + 1) return false;    // E3: 越界
  if (fromIndex <= lastIndex_) {                   // E4: == lastIndex_+1 ⇒ 无条目可删，直接 true（不计入 truncates）
    stat_truncates_.fetch_add(1);                  // M6.10 N9：只统计**真的删了条目**的 truncate
    const size_t keep = static_cast<size_t>(fromIndex - firstIndexLocked());
    if (keep >= terms_.size()) return false;       // 内部不变量破坏（不应发生）
    if (!deleteRangeLocked(fromIndex, lastIndex_, flush)) return false;  // E5/E6/E7
    stat_truncated_entries_.fetch_add(terms_.size() - keep);
    terms_.resize(keep);                                                 // E8
    lastIndex_ = fromIndex - 1;
    lastTerm_ = (lastIndex_ == lastIncluded_)
                    ? (lastIncluded_ == kNoIndex ? kNoTerm : lastIncludedTerm_)
                    : terms_.back();
  }
  return true;
#endif
}

// ---- slice（设计 §2.4.7）---------------------------------------------------

std::vector<LogEntry> LsmLogStore::slice(Index from, size_t maxEntries,
                                         size_t maxBytes) const {
#ifndef RAFTK_HAVE_LSM
  (void)from;
  (void)maxEntries;
  (void)maxBytes;
  return {};
#else
  std::lock_guard<std::mutex> lk(mu_);
  stat_slice_calls_.fetch_add(1);
  std::vector<LogEntry> out;
  if (db_ == nullptr) return out;
  Index start = from;
  if (start < firstIndexLocked()) start = firstIndexLocked();  // clamp（D3，L399）
  if (start > lastIndex_ || maxEntries == 0) return out;
  size_t bytes = 0;
  std::unique_ptr<lsm::Iterator> it(db_->NewIterator());  // 创建时刻的一致快照
  it->Seek(LsmSliceOf(entryKey(start)));
  while (it->Valid()) {
    if (out.size() >= maxEntries) break;
    const lsm::Slice k = it->key();
    if (k.size() != kEntryKeyLen ||
        static_cast<uint8_t>(k.data()[0]) != kLogPrefix) {
      break;  // 离开日志命名空间（meta key / 更大）
    }
    const std::string key(k.data(), k.size());
    Index idx = kNoIndex;
    if (!decodeEntryKey(key, &idx)) {
      stat_slice_decode_errors_.fetch_add(1);
      break;
    }
    if (idx < start) {  // 防御：Seek 已定位到 start，理论上不会命中
      it->Next();
      continue;
    }
    if (idx > lastIndex_) break;  // 不能靠「迭代器耗尽」终止（DB 里可能有更多 key）
    LogEntry e;
    const lsm::Slice v = it->value();
    if (!decodeEntry(reinterpret_cast<const Byte*>(v.data()), v.size(), e) ||
        e.index != idx) {
      stat_slice_decode_errors_.fetch_add(1);
      break;  // 只读路径没有错误通道：停下并计数（设计 §2.5 的计数纪律）
    }
    const size_t sz = e.key.size() + e.value.size();  // 与 L403 **逐字相同**的口径
    if (!out.empty() && bytes + sz > maxBytes) break;  // 至少返回 1 条（L404）
    out.push_back(std::move(e));
    bytes += sz;
    it->Next();
  }
  return out;
#endif
}

std::vector<LogEntry> LsmLogStore::all() const {
  return slice(kNoIndex, std::numeric_limits<size_t>::max(),
               std::numeric_limits<size_t>::max());
}

// ---- 只读访问器（设计 §2.4.8）----------------------------------------------

Index LsmLogStore::lastIndex() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastIndex_;
}

Term LsmLogStore::lastTerm() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastTerm_;
}

Term LsmLogStore::termAtLocked(Index index) const {
  if (index == kNoIndex) return kNoTerm;
  if (index == lastIncluded_ && lastIncluded_ != kNoIndex) {
    return lastIncludedTerm_;  // 边界条目（D3）
  }
  if (index < firstIndexLocked() || index > lastIndex_) return kNoTerm;
  return terms_[static_cast<size_t>(index - firstIndexLocked())];
}

Term LsmLogStore::termAt(Index index) const {
  std::lock_guard<std::mutex> lk(mu_);
  return termAtLocked(index);
}

Index LsmLogStore::firstIndex() const {
  std::lock_guard<std::mutex> lk(mu_);
  return firstIndexLocked();
}

Index LsmLogStore::lastIncludedIndex() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastIncluded_;
}

Term LsmLogStore::lastIncludedTerm() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastIncludedTerm_;
}

// ---- 边界与压缩（设计 §2.4.9）----------------------------------------------

void LsmLogStore::setBoundary(Index lastIncludedIndex, Term lastIncludedTerm) {
  std::lock_guard<std::mutex> lk(mu_);
  // 必须在改边界**之前**算「当前 firstIndex」：设计 §2.4.9 的草案
  //   `while (drop < terms_.size() && (firstIndex() + drop) <= lastIncludedIndex)`
  // 在 lastIncluded_ 已被更新后调用 firstIndex()，会漏掉正好等于新边界的条目
  // （off-by-boundary）。这里按 FileLogStore L437-441 的语义（按绝对索引 + 旧 firstIndex）。
  const Index oldFirst = firstIndexLocked();
  lastIncluded_ = lastIncludedIndex;
  lastIncludedTerm_ = lastIncludedTerm;
  size_t drop = 0;
  while (drop < terms_.size() && (oldFirst + drop) <= lastIncludedIndex) ++drop;
  if (drop > 0) {
    terms_.erase(terms_.begin(), terms_.begin() + static_cast<ptrdiff_t>(drop));
  }
  // M6.10 N2：setBoundary 可能把边界**向后**移动（生产唯一调用点在构造期，故不可达；
  // 但 17 方法必须逐项等价）。FileLogStore 的 lastIndex() 完全由 entries_ 推导：
  //   entries_.empty() ? lastIncluded_ : entries_.back().index
  // 这里按同一推导重算 lastIndex_/lastTerm_。旧写法在「terms_ 已空 + 边界后移」时
  // 会把 lastIndex_ 停在旧值，termAt()/slice() 进而对空的 terms_ 越界索引（评审复现）。
  if (terms_.empty()) {
    lastIndex_ = lastIncluded_;
    // FileLogStore::lastTerm() returns kNoTerm when lastIncluded_ == kNoIndex
    // (even if a lastIncludedTerm was supplied) -- match it exactly.
    lastTerm_ = (lastIncluded_ == kNoIndex) ? kNoTerm : lastIncludedTerm_;
  } else {
    // 保留后缀 terms_[j] 的绝对索引 = oldFirst + drop + j（drop = 本次删掉的前缀条数）
    lastIndex_ = oldFirst + drop + static_cast<Index>(terms_.size()) - 1;
    lastTerm_ = terms_.back();
  }
}

bool LsmLogStore::compact(Index upTo, Term termAtUpTo) {
#ifndef RAFTK_HAVE_LSM
  (void)upTo;
  (void)termAtUpTo;
  return false;
#else
  std::lock_guard<std::mutex> lk(mu_);
  if (poisoned_ || db_ == nullptr) return false;
  if (upTo == kNoIndex) return true;         // L456
  if (upTo <= lastIncluded_) return true;    // L457: 已压缩，no-op
  stat_compacts_.fetch_add(1);
  // upTo 可以超过 lastIndex_（InstallSnapshot）⇒ 保留后缀为空（L458 注释）。
  const Index hi = std::min(upTo, lastIndex_);
  size_t drop = 0;
  if (hi >= firstIndexLocked()) {
    drop = static_cast<size_t>(hi - firstIndexLocked() + 1);
  }
  if (drop > terms_.size()) return false;    // 内部不变量破坏（不应发生）
  if (drop > 0) {
    if (!deleteRangeLocked(firstIndexLocked(), hi, /*sync=*/true)) return false;
    stat_compacted_entries_.fetch_add(drop);
    terms_.erase(terms_.begin(), terms_.begin() + static_cast<ptrdiff_t>(drop));
  }
  lastIncluded_ = upTo;
  lastIncludedTerm_ = termAtUpTo;
  if (lastIndex_ < lastIncluded_) lastIndex_ = lastIncluded_;
  lastTerm_ = (lastIndex_ == lastIncluded_) ? lastIncludedTerm_ : terms_.back();
  return true;
#endif
}

// ---- 诊断 -------------------------------------------------------------------

LsmLogStore::Stats LsmLogStore::stats() const {
  Stats s;
  s.loads = stat_loads_.load();
  s.appends = stat_appends_.load();
  s.appended_entries = stat_appended_entries_.load();
  s.syncs = stat_syncs_.load();
  s.truncates = stat_truncates_.load();
  s.truncated_entries = stat_truncated_entries_.load();
  s.compacts = stat_compacts_.load();
  s.compacted_entries = stat_compacted_entries_.load();
  s.meta_persists = stat_meta_persists_.load();
  s.slice_calls = stat_slice_calls_.load();
  s.slice_decode_errors = stat_slice_decode_errors_.load();
  s.wal_tail_truncated_bytes = stat_wal_tail_truncated_bytes_.load();
  {
    std::lock_guard<std::mutex> lk(mu_);
    s.poisoned = poisoned_;
  }
  return s;
}

uint64_t LsmLogStore::walTailTruncatedBytes() const {
  return stat_wal_tail_truncated_bytes_.load();
}

// M6.r2（O5/O6/O7）：把 lsm 引擎的内部计数渲染成一行 k=v，供 node 的 status 原样输出。
// 只用 lsm 的**公开诊断 API**（GetLevelStats/GetFlushStats/GetAmplificationStats），
// 不读内部字段、不做任何猜测：每个数字都是引擎自己维护的计数。
std::string LsmLogStore::engineStatsFragment() const {
#ifndef RAFTK_HAVE_LSM
  return {};
#else
  std::lock_guard<std::mutex> lk(mu_);
  auto* p = dynamic_cast<lsm::PersistentDBImpl*>(db_.get());
  if (p == nullptr) return {};
  const lsm::LevelStats lv = p->GetLevelStats();
  const lsm::FlushStats fl = p->GetFlushStats();
  const lsm::AmplificationStats am = p->GetAmplificationStats();
  uint64_t sst_files = 0;
  uint64_t sst_bytes = 0;
  for (int i = 0; i < lsm::kNumLevels; ++i) {
    sst_files += lv.files[i];
    sst_bytes += lv.bytes[i];
  }
  char buf[768];
  std::snprintf(
      buf, sizeof(buf),
      "lsm_l0_files=%llu lsm_sst_files=%llu lsm_sst_bytes=%llu lsm_wal_bytes=%llu "
      "lsm_flush_started=%llu lsm_flush_done=%llu lsm_flush_failed=%llu "
      "lsm_stall_events=%llu lsm_stall_ms=%llu lsm_wal_rotations=%llu "
      "lsm_compaction_rounds=%llu lsm_compaction_max_ms=%llu "
      "lsm_flush_write_bytes=%llu lsm_compact_write_bytes=%llu lsm_user_bytes=%llu",
      (unsigned long long)lv.files[0], (unsigned long long)sst_files,
      (unsigned long long)sst_bytes, (unsigned long long)am.log_bytes,
      (unsigned long long)fl.flushes_started, (unsigned long long)fl.flushes_completed,
      (unsigned long long)fl.flushes_failed, (unsigned long long)fl.stall_events,
      (unsigned long long)(fl.stall_micros / 1000ULL),
      (unsigned long long)fl.rotations,
      (unsigned long long)am.compaction_rounds,
      (unsigned long long)(am.compaction_round_max_us / 1000ULL),
      (unsigned long long)am.flush_write_bytes,
      (unsigned long long)am.compact_write_bytes,
      (unsigned long long)am.user_logical_bytes);
  return std::string(buf);
#endif
}

}  // namespace raftkv::raft
