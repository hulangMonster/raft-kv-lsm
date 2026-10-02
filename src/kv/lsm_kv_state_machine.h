
#pragma once
// M6.9: LSM-backed state machine. Same external contract/semantics as
// KvStateMachine (design 10.4), but the durable truth for the state machine
// (data + dedup table + lastApplied) is an lsm DB at <dir>/kv-lsm.
//
// Layering: this header forward-declares lsm::DB only. Every lsm type
// (DB/WriteBatch/Options) lives in the .cpp adapter, so raft_node.cpp never
// sees an lsm header.
//
// Lock discipline (design 10.6): RaftNode::mu_ serialises every call into this
// class; the class adds no lock of its own (same as KvStateMachine). get() and
// snapshotView() are pure in-memory reads off the mirror, so no I/O ever happens
// under mu_. sync() is the only call expected outside mu_.
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "raft/state_machine.h"
#include "raft/types.h"

namespace lsm {
class DB;
}

namespace raftkv {

class LsmKvStateMachine : public raft::StateMachine {
 public:
  static bool haveLsm();
  static const char* unavailableReason();

  // Internal key layout (not a wire/disk protocol; test-visible for pinning).
  static std::string dataKey(const std::string& userKey);  // 0x01 || userKey
  static std::string dedupKey(uint64_t clientId);          // 0x02 || BE64
  static std::string appliedKey();                         // 0x00 "applied"

  explicit LsmKvStateMachine(std::string dir);  // Open failure / no lsm => throw
  ~LsmKvStateMachine() override;

  void apply(const raft::LogEntry& entry) override;
  bool get(const std::string& key, std::string& out) const override;
  raft::Index lastApplied() const override;
  std::shared_ptr<const raft::SnapshotView> snapshotView() const override;
  bool restore(const Bytes& payload) override;

  // Fsync the lsm DB. MUST NOT be called while holding RaftNode::mu_.
  bool sync();

 private:
  struct LsmDbDeleter {
    void operator()(lsm::DB* db) const noexcept;
  };
  void rebuildMirror();

  std::string dir_;
  std::unique_ptr<lsm::DB, LsmDbDeleter> db_;
  std::unordered_map<std::string, std::string> data_;
  std::unordered_map<uint64_t, uint64_t> lastRequest_;
  raft::Index lastApplied_ = raft::kNoIndex;
  bool poisoned_ = false;
};

}  // namespace raftkv
