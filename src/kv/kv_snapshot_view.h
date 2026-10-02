
#pragma once
// M6.9: shared snapshot-payload codec + streaming view for StateMachine backends.
//
// FROZEN byte format (unchanged since M3; the snapshot file adds its own header
// around this):
//   [lastApplied:8]
//   [kvCount:4]    then kvCount    x [klen:4][vlen:4][key][value]
//   [dedupCount:4] then dedupCount x [clientId:8][requestId:8]
//
// NOTE (M6.10 N5): KvStateMachine keeps its own (unchanged, baseline) codec, so
// there ARE two implementations of this frozen format. They are pinned byte-for-byte
// by Contract_SnapshotBytesIdenticalToMem + Contract_CrossRestore* in the test suite;
// this header is the single place the LSM backend encodes/decodes.
// decode() never trusts a wire count: every field is bounds-checked before it is
// read (M6-I11: malformed/oversized keys must not read out of bounds or panic).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "raft/state_machine.h"
#include "raft/types.h"

namespace raftkv::kvview {

inline void putU32(Bytes& out, uint32_t v) {
  out.push_back(static_cast<Byte>((v >> 24) & 0xff));
  out.push_back(static_cast<Byte>((v >> 16) & 0xff));
  out.push_back(static_cast<Byte>((v >> 8) & 0xff));
  out.push_back(static_cast<Byte>(v & 0xff));
}
inline void putU64(Bytes& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<Byte>((v >> (i * 8)) & 0xff));
}
inline uint32_t getU32(const Byte* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint64_t getU64(const Byte* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

inline Bytes encode(const std::vector<std::pair<std::string, std::string>>& kv,
                    const std::vector<std::pair<uint64_t, uint64_t>>& dedup,
                    raft::Index lastApplied) {
  Bytes out;
  putU64(out, lastApplied);
  putU32(out, static_cast<uint32_t>(kv.size()));
  for (const auto& e : kv) {
    putU32(out, static_cast<uint32_t>(e.first.size()));
    putU32(out, static_cast<uint32_t>(e.second.size()));
    out.insert(out.end(), e.first.begin(), e.first.end());
    out.insert(out.end(), e.second.begin(), e.second.end());
  }
  putU32(out, static_cast<uint32_t>(dedup.size()));
  for (const auto& d : dedup) {
    putU64(out, d.first);
    putU64(out, d.second);
  }
  return out;
}

inline bool decode(const Bytes& payload,
                   std::unordered_map<std::string, std::string>& data,
                   std::unordered_map<uint64_t, uint64_t>& dedup,
                   raft::Index& lastApplied) {
  if (payload.size() < 8) return false;
  const raft::Index la = getU64(payload.data());
  size_t off = 8;

  if (off + 4 > payload.size()) return false;
  const uint32_t kvCount = getU32(payload.data() + off);
  off += 4;
  // Never trust a count from the wire: each entry needs at least 8 bytes.
  if (kvCount > (payload.size() - off) / 8) return false;

  std::unordered_map<std::string, std::string> d;
  d.reserve(kvCount);
  for (uint32_t i = 0; i < kvCount; ++i) {
    if (off + 8 > payload.size()) return false;
    const uint32_t klen = getU32(payload.data() + off);
    const uint32_t vlen = getU32(payload.data() + off + 4);
    off += 8;
    if (klen > payload.size() - off) return false;
    if (vlen > payload.size() - off - klen) return false;
    std::string k(reinterpret_cast<const char*>(payload.data() + off), klen);
    off += klen;
    std::string v(reinterpret_cast<const char*>(payload.data() + off), vlen);
    off += vlen;
    d[std::move(k)] = std::move(v);
  }

  if (off + 4 > payload.size()) return false;
  const uint32_t dedupCount = getU32(payload.data() + off);
  off += 4;
  if (dedupCount > (payload.size() - off) / 16) return false;

  std::unordered_map<uint64_t, uint64_t> dd;
  dd.reserve(dedupCount);
  for (uint32_t i = 0; i < dedupCount; ++i) {
    if (off + 16 > payload.size()) return false;
    const uint64_t cid = getU64(payload.data() + off);
    const uint64_t rid = getU64(payload.data() + off + 8);
    off += 16;
    dd[cid] = rid;
  }
  if (off != payload.size()) return false;  // trailing garbage

  data = std::move(d);
  dedup = std::move(dd);
  lastApplied = la;
  return true;
}

// Streaming producer, byte-identical to encode(). Chunks a single large record
// across next() calls (carry-over tail) so the peak is O(maxChunk + 1 record).
class Stream : public raft::SnapshotStream {
 public:
  Stream(std::vector<std::pair<std::string, std::string>> kv,
         std::vector<std::pair<uint64_t, uint64_t>> dedup,
         raft::Index lastApplied)
      : kv_(std::move(kv)), dedup_(std::move(dedup)), lastApplied_(lastApplied) {}

  bool next(Bytes& out, size_t maxChunk) override {
    if (maxChunk == 0) maxChunk = 4096;
    out.clear();
    while (out.size() < maxChunk) {
      if (tailOff_ < tail_.size()) {
        const size_t take = std::min(maxChunk - out.size(), tail_.size() - tailOff_);
        out.insert(out.end(), tail_.begin() + static_cast<std::ptrdiff_t>(tailOff_),
                   tail_.begin() + static_cast<std::ptrdiff_t>(tailOff_ + take));
        tailOff_ += take;
        if (tailOff_ == tail_.size()) {
          tail_.clear();
          tailOff_ = 0;
        }
        continue;
      }
      if (!buildNextRecord()) break;
    }
    return !out.empty();
  }

 private:
  bool buildNextRecord() {
    if (stage_ == Stage::kHeader) {
      putU64(tail_, lastApplied_);
      putU32(tail_, static_cast<uint32_t>(kv_.size()));
      stage_ = Stage::kKv;
      return true;
    }
    if (stage_ == Stage::kKv) {
      if (kvIdx_ < kv_.size()) {
        const auto& e = kv_[kvIdx_++];
        putU32(tail_, static_cast<uint32_t>(e.first.size()));
        putU32(tail_, static_cast<uint32_t>(e.second.size()));
        tail_.insert(tail_.end(), e.first.begin(), e.first.end());
        tail_.insert(tail_.end(), e.second.begin(), e.second.end());
        return true;
      }
      putU32(tail_, static_cast<uint32_t>(dedup_.size()));
      stage_ = Stage::kDedup;
      return true;
    }
    if (stage_ == Stage::kDedup) {
      if (dedupIdx_ < dedup_.size()) {
        const auto& d = dedup_[dedupIdx_++];
        putU64(tail_, d.first);
        putU64(tail_, d.second);
        return true;
      }
      stage_ = Stage::kDone;
      return false;
    }
    return false;
  }

  enum class Stage { kHeader, kKv, kDedup, kDone };
  std::vector<std::pair<std::string, std::string>> kv_;
  std::vector<std::pair<uint64_t, uint64_t>> dedup_;
  raft::Index lastApplied_ = raft::kNoIndex;
  Stage stage_ = Stage::kHeader;
  size_t kvIdx_ = 0;
  size_t dedupIdx_ = 0;
  Bytes tail_;
  size_t tailOff_ = 0;
};

// Immutable copy handed to the out-of-lock serializer (lock discipline L8).
class View : public raft::SnapshotView {
 public:
  View(std::vector<std::pair<std::string, std::string>> kv,
       std::vector<std::pair<uint64_t, uint64_t>> dedup,
       raft::Index lastApplied)
      : kv_(std::move(kv)), dedup_(std::move(dedup)), lastApplied_(lastApplied) {}

  std::unique_ptr<raft::SnapshotStream> stream() const override {
    return std::make_unique<Stream>(kv_, dedup_, lastApplied_);
  }
  Bytes serialize() const override { return encode(kv_, dedup_, lastApplied_); }

 private:
  std::vector<std::pair<std::string, std::string>> kv_;
  std::vector<std::pair<uint64_t, uint64_t>> dedup_;
  raft::Index lastApplied_;
};

}  // namespace raftkv::kvview
