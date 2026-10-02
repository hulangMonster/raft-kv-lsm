
// M6.10.4: post-lsm-fix regression for the M6.10.1 BLOCKER.
//
// Not added to CMakeLists.txt until the lsm engine fix lands (see
// docs/raw/m6.10.1-BLOCKER-lsm-flush-close.md): on the f06a44d pin this pattern
// deterministically leaves an orphan *.sst without a MANIFEST, so DB::Open
// fails after reopen. Once the pinned lsm sha carries the fix, add this file to
// raftkv_raft_tests and it becomes the formal regression.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "kv/lsm_kv_state_machine.h"

using namespace raftkv;
using raft::Index;
using raft::LogEntry;

namespace {

std::string tempDir() {
  static uint64_t counter = 0;
  auto p = std::filesystem::temp_directory_path() /
           ("raftkv_apply_reopen_" + std::to_string(::getpid()) + "_" +
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

}  // namespace

// apply() of a value above the lsm write_buffer_size (4 MiB) triggers a
// memtable flush; the DB must still reopen with the value intact.
TEST(LsmApplyReopen, LargeApplySurvivesReopen) {
  const std::string dir = tempDir();
  const size_t kVal = 6u * 1024u * 1024u;
  {
    LsmKvStateMachine sm(dir);
    sm.apply(putE(1, "huge", std::string(kVal, 'h'), 1, 1));
    std::string out;
    ASSERT_TRUE(sm.get("huge", out));
    EXPECT_EQ(out.size(), kVal);
    ASSERT_TRUE(sm.sync());
  }
  {
    LsmKvStateMachine sm2(dir);
    std::string out;
    ASSERT_TRUE(sm2.get("huge", out));
    EXPECT_EQ(out.size(), kVal);
    EXPECT_EQ(sm2.lastApplied(), static_cast<Index>(1));
  }
  std::filesystem::remove_all(dir);
}

#ifndef __SANITIZE_THREAD__
// Same, but the writer process is killed (kill -9 equivalent) right after the
// large apply instead of closing gracefully.
TEST(LsmApplyReopen, LargeApplySurvivesKill9) {
  const std::string dir = tempDir();
  const size_t kVal = 6u * 1024u * 1024u;
  ::pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    LsmKvStateMachine* sm = new LsmKvStateMachine(dir);  // leaked on purpose
    sm->apply(putE(1, "huge", std::string(kVal, 'h'), 1, 1));
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  LsmKvStateMachine sm2(dir);
  std::string out;
  ASSERT_TRUE(sm2.get("huge", out));
  EXPECT_EQ(out.size(), kVal);
  std::filesystem::remove_all(dir);
}
#endif  // !__SANITIZE_THREAD__
