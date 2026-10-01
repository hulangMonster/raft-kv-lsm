// M6.2：LsmLogStore 语义单测（**不接 Raft**）。
//
// 每条用例对齐 docs/m6-design.md §2.4 的等价性表中的一条论证，或 §4-M6.2 的用例清单。
// 断言口径与 FileLogStore 的既有用例（tests/raft_restart_test.cpp）保持一致。
// 纪律：没有空断言用例；未配置 lsm 的构建下，本文件只剩一条「必须明确失败」的用例
// （不是 GTEST_SKIP —— 那条用例真的在断言 haveLsm()==false 且构造抛异常）。
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "raft/log_entry_codec.h"
#include "raft/log_store.h"
#include "raft/lsm_log_store.h"

#ifdef RAFTK_HAVE_LSM
#include "db.h"
#include "db_impl.h"
#include "write_batch.h"
#endif

using namespace raftkv::raft;

namespace {

std::string tempDir() {
  static uint64_t counter = 0;
  auto p = std::filesystem::temp_directory_path() /
           ("raftkv_lsm_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++));
  std::filesystem::create_directories(p);
  return p.string();
}

#ifdef RAFTK_HAVE_LSM

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

// 解析 FileLogStore 的 raft.log：重复的 [crc:4][len:4][payload]，全大端。
std::vector<std::string> readLogPayloads(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream in(path, std::ios::binary);
  std::string all((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  size_t off = 0;
  while (off + 8 <= all.size()) {
    const auto* p = reinterpret_cast<const unsigned char*>(all.data() + off);
    const uint32_t len = (static_cast<uint32_t>(p[4]) << 24) |
                         (static_cast<uint32_t>(p[5]) << 16) |
                         (static_cast<uint32_t>(p[6]) << 8) |
                         static_cast<uint32_t>(p[7]);
    if (off + 8 + len > all.size()) break;
    out.emplace_back(all.data() + off + 8, len);
    off += 8 + len;
  }
  return out;
}
#endif

}  // namespace

#ifdef RAFTK_HAVE_LSM

// ---- §2.1 键编码 ------------------------------------------------------------

TEST(LsmLogStore, KeyEncodingIsBytewiseIndexOrdered) {
  const Index idx[] = {0,    1,      2,      255,     256,
                       65535, 65536, 1ull << 32, ~0ull};
  const size_t n = sizeof(idx) / sizeof(idx[0]);
  for (size_t i = 0; i < n; ++i) {
    const std::string k = LsmLogStore::entryKey(idx[i]);
    ASSERT_EQ(k.size(), LsmLogStore::kEntryKeyLen);
    EXPECT_EQ(static_cast<uint8_t>(k[0]), LsmLogStore::kLogPrefix);
    EXPECT_TRUE(LsmLogStore::isLogKey(k));
    Index back = 12345;
    EXPECT_TRUE(LsmLogStore::decodeEntryKey(k, &back));
    EXPECT_EQ(back, idx[i]);
    if (i + 1 < n) {
      EXPECT_LT(k.compare(LsmLogStore::entryKey(idx[i + 1])), 0)
          << "bytewise order must equal numeric order at i=" << i;
    }
  }
}

TEST(LsmLogStore, MetaKeyIsOutsideLogRange) {
  const std::string m = LsmLogStore::metaKey();
  EXPECT_EQ(m.size(), LsmLogStore::kMetaKeyLen);
  EXPECT_EQ(static_cast<uint8_t>(m[0]), 0x02u);
  EXPECT_EQ(m, std::string("\x02" "meta", 5));
  EXPECT_FALSE(LsmLogStore::isLogKey(m));
  const Index probes[] = {0, 1, 255, 65536, 1ull << 40, ~0ull};
  for (Index i : probes) {
    EXPECT_LT(LsmLogStore::entryKey(i).compare(m), 0)
        << "every log key must sort before the meta key (i=" << i << ")";
  }
}

// ---- §2.4.1 重启恢复 --------------------------------------------------------
// 注意（M6.3）：`LsmLogStore.RestartRestoresMetaAndLog` **不在这里** —— 它已按设计 §4-M6.3
// 搬到 tests/raft_restart_test.cpp，与 `FileLogStore.RestartRestoresMetaAndLog` 共用同一份
// 用例体（两个引擎、逐字相同的断言文本）。同名重复定义会撞链接，且共享体比两份拷贝更强。

// ---- §2.4.3 append 的 durable + 立即可见 ------------------------------------

TEST(LsmLogStore, AppendIsDurableAndVisible) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    entries.push_back(entry(1, 3, "a", "x"));
    entries.push_back(entry(2, 3, "b", "yy"));
    ASSERT_TRUE(store.append(entries));
    // 返回即已 durable，且立刻对 slice/lastIndex/termAt 可见。
    EXPECT_EQ(store.lastIndex(), 2u);
    EXPECT_EQ(store.lastTerm(), 3u);
    EXPECT_EQ(store.termAt(1), 3u);
    auto got = store.slice(1, 10, SIZE_MAX);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].key, "a");
    EXPECT_EQ(got[1].value, "yy");
  }
  {
    LsmLogStore store(dir);  // 重启（append 已 fsync）
    Term t = 0;
    int v = -1;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    EXPECT_EQ(li, 2u);
    EXPECT_EQ(store.termAt(2), 3u);
  }
  std::filesystem::remove_all(dir);
}

// ---- §2.4.4 appendNoSync 可见但不 durable；sync() 后 durable -----------------

TEST(LsmLogStore, AppendNoSyncIsVisibleButSurvivesSync) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    ASSERT_TRUE(store.appendNoSync({entry(1, 1, "a", "1")}));
    // 契约（log_store.h L32-33）：appendNoSync 之后必须对 slice/lastIndex 可见。
    EXPECT_EQ(store.lastIndex(), 1u);
    EXPECT_EQ(store.slice(1, 1, SIZE_MAX).size(), 1u);
    ASSERT_TRUE(store.appendNoSync({entry(2, 1, "b", "2")}));
    ASSERT_TRUE(store.sync());
    EXPECT_EQ(store.stats().syncs, 1u);
  }
  {
    LsmLogStore store(dir);
    Term t = 0;
    int v = -1;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    EXPECT_EQ(li, 2u) << "sync() must have made both entries durable";
  }
  std::filesystem::remove_all(dir);
}

TEST(LsmLogStore, IdempotentAppendSameIndexSameTerm) {
  const std::string dir = tempDir();
  LsmLogStore store(dir);
  std::vector<LogEntry> entries;
  entries.push_back(entry(1, 1, "a", "1"));
  entries.push_back(entry(2, 1, "b", "2"));
  ASSERT_TRUE(store.append(entries));
  // 同 index 同 term 重复 append：不报错、不重复、内容不变。
  ASSERT_TRUE(store.append(entries));
  EXPECT_EQ(store.lastIndex(), 2u);
  EXPECT_EQ(store.all().size(), 2u);
  EXPECT_EQ(store.slice(1, 10, SIZE_MAX)[1].key, "b");
  std::filesystem::remove_all(dir);
}

// ---- §2.4.4/E5 冲突覆盖：同 index 异 term --------------------------------

TEST(LsmLogStore, ConflictTruncateThenAppend) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> first;
    first.push_back(entry(1, 1, "a", "1"));
    first.push_back(entry(2, 1, "b", "2"));
    first.push_back(entry(3, 1, "c", "3"));
    ASSERT_TRUE(store.append(first));
    // 冲突：index 2 换 term，连带截断 2..3 再追加 2..3（新 term）。
    std::vector<LogEntry> overwrite;
    overwrite.push_back(entry(2, 5, "b", "NEW"));
    overwrite.push_back(entry(3, 5, "c", "NEW3"));
    ASSERT_TRUE(store.append(overwrite));
    EXPECT_EQ(store.termAt(2), 5u);
    EXPECT_EQ(store.termAt(3), 5u);
    EXPECT_EQ(store.lastIndex(), 3u);
    auto got = store.slice(1, 10, SIZE_MAX);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[1].value, "NEW");
    EXPECT_EQ(store.stats().truncated_entries, 2u);
  }
  {
    LsmLogStore store(dir);  // 重启后必须是新值（Delete 与 Put 都在 durable 批里）
    Term t = 0;
    int v = -1;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    ASSERT_EQ(li, 3u);
    auto got = store.slice(1, 10, SIZE_MAX);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].value, "1");
    EXPECT_EQ(got[1].value, "NEW");
    EXPECT_EQ(got[2].value, "NEW3");
    EXPECT_EQ(store.termAt(2), 5u);
  }
  std::filesystem::remove_all(dir);
}

// ---- §2.4.6 E1-E4 截断边界 ------------------------------------------------

TEST(LsmLogStore, TruncateSuffixBoundaries) {
  const std::string dir = tempDir();
  LsmLogStore store(dir);
  std::vector<LogEntry> entries;
  for (Index i = 1; i <= 4; ++i) {
    entries.push_back(entry(i, 1, "k" + std::to_string(i), "v"));
  }
  ASSERT_TRUE(store.append(entries));
  EXPECT_TRUE(store.truncateSuffix(kNoIndex)) << "E1: kNoIndex ⇒ no-op true";
  EXPECT_EQ(store.lastIndex(), 4u);
  EXPECT_TRUE(store.truncateSuffix(5)) << "E4: lastIndex+1 ⇒ true 且无变化";
  EXPECT_EQ(store.lastIndex(), 4u);
  EXPECT_FALSE(store.truncateSuffix(6)) << "E3: > lastIndex+1 ⇒ false";
  EXPECT_TRUE(store.truncateSuffix(3));
  EXPECT_EQ(store.lastIndex(), 2u);
  EXPECT_EQ(store.termAt(3), kNoTerm);
  // E2：先建立边界（compact 到 2），再试图切到边界之下 ⇒ false。
  EXPECT_TRUE(store.compact(2, 1));
  EXPECT_EQ(store.firstIndex(), 3u);
  EXPECT_EQ(store.lastIndex(), 2u);
  EXPECT_FALSE(store.truncateSuffix(2)) << "E2: <= lastIncluded_ ⇒ false";
  EXPECT_FALSE(store.truncateSuffix(1)) << "E2: 更靠下的索引同样 false";
  EXPECT_TRUE(store.truncateSuffix(3)) << "== lastIndex_+1 ⇒ true 且无变化";
  EXPECT_EQ(store.lastIndex(), 2u);
  std::filesystem::remove_all(dir);
}

TEST(LsmLogStore, TruncateNoSyncThenSyncIsDurable) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    for (Index i = 1; i <= 5; ++i) entries.push_back(entry(i, 1, "k", "v"));
    ASSERT_TRUE(store.append(entries));
    ASSERT_TRUE(store.truncateSuffixNoSync(4));
    EXPECT_EQ(store.lastIndex(), 3u) << "截断必须立刻可见";
    ASSERT_TRUE(store.sync());
  }
  {
    LsmLogStore store(dir);
    Term t = 0;
    int v = -1;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    EXPECT_EQ(li, 3u) << "同一批的 sync() 必须持久化截断";
    EXPECT_EQ(store.termAt(4), kNoTerm);
  }
  std::filesystem::remove_all(dir);
}

// ---- §2.4.6 崩溃窗口：NoSync 之后进程死掉（不析构、不 sync）----------------
//
// ⚠️ 偏差登记（见 docs/m6-evidence.md §M6.2）：设计 §4-M6.2 期望「后缀复活」。
// 本 VM 上页缓存会存活 ⇒ kill -9 后截断**仍然可见**（这正是设计 §3.1 矩阵里
// 「appendNoSync：本 VM 实测必存活（page cache）」那一行的同一现象）。
// 「复活」只可能发生在掉电（页缓存丢失），测试装置无法在不引入假文件系统的前提下构造。
// 因此本用例断言**实测行为**，并把「掉电语义未覆盖」写入未验证清单。
TEST(LsmLogStore, TruncateNoSyncIntoKillDashNineKeepsDeletionInPageCache) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    for (Index i = 1; i <= 5; ++i) entries.push_back(entry(i, 1, "k", "v"));
    ASSERT_TRUE(store.append(entries));
  }
  const pid_t pid = ::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // 子进程：截断但不 fsync，然后 **不跑析构** 直接退出（= 崩溃语义）。
    bool ok = false;
    try {
      LsmLogStore child(dir);
      Term t = 0;
      int v = -1;
      Index li = 0;
      // 与 RaftNode 的启动顺序一致：先 load() 再变更（内存态来自 load）。
      ok = child.load(t, v, li) && li == 5 && child.truncateSuffixNoSync(4);
    } catch (...) {
      ok = false;
    }
    ::_exit(ok ? 0 : 3);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0) << "child failed to truncate";
  {
    LsmLogStore store(dir);
    Term t = 0;
    int v = -1;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    EXPECT_EQ(li, 3u) << "页缓存存活 ⇒ kill -9 下截断仍可见（与 §3.1 矩阵一致）";
  }
  std::filesystem::remove_all(dir);
}

// ---- §2.4.7 slice 的 clamp / maxEntries / maxBytes(至少 1 条) --------------

TEST(LsmLogStore, SliceClampsAndHonoursLimits) {
  const std::string dir = tempDir();
  LsmLogStore store(dir);
  std::vector<LogEntry> entries;
  for (Index i = 1; i <= 5; ++i) {
    entries.push_back(entry(i, 1, "k", std::string(100, 'x')));  // 100B value
  }
  ASSERT_TRUE(store.append(entries));
  ASSERT_TRUE(store.compact(2, 1));  // firstIndex == 3

  auto clamped = store.slice(1, 10, SIZE_MAX);  // from < firstIndex ⇒ clamp 到 3
  ASSERT_EQ(clamped.size(), 3u);
  EXPECT_EQ(clamped[0].index, 3u);

  auto limited = store.slice(3, 2, SIZE_MAX);
  ASSERT_EQ(limited.size(), 2u);
  EXPECT_EQ(limited[0].index, 3u);
  EXPECT_EQ(limited[1].index, 4u);

  // maxBytes 口径 = key.size()+value.size()，且「只要有可用条目就至少返回 1 条」（L404）。
  auto one = store.slice(3, 10, 1);
  ASSERT_EQ(one.size(), 1u);
  EXPECT_EQ(one[0].index, 3u);

  EXPECT_TRUE(store.slice(6, 10, SIZE_MAX).empty());  // start > lastIndex
  EXPECT_TRUE(store.slice(3, 0, SIZE_MAX).empty());   // maxEntries == 0
  std::filesystem::remove_all(dir);
}

// ---- §2.4.9 + §2.4.1 R2：compact 后前缀不可见且能重启 ----------------------

TEST(LsmLogStore, CompactDropsPrefixAndSurvivesRestart) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    for (Index i = 1; i <= 5; ++i) {
      entries.push_back(entry(i, (i <= 3 ? 1 : 2), "k" + std::to_string(i), "v"));
    }
    ASSERT_TRUE(store.append(entries));
    ASSERT_TRUE(store.compact(3, 1));  // upTo=3, term@3=1
    EXPECT_EQ(store.firstIndex(), 4u);
    EXPECT_EQ(store.lastIndex(), 5u);
    EXPECT_EQ(store.lastIncludedTerm(), 1u);
  }
  {
    // 重启：RaftNode 的顺序 = 快照 load → setBoundary → log.load（D3）。
    LsmLogStore store(dir);
    store.setBoundary(3, 1);
    Term t = 0;
    int v = -1;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    EXPECT_EQ(store.firstIndex(), 4u);
    EXPECT_EQ(li, 5u);
    EXPECT_EQ(store.termAt(3), 1u);  // 边界条目
    auto got = store.slice(4, 10, SIZE_MAX);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].index, 4u);
    EXPECT_EQ(got[1].index, 5u);
  }
  std::filesystem::remove_all(dir);
}

// ---- §2.4.1 R4：前缀没了但快照没恢复 ⇒ 拒绝启动（对应 FileLogStore L223-228）--

TEST(LsmLogStore, PrefixGoneWithoutSnapshotRefusesLoad) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    for (Index i = 1; i <= 5; ++i) entries.push_back(entry(i, 1, "k", "v"));
    ASSERT_TRUE(store.append(entries));
    ASSERT_TRUE(store.compact(3, 1));
  }
  {
    LsmLogStore store(dir);  // 故意不调 setBoundary（= 快照丢了）
    Term t = 0;
    int v = -1;
    Index li = 0;
    EXPECT_FALSE(store.load(t, v, li))
        << "compacted prefix without a snapshot must refuse to start";
  }
  std::filesystem::remove_all(dir);
}

// ---- §2.4.1 R3：边界之下的残留被跳过 ---------------------------------------

TEST(LsmLogStore, LoadSkipsEntriesBelowBoundary) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    for (Index i = 1; i <= 5; ++i) entries.push_back(entry(i, 1, "k", "v"));
    ASSERT_TRUE(store.append(entries));
  }
  {
    LsmLogStore store(dir);
    store.setBoundary(2, 1);  // 快照已 durable、compact 尚未跑
    Term t = 0;
    int v = -1;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    EXPECT_EQ(store.firstIndex(), 3u);
    EXPECT_EQ(li, 5u);
    auto got = store.slice(3, 10, SIZE_MAX);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].index, 3u);
  }
  std::filesystem::remove_all(dir);
}

// ---- §2.4.1 R5：空日志 + 边界 ---------------------------------------------

TEST(LsmLogStore, EmptyLogWithBoundary) {
  const std::string dir = tempDir();
  LsmLogStore store(dir);
  store.setBoundary(7, 3);
  Term t = 0;
  int v = -1;
  Index li = 0;
  ASSERT_TRUE(store.load(t, v, li));
  EXPECT_EQ(store.lastIndex(), 7u);
  EXPECT_EQ(store.lastTerm(), 3u);
  EXPECT_EQ(store.firstIndex(), 8u);
  EXPECT_EQ(store.lastIncludedIndex(), 7u);
  EXPECT_EQ(store.lastIncludedTerm(), 3u);
  EXPECT_EQ(store.termAt(7), 3u);
  EXPECT_EQ(store.termAt(6), kNoTerm);
  EXPECT_TRUE(store.slice(8, 10, SIZE_MAX).empty());
  std::filesystem::remove_all(dir);
}

// ---- §2.4.2 meta durable ---------------------------------------------------

TEST(LsmLogStore, MetaPersistIsAtomicAndDurable) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    ASSERT_TRUE(store.persistMeta(9, 3));
    ASSERT_TRUE(store.persistMeta(10, -1));  // 覆盖写：votedFor 的位模式
  }
  {
    LsmLogStore store(dir);
    Term t = 0;
    int v = 12345;
    Index li = 0;
    ASSERT_TRUE(store.load(t, v, li));
    EXPECT_EQ(t, 10u);
    EXPECT_EQ(v, -1);
    EXPECT_EQ(store.stats().meta_persists, 0u) << "统计只算本次会话";
  }
  std::filesystem::remove_all(dir);
}

// ---- D10：LOCK 的**真实**作用域是「跨进程」（fcntl F_SETLK）----------------
// 偏差登记（见 evidence §M6.0-D4）：同进程二次 Open 不冲突，所以这里用 fork 验证。
TEST(LsmLogStore, SecondProcessOnSameDirIsRejectedByLock) {
  const std::string dir = tempDir();
  {
    LsmLogStore parent(dir);
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
      bool threw = false;
      try {
        LsmLogStore child(dir);  // 父进程仍持 LOCK ⇒ 必须失败
      } catch (const std::exception&) {
        threw = true;
      }
      ::_exit(threw ? 0 : 3);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "第二个进程的 Open 必须被 lsm 的 LOCK 拒绝";
  }
  std::filesystem::remove_all(dir);
}

// ---- D5：中间空洞 ⇒ 拒绝启动（直连 lsm 删掉中间一个 key 制造空洞）----------

TEST(LsmLogStore, LoadRefusesGapInTheMiddle) {
  const std::string dir = tempDir();
  {
    LsmLogStore store(dir);
    std::vector<LogEntry> entries;
    for (Index i = 1; i <= 3; ++i) entries.push_back(entry(i, 1, "k", "v"));
    ASSERT_TRUE(store.append(entries));
  }
  {
    lsm::DB* raw = nullptr;
    ASSERT_TRUE(lsm::DB::Open(lsm::Options(), dir + "/raft-lsm", &raw).ok());
    std::unique_ptr<lsm::DB> db(raw);
    lsm::WriteBatch wb;
    const std::string k = LsmLogStore::entryKey(2);
    wb.Delete(lsm::Slice(k));
    lsm::WriteOptions wo;
    wo.sync = true;
    ASSERT_TRUE(db->Write(wo, &wb).ok());
    ASSERT_TRUE(db->Close().ok());
  }
  {
    LsmLogStore store(dir);
    Term t = 0;
    int v = -1;
    Index li = 0;
    EXPECT_FALSE(store.load(t, v, li)) << "D5: 空洞必须拒绝启动，不得静默截断";
  }
  std::filesystem::remove_all(dir);
}

// ---- §3.3/对齐点 1：撕裂 WAL 尾由 DB::Open 截断，并被计数（O8/D8）----------

// ---- §3.3/对齐点 1：撕裂 WAL 尾由 DB::Open 截断，并被计数（O8/D8）----------
//
// 注意（M6.3）：与 `FileLogStore.TruncatesTornTail` 同形的孪生用例已按设计 §4-M6.3
// 落到 tests/raft_restart_test.cpp（`LsmLogStore.TruncatesTornTail`，同注入手法 + 同断言文本）。
// 这里不再保留第二份拷贝，避免同一注入重复维护。

// ---- M6.0-D3 的回归守卫：lsm 存下的 value 必须与 FileLogStore 写盘的 payload
//      逐字节相同（两个引擎共用 log_entry_codec.h 的同一份实现）。----------------

TEST(LsmLogStore, ValueBytesMatchFileLogStoreOnDisk) {
  std::vector<LogEntry> entries;
  entries.push_back(entry(1, 1, "alpha", ""));
  entries.push_back(entry(2, 7, "beta", std::string(300, 'z')));
  LogEntry cfg = entry(3, 7, "", "cfg");
  cfg.op = raftkv::OpCode::kConfig;
  cfg.key = "1=a";  // 配置条目也走同一条编码（op 白名单含 kConfig）
  entries.push_back(cfg);

  const std::string dirA = tempDir();
  {
    FileLogStore store(dirA);
    ASSERT_TRUE(store.append(entries));
  }
  const std::vector<std::string> filePayloads =
      readLogPayloads(dirA + "/raft/raft.log");
  ASSERT_EQ(filePayloads.size(), entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const raftkv::Bytes enc = encodeEntry(entries[i]);
    EXPECT_EQ(filePayloads[i], std::string(enc.begin(), enc.end()))
        << "FileLogStore 的磁盘 payload 必须与共享 codec 逐字节相同 (i=" << i << ")";
  }

  const std::string dirB = tempDir();
  {
    LsmLogStore store(dirB);
    ASSERT_TRUE(store.append(entries));
  }
  lsm::DB* raw = nullptr;
  ASSERT_TRUE(lsm::DB::Open(lsm::Options(), dirB + "/raft-lsm", &raw).ok());
  std::unique_ptr<lsm::DB> db(raw);
  std::unique_ptr<lsm::Iterator> it(db->NewIterator());
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const lsm::Slice k = it->key();
    if (k.size() != LsmLogStore::kEntryKeyLen) break;  // meta key 之后
    ASSERT_LT(n, filePayloads.size());
    EXPECT_EQ(std::string(it->value().data(), it->value().size()), filePayloads[n])
        << "lsm 的 value 必须与 FileLogStore 的 payload 逐字节相同 (n=" << n << ")";
    ++n;
  }
  EXPECT_EQ(n, filePayloads.size());
  ASSERT_TRUE(db->Close().ok());

  std::filesystem::remove_all(dirA);
  std::filesystem::remove_all(dirB);
}

// ---- M6.r3：**真正的多线程**用例（此前 LsmLogStore 的用例都是单线程的，
//      「TSan 窄面 0 报告」不足以支撑并发结论 —— 见 docs/m6-evidence.md 的限制清单）。
//
// 负载形状：1 个写者（appendNoSync 批 + 周期性 sync）+ 3 个读者（lastIndex/termAt/slice 并发）。
// 不变式：任何时刻 `slice()` 返回的条目，其 `termAt(index)` 必须等于该条目的 term
// （没有并发 truncate/compact 时 term 不会变；这条一致性由 store 的 mu_ 保证）。
TEST(LsmLogStore, ConcurrentAppendSyncAndReadersStayConsistent) {
  const std::string dir = tempDir();
  LsmLogStore store(dir);
  constexpr Index kBatches = 400;   // 400 批 × 5 条 = 2000 条
  constexpr size_t kBatch = 5;
  std::atomic<bool> done{false};
  std::atomic<int> write_failures{0};
  std::atomic<size_t> inconsistent{0};

  std::thread writer([&] {
    Index next = 1;
    for (Index b = 0; b < kBatches; ++b) {
      std::vector<LogEntry> es;
      for (size_t i = 0; i < kBatch; ++i) {
        es.push_back(entry(next, static_cast<Term>(1 + (next % 3)),
                           "k" + std::to_string(next), "v"));
        ++next;
      }
      if (!store.appendNoSync(es)) { ++write_failures; break; }
      if (b % 4 == 3 && !store.sync()) { ++write_failures; break; }
    }
    done.store(true);
  });

  auto reader = [&] {
    size_t local_bad = 0;
    while (!done.load()) {
      const Index last = store.lastIndex();
      if (last > 0) (void)store.termAt(1 + (last % 16));
      auto s = store.slice(1 + (last % 8), 4, SIZE_MAX);
      for (const LogEntry& e : s) {
        if (store.termAt(e.index) != e.term) ++local_bad;
      }
    }
    inconsistent.fetch_add(local_bad);
  };
  std::thread r1(reader), r2(reader), r3(reader);
  writer.join();
  r1.join();
  r2.join();
  r3.join();

  EXPECT_EQ(write_failures.load(), 0) << "并发下写入方不得失败";
  EXPECT_EQ(inconsistent.load(), 0u)
      << "slice() 的条目必须与 termAt() 一致（store 的 mu_ 下的同一份状态）";
  EXPECT_EQ(store.lastIndex(), kBatches * kBatch);
  Term t = 0;
  int v = -1;
  Index li = 0;
  ASSERT_TRUE(store.load(t, v, li));
  EXPECT_EQ(li, kBatches * kBatch);

  std::filesystem::remove_all(dir);
}

#else  // !RAFTK_HAVE_LSM

// 未配置 lsm 的构建：本文件只保留这一条**真断言**的用例（不是 GTEST_SKIP）。
TEST(LsmLogStore, WithoutLsmSupportConstructionFailsLoudly) {
  const std::string dir = tempDir();
  EXPECT_FALSE(LsmLogStore::haveLsm());
  EXPECT_NE(LsmLogStore::unavailableReason(), nullptr);
  EXPECT_THROW(static_cast<void>(LsmLogStore(dir)), std::runtime_error);
  std::filesystem::remove_all(dir);
}

#endif  // RAFTK_HAVE_LSM
