// B-group: disk persistence tests (FileLogStore + LsmLogStore) — real temp dirs, real files.
//
// M6.3（设计 §4-M6.3 / D15）：把用例体抽成 `template <typename Store>`，让**两个引擎由
// 同一份断言文本**钉住；`FileLogStore.*` 的用例名与断言**逐字保留**（一行未改），新增
// `LsmLogStore.*` 孪生。确定性内存集群的用例（tests/raft_test_harness.h 的 makeCluster）
// 一律不动。
//
// `TruncatesTornTail` 的引擎差异：FileLogStore 用 `[crc][len]` 帧，撕裂尾靠自己的 CRC 判定；
// LsmLogStore 的 WAL 是 lsm 的格式，撕裂尾由 `DB::Open` 的恢复路径截断（m2-design §5.3）。
// 两者都能「往尾部追加垃圾」来注入，因此这里给出**真正的孪生**（而不是标注未覆盖）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "raft/log_store.h"
#include "raft/lsm_log_store.h"

using namespace raftkv::raft;

namespace {

std::string tempDir() {
  static uint64_t counter = 0;
  auto p = std::filesystem::temp_directory_path() /
           ("raftkv_raft_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++));
  std::filesystem::create_directories(p);
  return p.string();
}

LogEntry entry(Index index, Term term, const std::string& key,
               const std::string& value) {
  LogEntry e;
  e.index = index;
  e.term = term;
  e.op = raftkv::OpCode::kPut;
  e.key = key;
  e.value = value;
  e.clientId = 1;
  e.requestId = index;
  return e;
}

// 两个引擎共享的用例体（M6.3）：断言文本逐字相同，只有 Store 类型不同。
template <typename Store>
void RestartRestoresMetaAndLogBody() {
  const std::string dir = tempDir();
  {
    Store store(dir);
    ASSERT_TRUE(store.persistMeta(2, 1));
    std::vector<LogEntry> entries;
    entries.push_back(entry(1, 1, "a", "1"));
    entries.push_back(entry(2, 2, "b", "2"));
    ASSERT_TRUE(store.append(entries));
  }
  {
    Store store(dir);
    Term term = 0;
    int votedFor = -1;
    Index lastIndex = 0;
    ASSERT_TRUE(store.load(term, votedFor, lastIndex));
    EXPECT_EQ(term, 2);
    EXPECT_EQ(votedFor, 1);
    EXPECT_EQ(lastIndex, 2);
    EXPECT_EQ(store.lastTerm(), 2);
    EXPECT_EQ(store.termAt(1), 1);
    EXPECT_EQ(store.termAt(2), 2);
  }
  std::filesystem::remove_all(dir);
}

// LsmLogStore 的 WAL 路径（<dir>/raft-lsm/000001.log，编号会变 ⇒ 取第一个 *.log）。
std::string firstWalPath(const std::string& dir) {
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    const std::string p = e.path().string();
    if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".log") == 0) return p;
  }
  return {};
}

const char kTornJunk[] = "torn-tail-junk-not-a-record";

}  // namespace

// ---- FileLogStore（M2 起的既有用例：名字与断言一字不动）--------------------

TEST(FileLogStore, RestartRestoresMetaAndLog) {
  RestartRestoresMetaAndLogBody<FileLogStore>();
}

TEST(FileLogStore, TruncatesTornTail) {
  const std::string dir = tempDir();
  {
    FileLogStore store(dir);
    std::vector<LogEntry> entries;
    entries.push_back(entry(1, 1, "a", "1"));
    ASSERT_TRUE(store.append(entries));
  }
  {
    // Simulate a crash mid-write: append garbage after the valid record.
    std::ofstream out(dir + "/raft/raft.log", std::ios::binary | std::ios::app);
    out.write(kTornJunk, sizeof(kTornJunk) - 1);
  }
  {
    FileLogStore store(dir);
    Term term = 0;
    int votedFor = -1;
    Index lastIndex = 0;
    ASSERT_TRUE(store.load(term, votedFor, lastIndex));
    EXPECT_EQ(lastIndex, 1) << "torn tail must be truncated to the last valid record";
  }
  std::filesystem::remove_all(dir);
}

// 注意（M6.10 N9）：这些 LsmLogStore 孪生**不用 RAFTK_HAVE_LSM 包夹**——未把 lsm 编进
// 二进制时 LsmLogStore 的构造函数会抛（fail-loud），本文件据此失败而不是静默 skip。
// 这是有意的：门禁要求「未配置 lsm ⇒ 明确报错，绝不静默降级」。
// ---- LsmLogStore 孪生（M6.3）----------------------------------------------

TEST(LsmLogStore, RestartRestoresMetaAndLog) {
  RestartRestoresMetaAndLogBody<LsmLogStore>();
}

TEST(LsmLogStore, TruncatesTornTail) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    entries.push_back(entry(1, 1, "a", "1"));
    ASSERT_TRUE(store.append(entries));
  }
  {
    // 同一个注入手法：在活动 WAL 尾部追加垃圾（kill -9 半条 record 的等价物）。
    // 区别只在「谁来截断」：file 引擎是 FileLogStore::load() 的 CRC 扫描，
    // lsm 引擎是 DB::Open 的恢复路径（并把字节数记进 RecoveryStats::tail_truncated_bytes）。
    const std::string wal = firstWalPath(dir + "/raft-lsm");
    ASSERT_FALSE(wal.empty());
    std::ofstream out(wal, std::ios::binary | std::ios::app);
    out.write(kTornJunk, sizeof(kTornJunk) - 1);
  }
  {
    LsmLogStore store(dir);
    Term term = 0;
    int votedFor = -1;
    Index lastIndex = 0;
    ASSERT_TRUE(store.load(term, votedFor, lastIndex));
    EXPECT_EQ(lastIndex, 1) << "torn tail must be truncated to the last valid record";
    // lsm 侧的额外可观测性（file 引擎没有这个计数）：O8/D8 的采集点。
    EXPECT_GT(store.walTailTruncatedBytes(), 0u);
  }
  std::filesystem::remove_all(dir);
}
