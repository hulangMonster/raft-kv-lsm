
// M6.9.1: LsmKvStateMachine contract + crash-consistency tests.
// RED: kv/lsm_kv_state_machine.h does not exist yet, so this file must fail to
// compile until M6.9.2 implements the LSM backend.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "kv/kv_state_machine.h"
#include "kv/lsm_kv_state_machine.h"
#include "sm_contract_body.h"

using namespace raftkv;
using namespace raftkv::smtest;

namespace {

std::string tempDir() {
  static uint64_t counter = 0;
  auto p = std::filesystem::temp_directory_path() /
           ("raftkv_sm_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++));
  std::filesystem::create_directories(p);
  return p.string();
}

std::string firstWalPath(const std::string& dir) {
  if (!std::filesystem::exists(dir)) return {};
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    const std::string p = e.path().string();
    if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".log") == 0) return p;
  }
  return {};
}

}  // namespace

class LsmContract : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = tempDir(); }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  Factory<LsmKvStateMachine> Lsm() {
    const std::string d = dir_;
    return [d]() { return std::make_unique<LsmKvStateMachine>(d); };
  }
  std::string dir_;
};

TEST_F(LsmContract, Empty) { Contract_Empty<LsmKvStateMachine>(Lsm()); }
TEST_F(LsmContract, BasicOps) { Contract_BasicOps<LsmKvStateMachine>(Lsm()); }
TEST_F(LsmContract, IdempotentSameIndex) {
  Contract_IdempotentSameIndex<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, IdempotentHigherIndexAdvancesApplied) {
  Contract_IdempotentHigherIndexAdvancesApplied<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, MarkersAdvanceApplied) {
  Contract_MarkersAdvanceApplied<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, CanonicalState) {
  Contract_CanonicalState<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, SnapshotDeterministic) {
  Contract_SnapshotDeterministic<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, StreamMatchesSerialize) {
  Contract_StreamMatchesSerialize<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, SnapshotRoundTrip) {
  Contract_SnapshotRoundTrip<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, RestoreRejectsMalformed) {
  Contract_RestoreRejectsMalformed<LsmKvStateMachine>(Lsm());
}
TEST_F(LsmContract, CrossRestoreMemToLsm) {
  Contract_CrossRestore<KvStateMachine, LsmKvStateMachine>(
      []() { return std::make_unique<KvStateMachine>(); }, Lsm());
}
TEST_F(LsmContract, CrossRestoreLsmToMem) {
  Contract_CrossRestore<LsmKvStateMachine, KvStateMachine>(
      Lsm(), []() { return std::make_unique<KvStateMachine>(); });
}

// The LSM backend must produce byte-identical snapshot payloads to the mem
// backend for the same apply sequence (design 10.4 / R-D).
TEST_F(LsmContract, SnapshotBytesIdenticalToMem) {
  auto m = std::make_unique<KvStateMachine>();
  Factory<LsmKvStateMachine> f = Lsm();
  auto l = f();
  for (const LogEntry& e : canonicalOps()) {
    m->apply(e);
    l->apply(e);
  }
  auto vm = m->snapshotView();
  auto vl = l->snapshotView();
  ASSERT_NE(vm, nullptr);
  ASSERT_NE(vl, nullptr);
  EXPECT_EQ(vm->serialize(), vl->serialize());

  // streaming must agree too
  auto sm = vm->stream();
  auto sl = vl->stream();
  ASSERT_NE(sm, nullptr);
  ASSERT_NE(sl, nullptr);
  Bytes a;
  Bytes b;
  Bytes pa;
  Bytes pb;
  while (sm->next(pa, 1)) a.insert(a.end(), pa.begin(), pa.end());
  while (sl->next(pb, 1)) b.insert(b.end(), pb.begin(), pb.end());
  EXPECT_EQ(a, b);
  EXPECT_EQ(a, vm->serialize());
}

class LsmCrash : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = tempDir(); }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  std::string dir_;
};

#ifndef __SANITIZE_THREAD__
// fork + _exit(0) bypasses every destructor == kill -9 equivalent.
TEST_F(LsmCrash, UncleanExitKeepsConsistentPrefix) {
  const int N = 200;
  ::pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    LsmKvStateMachine* sm = new LsmKvStateMachine(dir_);  // leaked on purpose
    for (int i = 1; i <= N; ++i) {
      sm->apply(put(static_cast<Index>(i), "k" + std::to_string(i),
                    "v" + std::to_string(i), 1, static_cast<uint64_t>(i)));
    }
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  LsmKvStateMachine sm2(dir_);
  const Index m = sm2.lastApplied();
  EXPECT_LE(m, static_cast<Index>(N));
  for (int i = 1; i <= N; ++i) {
    std::string out;
    if (static_cast<Index>(i) <= m) {
      ASSERT_TRUE(sm2.get("k" + std::to_string(i), out)) << "i=" << i;
      EXPECT_EQ(out, "v" + std::to_string(i));
    } else {
      EXPECT_FALSE(sm2.get("k" + std::to_string(i), out)) << "i=" << i;
    }
  }
  // Replaying the whole log must be idempotent and converge to the full state.
  for (int i = 1; i <= N; ++i) {
    sm2.apply(put(static_cast<Index>(i), "k" + std::to_string(i),
                  "v" + std::to_string(i), 1, static_cast<uint64_t>(i)));
  }
  EXPECT_EQ(sm2.lastApplied(), static_cast<Index>(N));
  for (int i = 1; i <= N; ++i) {
    std::string out;
    ASSERT_TRUE(sm2.get("k" + std::to_string(i), out)) << "i=" << i;
    EXPECT_EQ(out, "v" + std::to_string(i));
  }
}

TEST_F(LsmCrash, SyncBeforeExitKeepsAllDurable) {
  const int N = 200;
  ::pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    LsmKvStateMachine* sm = new LsmKvStateMachine(dir_);
    for (int i = 1; i <= N; ++i) {
      sm->apply(put(static_cast<Index>(i), "k" + std::to_string(i),
                    "v" + std::to_string(i), 1, static_cast<uint64_t>(i)));
    }
    if (!sm->sync()) ::_exit(2);
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  LsmKvStateMachine sm2(dir_);
  EXPECT_EQ(sm2.lastApplied(), static_cast<Index>(N));
  for (int i = 1; i <= N; ++i) {
    std::string out;
    ASSERT_TRUE(sm2.get("k" + std::to_string(i), out)) << "i=" << i;
    EXPECT_EQ(out, "v" + std::to_string(i));
  }
}
#endif  // !__SANITIZE_THREAD__

TEST_F(LsmCrash, TornWalTailRecoversConsistentPrefix) {
  const int N = 200;
  {
    LsmKvStateMachine sm(dir_);
    for (int i = 1; i <= N; ++i) {
      sm.apply(put(static_cast<Index>(i), "k" + std::to_string(i),
                    "v" + std::to_string(i), 1, static_cast<uint64_t>(i)));
    }
    ASSERT_TRUE(sm.sync());
  }
  const std::string wal = firstWalPath(dir_ + "/kv-lsm");
  ASSERT_FALSE(wal.empty());
  {
    std::ofstream out(wal, std::ios::binary | std::ios::app);
    const char junk[] = "torn-tail-junk-not-a-record";
    out.write(junk, sizeof(junk) - 1);
  }
  LsmKvStateMachine sm2(dir_);
  EXPECT_EQ(sm2.lastApplied(), static_cast<Index>(N));
  for (int i = 1; i <= N; ++i) {
    std::string out;
    ASSERT_TRUE(sm2.get("k" + std::to_string(i), out)) << "i=" << i;
    EXPECT_EQ(out, "v" + std::to_string(i));
  }
}

// restore() must replace the whole state (data + dedup + applied) in one atomic
// batch, and that must survive a reopen.
TEST_F(LsmCrash, RestoreAtomicSurvivesReopen) {
  Bytes payload;
  {
    KvStateMachine mem;
    for (const LogEntry& e : canonicalOps()) mem.apply(e);
    payload = mem.snapshotView()->serialize();
  }
  {
    LsmKvStateMachine sm(dir_);
    sm.apply(put(1, "stale", "x", 5, 1));
    ASSERT_TRUE(sm.sync());
  }
  {
    LsmKvStateMachine sm(dir_);
    ASSERT_TRUE(sm.restore(payload));
    ASSERT_TRUE(sm.sync());
  }
  {
    LsmKvStateMachine sm(dir_);
    checkCanonicalState(sm);
    std::string out;
    EXPECT_FALSE(sm.get("stale", out));
  }
}
