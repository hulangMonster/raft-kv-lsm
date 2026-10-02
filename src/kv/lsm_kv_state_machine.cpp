
#include "kv/lsm_kv_state_machine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kv/kv_snapshot_view.h"

#ifdef RAFTK_HAVE_LSM
#include "db.h"
#include "write_batch.h"
#endif

namespace raftkv {

namespace {

constexpr const char* kNoLsmReason =
    "LsmKvStateMachine: this build was configured without lsm support "
    "(no lsm source tree at configure time; re-run cmake with "
    "-DRAFTKV_LSM_DIR=<pinned lsm tree>)";
constexpr const char* kPoisonedReason =
    "LsmKvStateMachine: poisoned by an earlier failed lsm write";

#ifdef RAFTK_HAVE_LSM

// ---- on-disk key layout (design 10.15; internal, not a wire format) -------
//   ptrKey()              -> BE64(ns)         current committed namespace {0,1}
//   pendKey()             -> BE64(targetNs)   present only while restore stages
//   nsByte(ns) | 0x00     -> BE64(lastApplied)
//   nsByte(ns) | 0x01 | userKey                -> inline value (<= 1 MiB)
//   nsByte(ns) | 0x02 | BE64(cid)              -> BE64(rid)
//   nsByte(ns) | 0x04 | userKey                -> BE64(vid) BE64(len) BE32(n)
//   nsByte(ns) | 0x03 | userKey | BE64(vid) | BE32(i) -> chunk i
constexpr uint8_t kNsByte0 = 0x11;
constexpr uint8_t kNsByte1 = 0x12;
constexpr size_t kInlineValueMax = 1u << 20;  // 1 MiB
constexpr size_t kValueChunkSize = 4u << 20;  // 4 MiB
constexpr size_t kMaxOpsPerBatch = 4096;
constexpr size_t kMaxBytesPerBatch = 8u << 20;

lsm::Slice Sl(const std::string& s) { return lsm::Slice(s.data(), s.size()); }

std::string be64str(uint64_t v) {
  std::string s;
  s.resize(8);
  for (int i = 0; i < 8; ++i) {
    s[static_cast<size_t>(i)] = static_cast<char>((v >> ((7 - i) * 8)) & 0xff);
  }
  return s;
}
std::string be32str(uint32_t v) {
  std::string s;
  s.resize(4);
  for (int i = 0; i < 4; ++i) {
    s[static_cast<size_t>(i)] = static_cast<char>((v >> ((3 - i) * 8)) & 0xff);
  }
  return s;
}
uint64_t readBe64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(p[i]);
  return v;
}
uint32_t readBe32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<uint8_t>(p[i]);
  return v;
}

uint8_t nsByte(uint8_t ns) { return ns == 0 ? kNsByte0 : kNsByte1; }

std::string ptrKey() {
  std::string k;
  k.push_back(static_cast<char>(0));
  k.append("ns");
  return k;
}
std::string pendKey() {
  std::string k;
  k.push_back(static_cast<char>(0));
  k.append("pending");
  return k;
}
std::string keyApplied(uint8_t ns) {
  std::string k;
  k.push_back(static_cast<char>(nsByte(ns)));
  k.push_back(static_cast<char>(0x00));
  return k;
}
std::string keyInline(uint8_t ns, const std::string& uk) {
  std::string k;
  k.push_back(static_cast<char>(nsByte(ns)));
  k.push_back(static_cast<char>(0x01));
  k.append(uk);
  return k;
}
std::string keyDedup(uint8_t ns, uint64_t cid) {
  std::string k;
  k.push_back(static_cast<char>(nsByte(ns)));
  k.push_back(static_cast<char>(0x02));
  k.append(be64str(cid));
  return k;
}
std::string keyHeader(uint8_t ns, const std::string& uk) {
  std::string k;
  k.push_back(static_cast<char>(nsByte(ns)));
  k.push_back(static_cast<char>(0x04));
  k.append(uk);
  return k;
}
std::string keyChunk(uint8_t ns, const std::string& uk, uint64_t vid, uint32_t idx) {
  std::string k;
  k.push_back(static_cast<char>(nsByte(ns)));
  k.push_back(static_cast<char>(0x03));
  k.append(uk);
  k.append(be64str(vid));
  k.append(be32str(idx));
  return k;
}

size_t chunkCountFor(size_t len) {
  return len == 0 ? 0 : (len + kValueChunkSize - 1) / kValueChunkSize;
}
std::string headerValue(uint64_t vid, size_t total) {
  return be64str(vid) + be64str(static_cast<uint64_t>(total)) +
         be32str(static_cast<uint32_t>(chunkCountFor(total)));
}

void WriteOrThrow(lsm::DB* db, lsm::WriteBatch& wb) {
  lsm::WriteOptions wo;
  wo.sync = false;
  const lsm::Status s = db->Write(wo, &wb);
  if (!s.ok()) {
    throw std::runtime_error("LsmKvStateMachine: DB::Write failed: " + s.ToString());
  }
}

// Bounded WriteBatch accumulator: flushes at kMaxOpsPerBatch / kMaxBytesPerBatch.
class BatchPump {
 public:
  explicit BatchPump(lsm::DB* db) : db_(db) {}
  void SetAfterFlush(std::function<void(size_t)> fn) { afterFlush_ = std::move(fn); }
  void Put(const std::string& k, const std::string& v) {
    MaybeFlush(k.size() + v.size());
    wb_.Put(Sl(k), Sl(v));
    bytes_ += k.size() + v.size();
    ops_ += 1;
  }
  void Delete(const std::string& k) {
    MaybeFlush(k.size());
    wb_.Delete(Sl(k));
    bytes_ += k.size();
    ops_ += 1;
  }
  void Flush() {
    if (ops_ == 0) return;
    lsm::WriteOptions wo;
    wo.sync = false;
    const lsm::Status s = db_->Write(wo, &wb_);
    if (!s.ok()) {
      throw std::runtime_error("LsmKvStateMachine: staged lsm write failed: " + s.ToString());
    }
    wb_.Clear();
    bytes_ = 0;
    ops_ = 0;
    ++flushes_;
    if (afterFlush_) afterFlush_(flushes_);
  }
  size_t flushes() const { return flushes_; }

 private:
  void MaybeFlush(size_t add) {
    if (ops_ > 0 && (ops_ + 1 > kMaxOpsPerBatch || bytes_ + add + 64 > kMaxBytesPerBatch)) {
      Flush();
    }
  }
  lsm::DB* db_;
  lsm::WriteBatch wb_;
  size_t bytes_ = 0;
  size_t ops_ = 0;
  size_t flushes_ = 0;
  std::function<void(size_t)> afterFlush_;
};

// Stage a large value's chunks (invisible to readers until its header commits).
void PumpValueChunks(BatchPump& pump, uint8_t ns, const std::string& uk,
                     const std::string& value, uint64_t vid) {
  const size_t n = chunkCountFor(value.size());
  for (size_t i = 0; i < n; ++i) {
    const size_t off = i * kValueChunkSize;
    const size_t len = std::min(kValueChunkSize, value.size() - off);
    pump.Put(keyChunk(ns, uk, vid, static_cast<uint32_t>(i)), value.substr(off, len));
  }
}

void ClearNamespace(lsm::DB* db, uint8_t ns) {
  const uint8_t nb = nsByte(ns);
  std::vector<std::string> keys;
  {
    std::unique_ptr<lsm::Iterator> it(db->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const lsm::Slice k = it->key();
      if (k.size() >= 1 && static_cast<uint8_t>(k.data()[0]) == nb) {
        keys.emplace_back(k.data(), k.size());
      }
    }
  }
  if (keys.empty()) return;
  BatchPump pump(db);
  for (const auto& k : keys) pump.Delete(k);
  pump.Flush();
}

void DeleteValueChunks(lsm::DB* db, uint8_t ns, const std::string& uk,
                       size_t total, uint64_t vid) {
  if (total <= kInlineValueMax) return;
  BatchPump pump(db);
  const size_t n = chunkCountFor(total);
  for (size_t i = 0; i < n; ++i) {
    pump.Delete(keyChunk(ns, uk, vid, static_cast<uint32_t>(i)));
  }
  pump.Flush();
}

#endif  // RAFTK_HAVE_LSM

}  // namespace

bool LsmKvStateMachine::haveLsm() {
#ifdef RAFTK_HAVE_LSM
  return true;
#else
  return false;
#endif
}

const char* LsmKvStateMachine::unavailableReason() { return kNoLsmReason; }

#ifdef RAFTK_HAVE_LSM
void LsmKvStateMachine::LsmDbDeleter::operator()(lsm::DB* db) const noexcept {
  if (db == nullptr) return;
  (void)db->Close();  // Close implies Sync (lsm I20/A30)
  delete db;
}
#else
void LsmKvStateMachine::LsmDbDeleter::operator()(lsm::DB* db) const noexcept {
  (void)db;
}
#endif

LsmKvStateMachine::LsmKvStateMachine(std::string dir) : dir_(std::move(dir)) {
#ifndef RAFTK_HAVE_LSM
  throw std::runtime_error(kNoLsmReason);
#else
  const std::string sub = dir_ + "/kv-lsm";
  std::error_code ec;
  std::filesystem::create_directories(sub, ec);
  if (ec) {
    throw std::runtime_error("LsmKvStateMachine: cannot create directory " + sub +
                             ": " + ec.message());
  }
  lsm::Options opts;
  lsm::DB* raw = nullptr;
  const lsm::Status s = lsm::DB::Open(opts, sub, &raw);
  if (!s.ok()) {
    throw std::runtime_error("LsmKvStateMachine: lsm DB::Open(" + sub +
                             ") failed: " + s.ToString());
  }
  db_.reset(raw);
  rebuildMirror();
  gcNonCurrentNamespace();
#endif
}

LsmKvStateMachine::~LsmKvStateMachine() { db_.reset(); }

#ifdef RAFTK_HAVE_LSM
void LsmKvStateMachine::rebuildMirror() {
  data_.clear();
  lastRequest_.clear();
  valueIds_.clear();
  lastApplied_ = raft::kNoIndex;
  nextValueId_ = 1;

  {
    std::string pv;
    const lsm::Status s = db_->Get(Sl(ptrKey()), &pv);
    ns_ = (s.ok() && pv.size() == 8 && readBe64(pv.data()) == 1) ? 1 : 0;
  }

  const uint8_t nb = nsByte(ns_);
  std::unordered_map<std::string, std::string> headers;
  std::unordered_map<std::string, std::string> chunks;
  uint64_t maxVid = 0;

  {
    std::unique_ptr<lsm::Iterator> it(db_->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const lsm::Slice k = it->key();
      const lsm::Slice v = it->value();
      if (k.size() < 2 || static_cast<uint8_t>(k.data()[0]) != nb) continue;
      const uint8_t type = static_cast<uint8_t>(k.data()[1]);
      if (type == 0x00) {
        if (k.size() == 2 && v.size() == 8) lastApplied_ = readBe64(v.data());
      } else if (type == 0x01) {
        data_[std::string(k.data() + 2, k.size() - 2)] = std::string(v.data(), v.size());
      } else if (type == 0x02) {
        if (k.size() == 10 && v.size() == 8) {
          lastRequest_[readBe64(k.data() + 2)] = readBe64(v.data());
        }
      } else if (type == 0x04) {
        if (v.size() == 20) {
          headers[std::string(k.data() + 2, k.size() - 2)] = std::string(v.data(), v.size());
        }
      } else if (type == 0x03) {
        if (k.size() < 2 + 12) continue;
        const std::string uk(k.data() + 2, k.size() - 2 - 12);
        const uint64_t vid = readBe64(k.data() + k.size() - 12);
        const uint32_t idx = readBe32(k.data() + k.size() - 4);
        chunks[be64str(vid) + be32str(idx) + uk] = std::string(v.data(), v.size());
        if (vid > maxVid) maxVid = vid;
      }
    }
  }

  for (const auto& h : headers) {
    const std::string& uk = h.first;
    const uint64_t vid = readBe64(h.second.data());
    const uint64_t total = readBe64(h.second.data() + 8);
    const uint32_t cnt = readBe32(h.second.data() + 16);
    std::string val;
    val.reserve(static_cast<size_t>(total));
    bool ok = true;
    for (uint32_t i = 0; i < cnt; ++i) {
      const auto cit = chunks.find(be64str(vid) + be32str(i) + uk);
      if (cit == chunks.end()) {
        ok = false;
        break;
      }
      val += cit->second;
    }
    if (ok && val.size() == total) {
      data_[uk] = std::move(val);
      valueIds_[uk] = vid;
    }
  }
  nextValueId_ = maxVid + 1;
}

void LsmKvStateMachine::gcNonCurrentNamespace() {
  const uint8_t nb = nsByte(ns_);
  const std::string pk = ptrKey();
  std::vector<std::string> stale;
  {
    std::unique_ptr<lsm::Iterator> it(db_->NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const lsm::Slice k = it->key();
      const std::string key(k.data(), k.size());
      if (key == pk) continue;
      const bool keep = (k.size() >= 1 && static_cast<uint8_t>(k.data()[0]) == nb);
      if (!keep) stale.push_back(key);
    }
  }
  if (stale.empty()) return;
  BatchPump pump(db_.get());
  for (const auto& k : stale) pump.Delete(k);
  pump.Flush();
}
#else
void LsmKvStateMachine::rebuildMirror() {}
void LsmKvStateMachine::gcNonCurrentNamespace() {}
#endif

void LsmKvStateMachine::apply(const raft::LogEntry& e) {
#ifndef RAFTK_HAVE_LSM
  (void)e;
  throw std::runtime_error(kNoLsmReason);
#else
  if (poisoned_) throw std::runtime_error(kPoisonedReason);
  try {
    if (e.op == OpCode::kGet || e.op == OpCode::kConfig) {
      lsm::WriteBatch wb;
      wb.Put(Sl(keyApplied(ns_)), Sl(be64str(e.index)));
      WriteOrThrow(db_.get(), wb);
      lastApplied_ = e.index;
      return;
    }
    const auto it = lastRequest_.find(e.clientId);
    if (it != lastRequest_.end() && e.requestId <= it->second) {
      return;  // m2-design 6.5: discarded; nothing changes (mem baseline)
    }

    const bool isPut = (e.op == OpCode::kPut);
    const bool isDel = (e.op == OpCode::kDel);
    bool hasOld = false;
    std::string oldValue;
    uint64_t oldVid = 0;
    if (isPut || isDel) {
      const auto dit = data_.find(e.key);
      if (dit != data_.end()) {
        hasOld = true;
        oldValue = dit->second;
        const auto vit = valueIds_.find(e.key);
        if (vit != valueIds_.end()) oldVid = vit->second;
      }
    }
    const bool oldChunked = hasOld && oldValue.size() > kInlineValueMax;

    uint64_t newVid = 0;
    if (isPut && e.value.size() > kInlineValueMax) {
      newVid = nextValueId_++;
      BatchPump pump(db_.get());
      PumpValueChunks(pump, ns_, e.key, e.value, newVid);
      pump.Flush();  // chunks stay invisible until the header commits
    }

    lsm::WriteBatch wb;
    if (isPut) {
      if (e.value.size() <= kInlineValueMax) {
        wb.Put(Sl(keyInline(ns_, e.key)), Sl(e.value));
        if (oldChunked) wb.Delete(Sl(keyHeader(ns_, e.key)));
      } else {
        if (hasOld && !oldChunked) wb.Delete(Sl(keyInline(ns_, e.key)));
        wb.Put(Sl(keyHeader(ns_, e.key)), Sl(headerValue(newVid, e.value.size())));
      }
    } else if (isDel) {
      if (hasOld) {
        if (oldChunked) wb.Delete(Sl(keyHeader(ns_, e.key)));
        else wb.Delete(Sl(keyInline(ns_, e.key)));
      }
    }
    wb.Put(Sl(keyDedup(ns_, e.clientId)), Sl(be64str(e.requestId)));
    wb.Put(Sl(keyApplied(ns_)), Sl(be64str(e.index)));
    WriteOrThrow(db_.get(), wb);

    if (isPut) {
      data_[e.key] = e.value;
      if (e.value.size() > kInlineValueMax) valueIds_[e.key] = newVid;
      else valueIds_.erase(e.key);
    } else if (isDel) {
      data_.erase(e.key);
      valueIds_.erase(e.key);
    }
    lastRequest_[e.clientId] = e.requestId;
    lastApplied_ = e.index;

    if (oldChunked) {
      try {
        DeleteValueChunks(db_.get(), ns_, e.key, oldValue.size(), oldVid);
      } catch (...) {
        // orphan chunks are invisible (no header) and reclaimed at startup/restore
      }
    }
  } catch (...) {
    poisoned_ = true;
    throw;
  }
#endif
}

bool LsmKvStateMachine::get(const std::string& key, std::string& out) const {
  const auto it = data_.find(key);
  if (it == data_.end()) return false;
  out = it->second;
  return true;
}

raft::Index LsmKvStateMachine::lastApplied() const { return lastApplied_; }

std::shared_ptr<const raft::SnapshotView> LsmKvStateMachine::snapshotView() const {
  std::vector<std::pair<std::string, std::string>> kv(data_.begin(), data_.end());
  std::vector<std::pair<uint64_t, uint64_t>> dedup(lastRequest_.begin(),
                                                   lastRequest_.end());
  return std::make_shared<kvview::View>(std::move(kv), std::move(dedup),
                                        lastApplied_);
}

bool LsmKvStateMachine::restore(const Bytes& payload) {
  std::unordered_map<std::string, std::string> data;
  std::unordered_map<uint64_t, uint64_t> dedup;
  raft::Index la = raft::kNoIndex;
  if (!kvview::decode(payload, data, dedup, la)) return false;
#ifndef RAFTK_HAVE_LSM
  return false;
#else
  if (db_ == nullptr) return false;
  if (poisoned_) return false;

  const uint8_t target = static_cast<uint8_t>(ns_ ^ 1);
  try {
    ClearNamespace(db_.get(), target);
    {
      lsm::WriteBatch wb;
      wb.Put(Sl(pendKey()), Sl(be64str(target)));
      WriteOrThrow(db_.get(), wb);
    }
    std::unordered_map<std::string, uint64_t> newValueIds;
    BatchPump pump(db_.get());
    if (restoreHook_) {
      pump.SetAfterFlush([this](size_t n) { restoreHook_(n); });
    }
    for (const auto& kv : data) {
      if (kv.second.size() <= kInlineValueMax) {
        pump.Put(keyInline(target, kv.first), kv.second);
      } else {
        const uint64_t vid = nextValueId_++;
        newValueIds[kv.first] = vid;
        PumpValueChunks(pump, target, kv.first, kv.second, vid);
        pump.Put(keyHeader(target, kv.first), headerValue(vid, kv.second.size()));
      }
    }
    for (const auto& d : dedup) {
      pump.Put(keyDedup(target, d.first), be64str(d.second));
    }
    pump.Flush();
    const uint64_t staged = pump.flushes();

    {
      lsm::WriteBatch wb;
      wb.Put(Sl(keyApplied(target)), Sl(be64str(la)));
      wb.Put(Sl(ptrKey()), Sl(be64str(target)));
      wb.Delete(Sl(pendKey()));
      WriteOrThrow(db_.get(), wb);
    }
    data_ = std::move(data);
    lastRequest_ = std::move(dedup);
    valueIds_ = std::move(newValueIds);
    lastApplied_ = la;
    ns_ = target;
    stagedBatches_ += staged;
    ++restoreCount_;
    try {
      ClearNamespace(db_.get(), static_cast<uint8_t>(ns_ ^ 1));
    } catch (...) {
    }
    return true;
  } catch (...) {
    try {
      lsm::WriteBatch wb;
      wb.Delete(Sl(pendKey()));
      WriteOrThrow(db_.get(), wb);
    } catch (...) {
    }
    return false;
  }
#endif
}

bool LsmKvStateMachine::sync() {
#ifdef RAFTK_HAVE_LSM
  if (db_ == nullptr) return false;
  return db_->Sync().ok();
#else
  return false;
#endif
}

}  // namespace raftkv
