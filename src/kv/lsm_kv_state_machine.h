
#pragma once
// M6.9/M6.10: LSM-backed state machine. Same external contract/semantics as
// KvStateMachine, but the durable truth for the state machine (data + dedup
// table + lastApplied) lives in an lsm DB at <dir>/kv-lsm.
//
// Layering: this header forward-declares lsm::DB only. Every lsm type
// (DB/WriteBatch/Options) lives in the .cpp adapter, so raft_node.cpp never
// sees an lsm header.
//
// Lock discipline (design 10.6): RaftNode::mu_ serialises every call into this
// class; the class adds no lock of its own (same as KvStateMachine). get() and
// snapshotView() are pure in-memory reads off the mirror, so no I/O ever happens
// under mu_. sync() is the only call expected outside mu_.
//
// M6.10.1 dual-namespace commit (design 10.15): restore() never overwrites the
// live namespace; it stages a full copy in the OTHER namespace in bounded
// WriteBatches, then atomically flips a single pointer key. Single values larger
// than the lsm per-record limit (64 MiB) are split into <=4 MiB chunk keys and
// become visible only when the header commits. A crash at any point therefore
// leaves exactly one COMPLETE namespace visible (old or new), never a mix.
#include <cstdint>
#include <functional>
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

  // Test-only seam: called after every staged WriteBatch flush inside restore().
  // nullptr in production. A throwing hook makes restore() abort with false and
  // leaves the old namespace authoritative; a hook that calls _exit() simulates
  // kill -9 between staged batches.
  using RestoreProgressHook = std::function<void(size_t staged_batches_done)>;
  void setRestoreProgressHookForTest(RestoreProgressHook hook) {
    restoreHook_ = std::move(hook);
  }

  explicit LsmKvStateMachine(std::string dir);  // Open failure / no lsm => throw
  ~LsmKvStateMachine() override;

  void apply(const raft::LogEntry& entry) override;
  bool get(const std::string& key, std::string& out) const override;
  raft::Index lastApplied() const override;
  std::shared_ptr<const raft::SnapshotView> snapshotView() const override;
  bool restore(const Bytes& payload) override;

  // Fsync the lsm DB. MUST NOT be called while holding RaftNode::mu_.
  bool sync();

  // Diagnostics (read-only; not part of the StateMachine contract).
  uint64_t restoreCount() const { return restoreCount_; }
  uint64_t stagedBatches() const { return stagedBatches_; }

 private:
  struct LsmDbDeleter {
    void operator()(lsm::DB* db) const noexcept;
  };
  void rebuildMirror();
  void gcNonCurrentNamespace();

  std::string dir_;
  std::unique_ptr<lsm::DB, LsmDbDeleter> db_;
  std::unordered_map<std::string, std::string> data_;
  std::unordered_map<uint64_t, uint64_t> lastRequest_;
  std::unordered_map<std::string, uint64_t> valueIds_;  // chunked key -> valueId
  raft::Index lastApplied_ = raft::kNoIndex;
  bool poisoned_ = false;
  uint8_t ns_ = 0;  // current committed namespace (0 or 1)
  uint64_t nextValueId_ = 1;
  RestoreProgressHook restoreHook_;
  uint64_t restoreCount_ = 0;
  uint64_t stagedBatches_ = 0;
};

}  // namespace raftkv
