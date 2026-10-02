
#include "kv/lsm_kv_state_machine.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
lsm::Slice Sl(const std::string& s) { return lsm::Slice(s.data(), s.size()); }

std::string be64str(uint64_t v) {
  std::string s;
  s.resize(8);
  for (int i = 0; i < 8; ++i) {
    s[static_cast<size_t>(i)] = static_cast<char>((v >> ((7 - i) * 8)) & 0xff);
  }
  return s;
}

uint64_t readBe64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(p[i]);
  return v;
}

void WriteOrThrow(lsm::DB* db, lsm::WriteBatch& wb) {
  lsm::WriteOptions wo;
  wo.sync = false;  // fsync is NOT on the ack path; see design 10.5
  const lsm::Status s = db->Write(wo, &wb);
  if (!s.ok()) {
    throw std::runtime_error("LsmKvStateMachine: DB::Write failed: " + s.ToString());
  }
}
#endif

}  // namespace

bool LsmKvStateMachine::haveLsm() {
#ifdef RAFTK_HAVE_LSM
  return true;
#else
  return false;
#endif
}

const char* LsmKvStateMachine::unavailableReason() { return kNoLsmReason; }

std::string LsmKvStateMachine::dataKey(const std::string& userKey) {
  std::string k;
  k.reserve(1 + userKey.size());
  k.push_back(static_cast<char>(0x01));
  k.append(userKey);
  return k;
}

std::string LsmKvStateMachine::dedupKey(uint64_t clientId) {
  std::string k;
  k.reserve(9);
  k.push_back(static_cast<char>(0x02));
  for (int i = 7; i >= 0; --i) {
    k.push_back(static_cast<char>((clientId >> (i * 8)) & 0xff));
  }
  return k;
}

std::string LsmKvStateMachine::appliedKey() {
  std::string k;
  k.reserve(8);
  k.push_back(static_cast<char>(0));
  k.append("applied");
  return k;
}

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
#endif
}

LsmKvStateMachine::~LsmKvStateMachine() { db_.reset(); }

#ifdef RAFTK_HAVE_LSM
void LsmKvStateMachine::rebuildMirror() {
  data_.clear();
  lastRequest_.clear();
  lastApplied_ = raft::kNoIndex;
  const std::string ak = appliedKey();
  std::unique_ptr<lsm::Iterator> it(db_->NewIterator());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const lsm::Slice k = it->key();
    const lsm::Slice v = it->value();
    if (k.size() == ak.size() && std::memcmp(k.data(), ak.data(), ak.size()) == 0) {
      if (v.size() == 8) lastApplied_ = readBe64(v.data());
      continue;
    }
    if (k.size() >= 1 && static_cast<uint8_t>(k.data()[0]) == 0x02) {
      if (k.size() == 9 && v.size() == 8) {
        lastRequest_[readBe64(k.data() + 1)] = readBe64(v.data());
      }
      continue;
    }
    if (k.size() >= 1 && static_cast<uint8_t>(k.data()[0]) == 0x01) {
      data_[std::string(k.data() + 1, k.size() - 1)] =
          std::string(v.data(), v.size());
      continue;
    }
    // Unknown namespace: ignore (forward compatible, never crash).
  }
}
#else
void LsmKvStateMachine::rebuildMirror() {}
#endif

void LsmKvStateMachine::apply(const raft::LogEntry& e) {
#ifndef RAFTK_HAVE_LSM
  (void)e;
  throw std::runtime_error(kNoLsmReason);
#else
  if (poisoned_) throw std::runtime_error(kPoisonedReason);
  try {
    const bool isData = (e.op == OpCode::kPut || e.op == OpCode::kDel);
    if (!isData) {
      // No-op marker / config entry: only advance the applied watermark.
      lsm::WriteBatch wb;
      wb.Put(Sl(appliedKey()), Sl(be64str(e.index)));
      WriteOrThrow(db_.get(), wb);
      lastApplied_ = e.index;
      return;
    }

    const auto it = lastRequest_.find(e.clientId);
    if (it != lastRequest_.end() && e.requestId <= it->second) {
      // m2-design 6.5: "requestId <= lastRequestId -> directly discard". Mirror
      // the mem baseline exactly: nothing is written and the applied watermark
      // does not move (design 10.14 documents who owns the applied watermark).
      return;
    }

    // One WriteBatch == one WAL record: data + dedup + applied are atomic.
    lsm::WriteBatch wb;
    const std::string dk = dataKey(e.key);
    if (e.op == OpCode::kPut) {
      wb.Put(Sl(dk), Sl(e.value));
    } else {
      wb.Delete(Sl(dk));
    }
    wb.Put(Sl(dedupKey(e.clientId)), Sl(be64str(e.requestId)));
    wb.Put(Sl(appliedKey()), Sl(be64str(e.index)));
    WriteOrThrow(db_.get(), wb);

    if (e.op == OpCode::kPut) {
      data_[e.key] = e.value;
    } else {
      data_.erase(e.key);
    }
    lastRequest_[e.clientId] = e.requestId;
    lastApplied_ = e.index;
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
  try {
    // Diff against the in-memory mirror -> one atomic WriteBatch that removes
    // keys absent from the snapshot and installs the new ones.
    lsm::WriteBatch wb;
    for (const auto& kv : data) {
      const auto it = data_.find(kv.first);
      if (it == data_.end() || it->second != kv.second) {
        wb.Put(Sl(dataKey(kv.first)), Sl(kv.second));
      }
    }
    for (const auto& kv : data_) {
      if (data.find(kv.first) == data.end()) wb.Delete(Sl(dataKey(kv.first)));
    }
    for (const auto& d : dedup) {
      const auto it = lastRequest_.find(d.first);
      if (it == lastRequest_.end() || it->second != d.second) {
        wb.Put(Sl(dedupKey(d.first)), Sl(be64str(d.second)));
      }
    }
    for (const auto& d : lastRequest_) {
      if (dedup.find(d.first) == dedup.end()) wb.Delete(Sl(dedupKey(d.first)));
    }
    wb.Put(Sl(appliedKey()), Sl(be64str(la)));

    lsm::WriteOptions wo;
    wo.sync = false;
    const lsm::Status s = db_->Write(wo, &wb);
    if (!s.ok()) {
      // Structural rejection (batch too big) is transient; IO/corruption is
      // fail-stop, exactly like the log adapter's sticky poison.
      if (!s.IsInvalidArgument()) poisoned_ = true;
      return false;
    }
  } catch (...) {
    poisoned_ = true;
    return false;
  }

  data_ = std::move(data);
  lastRequest_ = std::move(dedup);
  lastApplied_ = la;
  return true;
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
