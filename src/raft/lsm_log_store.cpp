// LsmLogStore 实现（M6）。规格：docs/m6-design.md §2/§3。
//
// M6.1 只交付**构建接线 + 引擎开关**：本文件此时是骨架（ctor/dtor 与键编码助手完整，
// 其余方法显式抛 std::logic_error("not implemented (M6.2)")）。语义实现与单测在 M6.2。
#include "raft/lsm_log_store.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "raft/log_entry_codec.h"

#ifdef RAFTK_HAVE_LSM
#include "db.h"
#include "db_impl.h"   // RecoveryStats（尾部截断字节数，设计 §2.5/§5.2 O8）
#include "write_batch.h"
#endif

namespace raftkv::raft {

namespace {
constexpr const char* kNoLsmReason =
    "LsmLogStore: this build was configured without lsm support "
    "(no lsm source tree at configure time; re-run cmake with "
    "-DRAFTKV_LSM_DIR=<pinned lsm tree>)";
}  // namespace

bool LsmLogStore::haveLsm() {
#ifdef RAFTK_HAVE_LSM
  return true;
#else
  return false;
#endif
}

const char* LsmLogStore::unavailableReason() { return kNoLsmReason; }

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
  // Close 隐含 Sync（lsm I20/A30）；失败无上报通道，析构期只能吞掉。
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
  lsm::DB* raw = nullptr;
  const lsm::Status s = lsm::DB::Open(opts, sub, &raw);
  if (!s.ok()) {
    throw std::runtime_error("LsmLogStore: lsm DB::Open(" + sub +
                             ") failed: " + s.ToString());
  }
  db_.reset(raw);
#endif
}

LsmLogStore::~LsmLogStore() { db_.reset(); }

// ---- M6.1 骨架：以下方法在 M6.2 落地 ---------------------------------------

#define M6_NOT_IMPLEMENTED(name)                                        \
  throw std::logic_error("LsmLogStore::" name ": not implemented (M6.2)")

bool LsmLogStore::load(Term&, int&, Index&) { M6_NOT_IMPLEMENTED("load"); }
bool LsmLogStore::persistMeta(Term, int) { M6_NOT_IMPLEMENTED("persistMeta"); }
bool LsmLogStore::append(const std::vector<LogEntry>&) {
  M6_NOT_IMPLEMENTED("append");
}
bool LsmLogStore::appendNoSync(const std::vector<LogEntry>&) {
  M6_NOT_IMPLEMENTED("appendNoSync");
}
bool LsmLogStore::sync() { M6_NOT_IMPLEMENTED("sync"); }
bool LsmLogStore::truncateSuffix(Index) { M6_NOT_IMPLEMENTED("truncateSuffix"); }
bool LsmLogStore::truncateSuffixNoSync(Index) {
  M6_NOT_IMPLEMENTED("truncateSuffixNoSync");
}
std::vector<LogEntry> LsmLogStore::slice(Index, size_t, size_t) const {
  M6_NOT_IMPLEMENTED("slice");
}
Index LsmLogStore::lastIndex() const { M6_NOT_IMPLEMENTED("lastIndex"); }
Term LsmLogStore::lastTerm() const { M6_NOT_IMPLEMENTED("lastTerm"); }
Term LsmLogStore::termAt(Index) const { M6_NOT_IMPLEMENTED("termAt"); }
// setBoundary 是**唯一**必须在 load() 之前成功返回的方法（RaftNode ctor 的 D3 顺序），
// 因此骨架期它是 no-op，让 --log-engine=lsm 的失败点稳定落在 load()（M6.1 判据）。
void LsmLogStore::setBoundary(Index, Term) {}
bool LsmLogStore::compact(Index, Term) { M6_NOT_IMPLEMENTED("compact"); }
Index LsmLogStore::firstIndex() const { M6_NOT_IMPLEMENTED("firstIndex"); }
Index LsmLogStore::lastIncludedIndex() const {
  M6_NOT_IMPLEMENTED("lastIncludedIndex");
}
Term LsmLogStore::lastIncludedTerm() const {
  M6_NOT_IMPLEMENTED("lastIncludedTerm");
}
std::vector<LogEntry> LsmLogStore::all() const { M6_NOT_IMPLEMENTED("all"); }
LsmLogStore::Stats LsmLogStore::stats() const {
  M6_NOT_IMPLEMENTED("stats");
}
uint64_t LsmLogStore::walTailTruncatedBytes() const {
  M6_NOT_IMPLEMENTED("walTailTruncatedBytes");
}
bool LsmLogStore::appendNoSyncLocked(const std::vector<LogEntry>&) {
  M6_NOT_IMPLEMENTED("appendNoSyncLocked");
}
bool LsmLogStore::truncateLocked(Index, bool) {
  M6_NOT_IMPLEMENTED("truncateLocked");
}
Term LsmLogStore::termAtLocked(Index) const { M6_NOT_IMPLEMENTED("termAtLocked"); }

#undef M6_NOT_IMPLEMENTED

}  // namespace raftkv::raft
