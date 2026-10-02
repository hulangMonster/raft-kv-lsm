// LsmLogStore：用本项目的 lsm 引擎承载 raft 的日志 + term/votedFor。
//
// 规格：docs/m6-design.md §2（位级布局 / 逐方法等价性表 / 内部状态）。
// 物理隔离：数据在 <dir>/raft-lsm（FileLogStore 是 <dir>/raft）。
// 键空间（同一 lsm 库）：
//   0x01 || BE64(index)   日志条目（固定 9 字节 user key）
//   0x02 "meta"           term/votedFor（固定 5 字节 user key）
// 值编码：日志条目 = log_entry_codec.h 的 encodeEntry()（与 FileLogStore 逐字相同）；
//         meta = encodeMeta()（12 字节）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "raft/log_store.h"

namespace lsm {
class DB;
}  // namespace lsm

namespace raftkv::raft {

class LsmLogStore : public LogStore {
 public:
  static constexpr uint8_t kLogPrefix = 0x01;
  static constexpr const char kMetaKey[] = "\x02" "meta";  // 5 字节
  static constexpr size_t kMetaKeyLen = 5;
  static constexpr size_t kEntryKeyLen = 9;

  // 键编码助手（公开以便单测直接钉住位级布局，设计 §4-M6.2）。
  static std::string entryKey(Index index);          // 0x01 || BE64(index)
  static std::string metaKey() { return std::string(kMetaKey, kMetaKeyLen); }
  static bool isLogKey(const std::string& key);      // key.size()==9 && key[0]==0x01
  static bool decodeEntryKey(const std::string& key, Index* index);

  // 本构建是否把 lsm 链接了进来（设计 §8 D8：没有就明确报错，绝不静默降级）。
  static bool haveLsm();
  // 不可用时的统一错误文案（构造期抛出，包含修复提示）。
  static const char* unavailableReason();

  explicit LsmLogStore(std::string dir);  // Open 失败 ⇒ throw std::runtime_error
  ~LsmLogStore() override;                // Close（隐含 Sync）+ 释放 LOCK

  bool load(Term& term, int& votedFor, Index& lastIndex) override;
  bool persistMeta(Term term, int votedFor) override;
  bool append(const std::vector<LogEntry>& entries) override;
  bool appendNoSync(const std::vector<LogEntry>& entries) override;
  bool sync() override;
  bool truncateSuffix(Index fromIndex) override;
  bool truncateSuffixNoSync(Index fromIndex) override;  // ★ 必须覆写（log_store.h L48-49）
  std::vector<LogEntry> slice(Index from, size_t maxEntries,
                              size_t maxBytes) const override;
  Index lastIndex() const override;
  Term lastTerm() const override;
  Term termAt(Index index) const override;
  void setBoundary(Index lastIncludedIndex, Term lastIncludedTerm) override;
  bool compact(Index upTo, Term termAtUpTo) override;
  Index firstIndex() const override;
  Index lastIncludedIndex() const override;
  Term lastIncludedTerm() const override;

  // 仅测试/诊断（与 MemoryLogStore::all() 同形的 seam）：逻辑日志 [firstIndex, lastIndex]。
  std::vector<LogEntry> all() const;

  // 诊断计数（口径对齐设计 §2.5；「任何丢弃/跳过/截断/删除都必须有一个计数落点」）。
  struct Stats {
    uint64_t loads = 0;
    uint64_t appends = 0;            // append/appendNoSync 调用次数
    uint64_t appended_entries = 0;
    uint64_t syncs = 0;
    uint64_t truncates = 0;          // 实际删除条目的 truncate 次数（E1-E4 no-op 不计；M6.10 N9 口径）
    uint64_t truncated_entries = 0;  // 被删除的索引条数（R3 的写放大口径）
    uint64_t compacts = 0;
    uint64_t compacted_entries = 0;
    uint64_t meta_persists = 0;
    uint64_t slice_calls = 0;
    uint64_t slice_decode_errors = 0;
    uint64_t wal_tail_truncated_bytes = 0;  // lsm RecoveryStats::tail_truncated_bytes（O8）
    bool poisoned = false;
  };
  Stats stats() const;
  uint64_t walTailTruncatedBytes() const;

  // M6.r2（设计 §5.2 的 O5/O6/O7）：lsm 引擎内部的**只读**统计，拼成一行 `k=v ...`，
  // 由 node 的 `status` 原样输出（file 引擎 / 未配置 lsm 时返回空串）。
  // 口径：LevelStats（各层文件数/字节）、FlushStats（flush/stall/WAL 轮转）、
  //       AmplificationStats（flush/compaction 写字节、compaction 轮次与最大耗时）。
  // 只做计数上报，不参与任何判定（与 Metrics 同纪律）。
  std::string engineStatsFragment() const;

 private:
  // 调用方必须持 mu_（**不得**在持锁时调用公共访问器：它们是 non-recursive mutex）。
  Index firstIndexLocked() const { return lastIncluded_ + 1; }
  bool appendNoSyncLocked(const std::vector<LogEntry>& entries);
  bool truncateLocked(Index fromIndex, bool flush);
  bool deleteRangeLocked(Index from, Index to, bool sync);
  Term termAtLocked(Index index) const;

  // 让头文件不必包含 lsm 的 db.h（未配置 lsm 时也能编译）。
  struct LsmDbDeleter {
    void operator()(lsm::DB* db) const noexcept;
  };

  std::string dir_;
  std::unique_ptr<lsm::DB, LsmDbDeleter> db_;
  mutable std::mutex mu_;
  Term term_ = kNoTerm;
  int votedFor_ = -1;
  Index lastIncluded_ = kNoIndex;
  Term lastIncludedTerm_ = kNoTerm;
  // terms_[i - firstIndex()] == 索引 i 的 term，仅在 [firstIndex(), lastIndex_] 有意义。
  std::vector<Term> terms_;
  Index lastIndex_ = kNoIndex;
  Term lastTerm_ = kNoTerm;
  bool poisoned_ = false;
  // 诊断计数（sync()/appendNoSync() 的计数点在不同锁上下文，用原子避免额外锁）。
  mutable std::atomic<uint64_t> stat_loads_{0};
  mutable std::atomic<uint64_t> stat_appends_{0};
  mutable std::atomic<uint64_t> stat_appended_entries_{0};
  mutable std::atomic<uint64_t> stat_syncs_{0};
  mutable std::atomic<uint64_t> stat_truncates_{0};
  mutable std::atomic<uint64_t> stat_truncated_entries_{0};
  mutable std::atomic<uint64_t> stat_compacts_{0};
  mutable std::atomic<uint64_t> stat_compacted_entries_{0};
  mutable std::atomic<uint64_t> stat_meta_persists_{0};
  mutable std::atomic<uint64_t> stat_slice_calls_{0};
  mutable std::atomic<uint64_t> stat_slice_decode_errors_{0};
  mutable std::atomic<uint64_t> stat_wal_tail_truncated_bytes_{0};
};

}  // namespace raftkv::raft
