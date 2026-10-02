
// M6.10.1 (B1): chunked / atomic restore tests for LsmKvStateMachine.
//
// The lsm engine caps a single record at kMaxLogicalRecordSize = 64 MiB, and a
// single WriteBatch at write_batch::kMaxBytes = 64 MiB. restore() must therefore
// (a) split a large payload across many bounded WriteBatches and (b) split a
// single oversized value across chunk keys, and it must switch namespaces
// atomically so a crash never exposes a half-restored state.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "kv/kv_snapshot_view.h"
#include "kv/lsm_kv_state_machine.h"

using namespace raftkv;
using raft::Index;
using raft::LogEntry;

namespace {

std::string tempDir() {
  static uint64_t counter = 0;
  auto p = std::filesystem::temp_directory_path() /
           ("raftkv_restore_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++));
  std::filesystem::create_directories(p);
  return p.string();
}

LogEntry putE(Index i, const std::string& k, const std::string& v,
              uint64_t cid = 1, uint64_t rid = 0) {
  LogEntry e;
  e.index = i;
  e.term = 1;
  e.op = OpCode::kPut;
  e.key = k;
  e.value = v;
  e.clientId = cid;
  e.requestId = rid != 0 ? rid : i;
  return e;
}

Bytes encodeOne(const std::string& key, const std::string& value, Index la) {
  std::vector<std::pair<std::string, std::string>> kv = {{key, value}};
  std::vector<std::pair<uint64_t, uint64_t>> dd = {{7, la}};
  return kvview::encode(kv, dd, la);
}

Bytes encodeMany(size_t n, size_t valueLen, Index la) {
  std::vector<std::pair<std::string, std::string>> kv;
  for (size_t i = 0; i < n; ++i) {
    kv.emplace_back("big" + std::to_string(i),
                    std::string(valueLen, static_cast<char>('a' + (i % 26))));
  }
  std::vector<std::pair<uint64_t, uint64_t>> dd = {{7, la}};
  return kvview::encode(kv, dd, la);
}

void SeedStateA(LsmKvStateMachine& sm) {
  sm.apply(putE(1, "a1", "A-value", 1, 11));
  sm.apply(putE(2, "a2", "A-two", 1, 12));
}

void CheckStateA(LsmKvStateMachine& sm) {
  std::string out;
  ASSERT_TRUE(sm.get("a1", out));
  EXPECT_EQ(out, "A-value");
  ASSERT_TRUE(sm.get("a2", out));
  EXPECT_EQ(out, "A-two");
  EXPECT_EQ(sm.lastApplied(), static_cast<Index>(2));
}

class LsmRestore : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = tempDir(); }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  std::string dir_;
};

}  // namespace

TEST_F(LsmRestore, MultiEntryPayloadOver64MiBRestoresInManyBatches) {
  const size_t kEntries = 160;
  const size_t kVal = 512 * 1024;  // 160 * 512 KiB = 80 MiB payload
  const Bytes payload = encodeMany(kEntries, kVal, 500);
  ASSERT_GT(payload.size(), 64u * 1024 * 1024u);
  {
    LsmKvStateMachine sm(dir_);
    ASSERT_TRUE(sm.restore(payload));
    EXPECT_GE(sm.stagedBatches(), static_cast<uint64_t>(2));
    EXPECT_EQ(sm.lastApplied(), static_cast<Index>(500));
    std::string out;
    ASSERT_TRUE(sm.get("big0", out));
    EXPECT_EQ(out.size(), kVal);
    ASSERT_TRUE(sm.get("big159", out));
    EXPECT_EQ(out.size(), kVal);
    ASSERT_TRUE(sm.sync());
  }
  {
    LsmKvStateMachine sm2(dir_);
    EXPECT_EQ(sm2.lastApplied(), static_cast<Index>(500));
    std::string out;
    ASSERT_TRUE(sm2.get("big0", out));
    EXPECT_EQ(out.size(), kVal);
    ASSERT_TRUE(sm2.get("big159", out));
    EXPECT_EQ(out.size(), kVal);
  }
}

TEST_F(LsmRestore, SingleHugeValueRestoresAcrossChunks) {
  // the reviewer's repro: one value > the 64 MiB lsm per-record limit
  const size_t kVal = 70u * 1024u * 1024u;
  const Bytes payload = encodeOne("big", std::string(kVal, 'x'), 9);
  ASSERT_GT(payload.size(), 64u * 1024 * 1024u);
  {
    LsmKvStateMachine sm(dir_);
    ASSERT_TRUE(sm.restore(payload));
    std::string out;
    ASSERT_TRUE(sm.get("big", out));
    EXPECT_EQ(out.size(), kVal);
    EXPECT_EQ(sm.lastApplied(), static_cast<Index>(9));
  }
  {
    LsmKvStateMachine sm2(dir_);
    std::string out;
    ASSERT_TRUE(sm2.get("big", out));
    EXPECT_EQ(out.size(), kVal);
    EXPECT_EQ(out[0], 'x');
    EXPECT_EQ(out[kVal - 1], 'x');
  }
}

TEST_F(LsmRestore, Exactly64MiBPayloadRestores) {
  // 8(la) + 4(kvCount) + 8(kl/vl lens) + klen + vlen + 4(dedupCount) + 16 = 40 + klen + vlen
  const size_t klen = 1;
  const size_t vlen = 64u * 1024u * 1024u - 40u - klen;
  const Bytes payload = encodeOne(std::string(klen, 'k'), std::string(vlen, 'y'), 4);
  EXPECT_EQ(payload.size(), 64u * 1024 * 1024u);
  LsmKvStateMachine sm(dir_);
  ASSERT_TRUE(sm.restore(payload));
  std::string out;
  ASSERT_TRUE(sm.get("k", out));
  EXPECT_EQ(out.size(), vlen);
}

TEST_F(LsmRestore, OverOneChunkBelow64MiBRestores) {
  const size_t kVal = 10u * 1024u * 1024u;
  const Bytes payload = encodeOne("mid", std::string(kVal, 'z'), 3);
  ASSERT_GT(payload.size(), 1u << 20);
  ASSERT_LT(payload.size(), 64u * 1024 * 1024u);
  LsmKvStateMachine sm(dir_);
  ASSERT_TRUE(sm.restore(payload));
  std::string out;
  ASSERT_TRUE(sm.get("mid", out));
  EXPECT_EQ(out.size(), kVal);
}

TEST_F(LsmRestore, RepeatedRestoreIsIdempotent) {
  const Bytes p1 = encodeOne("k1", std::string(5u << 20, '1'), 10);
  const Bytes p2 = encodeOne("k2", std::string(5u << 20, '2'), 20);
  LsmKvStateMachine sm(dir_);
  ASSERT_TRUE(sm.restore(p1));
  ASSERT_TRUE(sm.restore(p1));
  std::string out;
  ASSERT_TRUE(sm.get("k1", out));
  EXPECT_EQ(out.size(), 5u << 20);
  EXPECT_EQ(sm.lastApplied(), static_cast<Index>(10));
  ASSERT_TRUE(sm.restore(p2));
  EXPECT_FALSE(sm.get("k1", out));
  ASSERT_TRUE(sm.get("k2", out));
  EXPECT_EQ(sm.lastApplied(), static_cast<Index>(20));
  ASSERT_TRUE(sm.restore(p1));
  EXPECT_FALSE(sm.get("k2", out));
  ASSERT_TRUE(sm.get("k1", out));
  EXPECT_EQ(sm.lastApplied(), static_cast<Index>(10));
}

// NOTE (M6.10.1 BLOCKER): the reopen leg of this case is deliberately not
// asserted. An lsm memtable flush followed immediately by Close() can leave an
// orphan *.sst without a MANIFEST, making the DB unrecoverable; this pattern is
// deterministic for apply() of a value above write_buffer_size. Evidence and
// proposed lsm fix: docs/raw/m6.10.1-BLOCKER-lsm-flush-close.md. It also affects
// the pre-existing LsmLogStore. The reopen assertion is parked until fixed.
TEST_F(LsmRestore, ApplyLargeValueChunksInProcess) {
  const size_t kVal = 6u * 1024u * 1024u;
  LsmKvStateMachine sm(dir_);
  sm.apply(putE(1, "huge", std::string(kVal, 'h'), 1, 1));
  std::string out;
  ASSERT_TRUE(sm.get("huge", out));
  EXPECT_EQ(out.size(), kVal);
  EXPECT_EQ(sm.lastApplied(), static_cast<Index>(1));
}

TEST_F(LsmRestore, ThrowingRestoreHookKeepsOldState) {
  const Bytes big = encodeOne("big", std::string(20u << 20, 'B'), 99);
  LsmKvStateMachine sm(dir_);
  SeedStateA(sm);
  ASSERT_TRUE(sm.sync());
  sm.setRestoreProgressHookForTest([](size_t n) {
    if (n >= 1) throw std::runtime_error("injected restore failure");
  });
  EXPECT_FALSE(sm.restore(big));
  CheckStateA(sm);
  {
    LsmKvStateMachine sm2(dir_);
    CheckStateA(sm2);
    std::string out;
    EXPECT_FALSE(sm2.get("big", out));
  }
}

#ifndef __SANITIZE_THREAD__
TEST_F(LsmRestore, KillBetweenStagedBatchesKeepsOldStateAndConverges) {
  const Bytes big = encodeOne("big", std::string(30u << 20, 'C'), 77);
  {
    LsmKvStateMachine sm(dir_);
    SeedStateA(sm);
    ASSERT_TRUE(sm.sync());
  }
  ::pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    try {
      LsmKvStateMachine sm(dir_);
      sm.setRestoreProgressHookForTest([](size_t n) {
        if (n >= 1) ::_exit(7);  // kill -9 equivalent between staged batches
      });
      sm.restore(big);
    } catch (...) {
      ::_exit(8);
    }
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 7) << "child should be interrupted mid-restore";

  {
    LsmKvStateMachine sm(dir_);
    CheckStateA(sm);  // the old namespace is still authoritative
    std::string out;
    EXPECT_FALSE(sm.get("big", out));
  }
  {
    LsmKvStateMachine sm(dir_);
    ASSERT_TRUE(sm.restore(big));
    std::string out;
    ASSERT_TRUE(sm.get("big", out));
    EXPECT_EQ(out.size(), 30u << 20);
    EXPECT_FALSE(sm.get("a1", out));
    EXPECT_EQ(sm.lastApplied(), static_cast<Index>(77));
  }
  {
    LsmKvStateMachine sm(dir_);
    std::string out;
    ASSERT_TRUE(sm.get("big", out));
    EXPECT_EQ(out.size(), 30u << 20);
  }
}
#endif  // !__SANITIZE_THREAD__

TEST_F(LsmRestore, RestoredDedupTableStaysConsistentWithData) {
  const Bytes big = encodeMany(4, 8u << 20, 42);  // 4 x 8 MiB chunked values
  {
    LsmKvStateMachine sm(dir_);
    ASSERT_TRUE(sm.restore(big));
    ASSERT_TRUE(sm.sync());
  }
  {
    LsmKvStateMachine sm2(dir_);
    std::string out;
    ASSERT_TRUE(sm2.get("big0", out));
    const std::string before = out;
    // Replay a duplicate of the payload's dedup entry at a HIGHER index: the mem
    // contract discards it, so the data must not change and applied must not move.
    sm2.apply(putE(1000, "big0", "MUST-NOT-APPLY", 7, 42));
    ASSERT_TRUE(sm2.get("big0", out));
    EXPECT_EQ(out, before);
    EXPECT_EQ(sm2.lastApplied(), static_cast<Index>(42));
  }
}
