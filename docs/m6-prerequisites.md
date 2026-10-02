# M6 前置校验（#1 /verification-before-completion）——事前约束的事后逐条复核

> 对应设计：[m6-design.md](m6-design.md) **v1.0**（接口映射 + 崩溃/持久化契约 + A/B 方案）。
> 风格参照 `~/raft-kv/docs/m5-prerequisites.md`（raft-kv 的同名同类产出）。
>
> **诚实前置声明（必读）**
> 1. 本文件是 **事后补写**：M6 代码已经实现并提交，复核基线是 **写本文件时的当前 HEAD**
>    ```~/raft-kv-lsm @ 924492e5a10e7a3518a92b4ab75e57de93ab4045```（```origin/main``` 同，工作树干净）。
>    它不是“实现前冻结的约束”，而是“对已落地实现逐条回看是否满足当初约束”。
> 2. 本轮 **不跑任何构建 / 门禁 / ASan / TSan / 基准**（机器留给后续实现任务）。因此：
>    凡是没有**可当场复读的原始输出**支撑的条目，一律标为 **仅论证无测试** 或 **未复核**；
>    本文 **不因为某条看起来“应该成立”就写成“成立”**。
> 3. 每条复核都给出证据锚点：`file:line` / **用例名** / **命令 + 原文输出**。锚点来自本仓
>    HEAD、`~/raft-kv`（只读）、`~/lsm-kv`（只读）与两仓的 `docs/`。
> 4. 标注口径（四选一，逐条给）：**仍成立 / 被替换 / 需加强 / 未复核**。
>    “仅论证无测试”是“仍成立”的一种证据强度，单独注明；没有任何证据的写 **未复核**。
> 5. 本文件是 M6 的第 1 号交付物（`#1 /verification-before-completion`），**不改任何代码**。

> **提交时点备注（2026-10-02）**：本文件起草/复核时 HEAD 为 `924492e`、工作树干净；到**提交时点**
> HEAD 已前进到 `a8eb330`（仅新增 `docs/m6-design.md` §10 的**文档**提交，代码面无变化），
> 且工作区出现了**另一个并发 agent 的未提交改动**（`M CMakeLists.txt`、`?? tests/raft_sm_contract_test.cpp`、
> `?? tests/sm_contract_body.h`）。本文件**只新增 `docs/m6-prerequisites.md` 一个文件**，
> 未 add / 未 commit / 未触碰上述并发改动。§1–§8 的代码锚点与 `git diff` 数字均以 `924492e` 时点为准；
> 该时点至 `a8eb330` 之间**代码文件无变化**（`a8eb330` 只改 `docs/m6-design.md`）。

---

## §0 复核方法与“哪些是本轮实读、哪些是转述”

| 手段 | 本轮做了 | 说明 |
|---|---|---|
| 读代码 | ✅ | `lsm_log_store.{h,cpp}`、`log_entry_codec.h`、`log_store.h`、`raft_node.cpp`（只读）、`lock_probe.h`、`tests/*`、`scripts/*`、`CMakeLists.txt` |
| `git diff` 对账 | ✅ | `git diff --stat 6aabc27..HEAD`、`git diff 6aabc27..HEAD -- <files>`、`git diff -U0 1463620..HEAD -- tests/` |
| 读设计/证据文档 | ✅ | `docs/m6-design.md`、`docs/m6-evidence.md`、`docs/m6-bench.md`、`~/raft-kv/docs/m5-*.md` |
| 读 lsm 侧 | ✅（只读） | 本轮复核时 HEAD `f06a44d`；M6 当初链接的 pin `b1bd050` 曾随 /tmp 清空丢失（见 §7）。**M6.10 收口时 `~/lsm-kv` HEAD = `51c4672`（= 最终 pin `/tmp/lsm-pin-51c4672`）** |
| 构建 / 跑测试 / 跑 sanitizer / 跑基准 | ❌ | **本轮明确不跑**；因此所有“绿/通过/0 报告”都来自**当时写入 `m6-evidence.md`/`m6-bench.md` 的原文**，属转述证据 |
| 读 `~/raft-kv`/`~/lsm-kv` 的 git | ✅（只读） | 未对两仓做任何 git 写操作 |

> **“转述证据”的含义**：本轮能读到 `m6-evidence.md`/`m6-bench.md` 里**当时粘贴的原始行**
> （如 `[  PASSED  ] 122 tests.`），但**这些行对应的 /tmp 日志已丢**（见 §7），且本轮未复跑。
> 因此这类条目在本文里标注为 **“有文档内嵌原文，但原始日志已丢；本轮未复现”**。

---

## §1 不变量逐条复核（M6-I1 … M6-I12）

> 复核方法：先看该不变量在 M6 里**由谁承载**，再看是否有“代码 + 用例 + 原始输出”三重证据。
> **M6 的关键事实**：`raft_node.cpp` / `raft_node.h` / `log_store.h` / `src/kv/**` 在整个 M6
> **一行未改**（对 `raft_node.cpp` / `raft_node.h` / `log_store.h` / `src/kv/` 的 diff 输出为空）。因此凡是“Raft 共识侧”的不变量，M6 是
> **继承 M5** 而非重写；M6 的唯一新面是“把 `LogStore` 契约换一个实现”。

| # | 不变量（题述） | 判定 | 证据 / 论证 | 缺口 |
|---|---|---|---|---|
| **M6-I1** | 提交规则不变 | **仍成立** | 提交规则在 `src/raft/raft_node.cpp`（§5.4.2）；该文件与 `log_store.h` 在 M6 期间 diff 为空（上面的 `git diff --stat` 输出）。承载它的适配层只是 `LogStore` 的新实现，不改变 Raft 的多数派/任期判定入参 | 无（继承 M5 的 A 组用例，本轮未复跑） |
| **M6-I2** | durable-before-ack 落点 | **仍成立**（RaftNode 侧）；lsm 内部落点**仅论证无测试** | RaftNode 侧：`log_.persistMeta` 在 `metaPersistMu_` 叶子锁下（`raft_node.cpp:443`、`:468`，**不持 `mu_`**）；`log_.sync()` 在锁外（`raft_node.cpp:350`、`:589`、`:808`）。lsm 侧：`persistMeta` 用 `Write{sync=true}`（`lsm_log_store.cpp:251-253`），`append` = `appendNoSyncLocked` + `sync()`（`:271-278`），`sync()` = `db_->Sync()`（`:403`）。文档内嵌原文：`AppendIsDurableAndVisible` / `MetaPersistIsAtomicAndDurable` / `AppendNoSyncIsVisibleButSurvivesSync`（`m6-evidence.md` §M6.2 判据表） | **没有** lsm 专用的“卡住 sync ⇒ 不 ack”用例（M5 的 A3/A4 用 `SpyLogStore`，只覆盖 Memory 引擎）；“lsm `sync=true` 返回 ⟹ 已 fsync”是 **转述 `m2-design.md` §7.1 的结构性论证**，本轮未读 lsm 源码逐行复核（pin 已丢） |
| **M6-I3** | 索引连续与 term 语义 | **仍成立** | 连续性检查 `lsm_log_store.cpp:322`（`e.index != simLast + 1 ⇒ return false`）；同 index 同 term 幂等跳过 `~:317`；同 index 异 term 先规划 Delete 再 Put（同一 WriteBatch）；`terms_` 向量 + `termAtLocked`（`:548`）、`lastTerm_/lastIncludedTerm_` 边界特判。用例：`IdempotentAppendSameIndexSameTerm`、`ConflictTruncateThenAppend`、`KeyEncodingIsBytewiseIndexOrdered`、`EmptyLogWithBoundary` | `ConflictTruncateThenAppend` 同时钉住“重启后是新值 + term 正确”；边界条目 `termAt(lastIncluded_)` 由 `EmptyLogWithBoundary` / `CompactDropsPrefixAndSurvivesRestart` 钉住 |
| **M6-I4** | `truncateSuffix` 等价四条边界（E1–E4） | **仍成立** | `LsmLogStore::truncateLocked`（`lsm_log_store.cpp:450-471`）：E1 `fromIndex==kNoIndex ⇒ true`（`:457`）、E2 `<= lastIncluded_ ⇒ false`（`:458`）、E3 `> lastIndex_+1 ⇒ false`（`:459`）、E4 `== lastIndex_+1 ⇒ true 且无变化`（`:461` 分支不进入）。用例 `TruncateSuffixBoundaries`（逐条断言 E1/E2/E3/E4 的文字）；`truncateSuffixNoSync` 已覆写（`:421-423`），不会退回基类含 fsync 的默认实现 | E5–E10（Delete 区间 + 崩溃窗口）见 §3 R3；“截断的持久化”由 `TruncateNoSyncThenSyncIsDurable` 覆盖；**掉电语义未覆盖**（`m6-evidence.md` §M6.2 未验证 #2） |
| **M6-I5** | `compact` 边界 | **仍成立** | `LsmLogStore::compact`（`lsm_log_store.cpp:602-631`）：`upTo==kNoIndex ⇒ true`（`:610`）、`upTo<=lastIncluded_ ⇒ no-op`（`:611`）、`upTo>lastIndex_` 合法（`hi=min(upTo,lastIndex_)` `:614`，InstallSnapshot 语义）、删除 `[firstIndex,hi]` 走 `sync=true`（`:621`）、边界推进 + `lastIndex_/lastTerm_` 夹紧（`:624-628`）。用例：`CompactDropsPrefixAndSurvivesRestart`、`PrefixGoneWithoutSnapshotRefusesLoad`、`EmptyLogWithBoundary`、`SliceClampsAndHonoursLimits`（先 compact 再 slice clamp） | 与 `snapshotOpMu_` 的串行关系由 `raft_node.cpp:996` / `:1070` 保证（raft 侧未改）；lsm 侧无独立叶子锁（设计 §2.4.9 说明原因） |
| **M6-I6** | 重启恢复等价（R1–R6 六情形） | **被替换**（实现机制）；**可观察行为在 R1/R2/R4/R5/R6 等价**，R3 判据更保守 | 机制差异：FileLogStore 的撕裂尾靠“自己的 CRC 扫描 + `ftruncate`”（`file_log_store.cpp` L219-239），LsmLogStore 靠 **lsm 的 WAL 原子性 + `DB::Open` 恢复期截断**并记入 `RecoveryStats::tail_truncated_bytes`（`lsm_log_store.cpp:143-145`、`:218-221`）。“前缀丢失”判据逐字保留（`:203`）；“中间空洞”由 FileLogStore 的**截断**改为 **`return false` 拒绝启动**（`:205-207`，设计 §2.4.1 ⚠️ / D5 / W8）。用例：`RestartRestoresMetaAndLog`（file/lsm **同一 body**）、`LsmLogStore.TruncatesTornTail`（同一注入手法 + `walTailTruncatedBytes()>0`）、`LoadRefusesGapInTheMiddle`、`LoadSkipsEntriesBelowBoundary`、`PrefixGoneWithoutSnapshotRefusesLoad`、`EmptyLogWithBoundary` | R4（快照丢 + 已 compact ⇒ 拒绝启动）与 R3（边界之下残留在 user-view 里被跳过）都有定向用例；**撕裂尾的“由谁来截断”是两套机制**，这点是 **被替换** 而非等价 |
| **M6-I7** | compaction 不改已读日志 | **仍成立** | `compact` 只对 `[firstIndex, hi]` 逐条 `Delete`（`lsm_log_store.cpp:621`），**从不重写** `[hi+1, lastIndex]` 的条目；`terms_` 只 `erase` 前缀（`:623`）。用例 `CompactDropsPrefixAndSurvivesRestart` 断言 compact 后保留后缀（index 4/5）的值与重启后一致；`SliceClampsAndHonoursLimits` 断言 clamp 到 `firstIndex` | **没有**“保留后缀逐字节比对”的定向用例（只比对 index/term/value 的字段级相等）；也**没有**“compact 与并发 slice”的压力用例 |
| **M6-I8** | 两套 compact 不互破坏（日志 compact × lsm 内部后台 compaction） | **未复核** | 代码层只证明“日志 compact 与 Raft 的 snapshot 路径由 `snapshotOpMu_` 串行”（`raft_node.cpp:996/1070`，raft 侧未改）；**lsm 自身的后台 compaction 在本轮所有证据里从未触发**：`m6-bench.md` §5.2 D7 —— `lsm_compaction_rounds` 在 n≤4000 全为 0（“未触发 compaction”）。⇒ **两套 compaction 的交互没有任何实测** | 需要一条“强制 `max_bytes_for_level_base` 很小 + 并发 compact/slice”的定向用例或脚本；当前**无** |
| **M6-I9** | 回退后逐字节一致 | **仍成立**（编码/载荷层）；整库字节 **N/A** | 解释：两个引擎的**磁盘文件格式本就不同**（`raft.log` 帧格式 vs lsm 的 WAL/SST），“整库逐字节一致”不成立也不该成立；可要求且已钉住的是**日志条目编码载荷逐字节一致**（`encodeEntry` 单实现）。证据：`ValueBytesMatchFileLogStoreOnDisk`（`raft.log` 的 payload == `encodeEntry()` == lsm 的 value，逐字节）；`ConflictTruncateThenAppend`（冲突回退后保留条目的值不变） | 若 I9 的本意是“引擎回退到 `1463620` 后仓库树逐字节一致”（设计 §5.4），则属于 **git 操作**，本轮**未执行**（设计明确写“由父代理/用户执行”）⇒ **未复核** |
| **M6-I10** | 状态机幂等与原子落盘 | **仍成立** | `src/kv/**` 在 M6 diff 为空（上面的 `git diff --stat`）；`KvStateMachine` 的幂等应用语义未改。lsm 侧的“原子落盘”：`persistMeta` 是**单条 Put**（`lsm_log_store.cpp:248-253`）⇒ 一条 WAL record = 一个原子批；冲突覆盖的 Delete+Put 在**同一 WriteBatch**（`:352-368`）。用例：`MetaPersistIsAtomicAndDurable`、`ConflictTruncateThenAppend` | “一条 WAL record = 一个原子批”引自 lsm `m2-design.md` I15（转述，非本轮实测） |
| **M6-I11** | 编解码安全 | **仍成立** | 单一实现 `src/raft/log_entry_codec.h`（M6 新增，`kEntryFixedLen=41`、`kMetaPayloadLen=12`）；`decodeEntry` 校验 `n == 41+keyLen+valLen` 与 op 白名单，**并额外**校验 `e.index == key 里的 idx`（`lsm_log_store.cpp:199`、`:517`）——比 FileLogStore 多一层自校验。载荷字节一致性由 `ValueBytesMatchFileLogStoreOnDisk` 钉住 | **没有**对 lsm value 的**畸形输入**（截断/超长/坏 op）定向用例；`slice_decode_errors` 计数路径按 `m6-evidence.md` §M6.2 未验证 #4 **未构造注入点** |
| **M6-I12** | `slice` 上限语义（maxEntries / maxBytes / 至少 1 条 / clamp） | **仍成立** | `LsmLogStore::slice`（`lsm_log_store.cpp:478-528`）：clamp 到 `firstIndex`（`:490-491`）、`start>lastIndex_ || maxEntries==0 ⇒ 空`（`:492`）、字节口径 `key.size()+value.size()`（`:521`）、`if (!out.empty() && bytes+sz > maxBytes) break;`（`:522`，**只要有可用条目就至少返回 1 条**）。用例 `SliceClampsAndHonoursLimits` 逐条断言（clamp / maxEntries / maxBytes=1 仍出 1 条 / 越界 / maxEntries=0） | 与 FileLogStore L404 的“怪癖”**逐字相同**；`all()` seam 复用 `slice`（`:531-534`） |

**§1 小结**：M6-I1/I3/I4/I5/I7/I9(载荷层)/I10/I11/I12 = **仍成立**；M6-I6 = **被替换（机制）**；
M6-I8 = **未复核**；M6-I2 = 仍成立（RaftNode 侧）但 **lsm 内部落点仅论证无测试**。

---

## §2 线程安全契约与锁纪律（M6-L1 … M6-L6）

> 锁类别（`src/raft/lock_probe.h`）：`ProbedMutex`=`RaftNode::mu_`（kConsensus）、
> `MembershipMutex`=`membershipMu_`、`MetaMutex`=`metaPersistMu_`；`snapshotOpMu_` 是普通 `std::mutex` 叶子锁。
> **新增层**：`LsmLogStore::mu_`（普通 `std::mutex`，保护适配层内存态与 DB 结构性变更）。

| # | 契约（题述） | 判定 | 证据 / 论证 | 缺口 |
|---|---|---|---|---|
| **M6-L1** | 锁序单向：`RaftNode::mu_ → Transport::mu_ → 适配层锁 → LSM 内部锁` | **仍成立**（仅论证） | 适配层锁 = `LsmLogStore::mu_`：它在 `appendNoSyncLocked`（`:280-284`）与全部公共访问器里获取，而这些方法**由 RaftNode 在持 `mu_` 时调用**（`raft_node.cpp:335`/`:568`/`:746` 为 `appendNoSync`；`:148`/`:206`/`:250` 等为 `lastIndex/slice`）。LSM 内部锁在 `db_->Write/Get/NewIterator` 内部，**只被 `LsmLogStore::mu_` 之下的调用持有** ⇒ 传递序成立。反向边不存在：`LsmLogStore` 不引用 `RaftNode` | **没有**自动锁序测试；证据是静态阅读。TSan 的 `lock-order-inversion` 报告全部落在 `cv_`/`ProbedMutexT`，**不含** `LsmLogStore::mu_`（见 §6） |
| **M6-L2** | 禁止持 `mu_` 落盘与 fsync（I9 延续，**须可被 lockprobe 断言**） | **需加强** | 断言基础设施存在：`test_harness.h:190-227` 的 `SpyLogStore` 在每次 `sync/compact/persistMeta/truncateSuffix` 里读 `lockprobe::consensusHeld()` 并计数（`lockedSyncs/lockedCompacts/lockedMetaPersists/...`）。但 **`SpyLogStore : public MemoryLogStore`** ⇒ 它只证明 **RaftNode 的调用点纪律**（`raft_node.cpp` 在 M6 未改，故仍成立），**不覆盖 `LsmLogStore` 自身**。另一条真实风险：`compact` 持 `LsmLogStore::mu_` 做 `db_->Write(sync=true)`（`lsm_log_store.cpp:621` → `deleteRangeLocked` `:440`），而 RaftNode 持 `mu_` 调 `appendNoSync` 会**阻塞**在 `LsmLogStore::mu_` 上 ⇒ 共识锁可能**跨一次 fsync 等待**（fsync 不发生在 RaftNode 线程，但 `mu_` 被占住） | ① **没有一条 lockprobe 断言直接覆盖 `LsmLogStore`**（若要“须可被断言”，需给 LsmLogStore 加 `consensusHeld()` 探针或写一个 RaftNode+LsmLogStore 的 Spy 组合用例）；② 上面的“阻塞”是**静态推理**，无实测 |
| **M6-L3** | compaction 与选举心跳线程交互 + 最坏阻塞上界 | **未复核** | 结构事实：`compact` 持 `LsmLogStore::mu_` 逐块写 tombstone，**每块一次 `sync=true`**（`deleteRangeLocked` 分块 `kMaxOpsPerBatch=4096`，`lsm_log_store.cpp:425-447`）⇒ compact N 条最坏 = `ceil(N/4096)` 次 fsync，全程持 `LsmLogStore::mu_`；FileLogStore 的 compact 是**整份重写 + 1 次 fsync**。ticker/选举/心跳线程不持 `snapshotOpMu_`，但与 RaftNode 的写路径争 `LsmLogStore::mu_` | **没有任何最坏阻塞上界实测**（无“compact 期间 tick 延迟”用例/脚本；M6 的 lsm 后台 compaction 也从未触发，§1-I8）。需要一条定向基准才能给上界 |
| **M6-L4** | 关闭顺序 | **仍成立**（继承 M5 L15）；**仅论证无 lsm 进程级测试** | M5 L15 的关闭顺序（`Reactor::stop() → join reactor → join ticker → 析构 RaftNode/store`）在 `main_raft_node.cpp` 里未改（M6 对该文件只有 `--log-engine` 选型与 `g_lsmStore` 只读挂点，`git diff` 可见）。lsm 侧：`~LsmLogStore ⇒ db_.reset() ⇒ LsmDbDeleter` 调 `DB::Close()`（隐含 Sync）（`lsm_log_store.cpp:102-107`、`:145`） | **真实 node 进程的多线程关闭路径没有 TSan 覆盖**（TSan 不跨进程，`m6-evidence.md` §M6.r3-C #4 已登记）；任何“关闭期”异常都只有 Release e2e 覆盖 |
| **M6-L5** | 条件变量谓词跨层不丢唤醒 | **仍成立**（继承，`raft_node.cpp` 未改）；但**该区域有 TSan 报告** | 所有 `cv_` 在 `raft_node.cpp`（M6 diff 为空），适配层 `LsmLogStore` **没有条件变量**、不参与该跨层链。文档内嵌原文：M5 的 A11/A13/A15/A16 覆盖乱序/重复 ack 与多 proposer 唤醒 | TSan 全量 122/122 断言通过但报 **16 条**（8 double-lock + 8 lock-order-inversion，**0 data race**），全部落在 `RaftNode` 的 `condition_variable_any` 路径（`raft_node.cpp:140/164/895`）与脚手架 `ProbedMutexT`；与基线 `1463620` **逐项一致** ⇒ 判为**上游/脚手架既有报告**，非 M6 引入。**但“有报告”本身就是缺口**，见 §6/§9 |
| **M6-L6** | 适配层自持锁：`sync()` 可能不持 `mu_` | **仍成立** | `LsmLogStore::sync()`（`lsm_log_store.cpp:394-408`）**不获取 `mu_`**，只做 `if (db_==nullptr) return false; ... db_->Sync();`；失败时**回锁**只写 `poisoned_`（`:404-407`）。`persistMeta` 的两段式也刻意把 `Write{sync=true}` 放在**两次持锁之间**（`:236-266`）。头文件注释（`lsm_log_store.h` 顶部“锁纪律”）与实现一致 | `sync()` 里 `db_` 的读取无锁（`db_` 构造后不变、析构与 `sync` 并发属 UB 契约）——见 §5 UB 清单 U4 |

**§2 小结**：M6-L1/L5/L6 = **仍成立**；M6-L4 = 仍成立但仅论证；**M6-L2 = 需加强**（无直接 lockprobe 断言 + compact 持适配层锁做 fsync）；
**M6-L3 = 未复核**（无上界实测）。

---

## §3 风险清单（后果 + 缓解 + 现状证据）

> 风险编号沿用 `m6-design.md` §6.3 的 R1–R7，并补 M6 落地后**新增/变化**的 R8–R10。
> “现状证据”= 本轮能查到的**文档内嵌原文**；原始 /tmp 日志已丢（§7）。

| # | 风险 | 触发条件 | 后果 | 缓解（设计/现状） | 现状证据 | 判定 |
|---|---|---|---|---|---|---|
| **R1** | lsm `DB::Sync()` 在 `commit_mu_` 之下做 fsync，打散组提交 | lsm 引擎 + 高并发写 | 组提交调优（`groupCommitLingerUs`/`maxInflightPerPeer`）**可能失效**；吞吐塌陷 | 设计判定为 **lsm 仓库改动**（把 `log_->Sync()` 移出 `commit_mu_`，照搬 FileLogStore 的 `flushMu_` 叶子锁）⇒ 由父代理裁决；M6 不改 lsm | `m6-bench.md` §8：R1 修复（lsm `f06a44d`）后 p=64 **+8%**；但设计预测的 0.47× 塌陷**两轮都没出现**（b1bd050 0.89×、f06a44d 1.08×）。**A/B 数字受并发重活污染（`m6-bench.md` §6.6-D3 / M6.7-D5，loadavg 4.7–15.9）** | **缓解已发生（在 lsm 侧）**；M6 侧风险未消失 |
| **R2** | `slice()` 每次创建 lsm 迭代器 | 心跳路径 `buildPeerJobLocked()` 调 `slice(next,1,SIZE_MAX)`（`raft_node.cpp:250`） | 心跳热路径开销/抖动 | 设计 §7 W5：“最近的 append 批次”内存缓存 = **M6 不做（YAGNI）** | `m6-bench.md` §5.2 D10 未采集（“lsm 内部统计未接线”→后已接线，但 D10 无定向数字）；`m6-evidence.md` §M6.7 未验证清单列“P99 未采集” | **未复核** |
| **R3** | `truncateSuffix` 从 O(1) 退化为 O(被截断条目数) | 长冲突后缀（长时间分区 follower 回归） | 写放大、延迟放大 | 单测钉住口径（`TruncateSuffixBoundaries` + `ConflictTruncateThenAppend` 断言 `truncated_entries`） | `m6-bench.md` §6.3：“没有『长冲突后缀』的定向构造”；50 轮 kill -9/SIGSTOP 故障注入全过但**不测写放大** | **仅论证 + 口径单测；性能未复核** |
| **R4** | 空间放大 + 物理空间不即时回收（compact 只写 tombstone） | lsm 引擎 + compact 后继续写 | `raft_snapshot_fault.sh` 的空间判据曾**红**（`4450355 > 4194304`）；目录字节 ≈1.2–1.5× file | C6：`LOG_BOUND` 按引擎取口径（file `raft/raft.log` 1 MiB；lsm `raft-lsm/` 目录字节），阈值 **4 MiB → 8 MiB** 重标定（`m6-bench.md` §9.4） | `m6-bench.md` §5.1：`logdir_bytes=843535 vs lsm 自报 live WAL 326393` ⇒ ~517 KB 未回收；§9.5：8 MiB 下四臂 `--repeat 50` 全 PASS | **已缓解（重标定）但空间放大仍在**；重标定本身是“判据放宽”，需裁决接受 |
| **R5** | 写停顿（`WaitForImmutableCapacity`） | lsm 写量 > 后台 flush 能力（`raft_snapshot_fault.sh` 灌 100k 条） | 写者停等、P99 抖动 | 观测 O7（`stall_events`/`stall_ms`）已接线（`engineStatsFragment`） | `m6-bench.md` §5.1 采集到 `lsm_stall_*` 字段；但 §5.2 D7 显示 n≤4000 时 compaction 全 0 ⇒ **未触发** | **未复核（未触发）** |
| **R6** | 启动恢复代价（lsm 多阶段恢复 vs file 一遍扫描） | 大日志重启 | 启动变慢 | 观测 D4（`OPEN_MS`） | `m6-bench.md` §5.2 D4 **未采集**（“n 未达 / 无定向数字”） | **未复核** |
| **R7** | P99 抖动来自 lsm 后台线程（flush + compaction）与 raft 线程争 CPU | 单机多节点 + 后台线程 | P99 尾延迟 | 观测 D2/D7 | `m6-bench.md` §5.2 列 D7 compaction 全 0；`m6-evidence.md` §M6.7 列“P99 未采集” | **未复核** |
| **R8** | **适配层锁被 compact 长持**：`compact` 持 `LsmLogStore::mu_` 每 4096 条一次 fsync | 长区间 compact 与 RaftNode 写并发 | RaftNode `mu_` 可能跨多次 fsync 等待；选举/心跳延迟尾部长 | 无（M6 未处理；设计只把 compact 移出 RaftNode `mu_`，未把 store 锁内的 fsync 拆小） | 代码事实：`lsm_log_store.cpp:425-447`（分块 + 每块 `sync`）与 `:621`；**无实测** | **需加强**（与 L2/L3 同源） |
| **R9** | **中间空洞判据更严**：lsm 下“空洞 ⇒ 拒绝启动” | 介质级损坏 | 可用性下降（但不丢已提交数据） | 设计 D5/W8 视为**保守改进**；用例 `LoadRefusesGapInTheMiddle` | 用例名 + `m6-evidence.md` §M6.2 判据表 | **已接受（设计裁决）**；属可观察行为差异（I6） |
| **R10** | **同一 `--data-dir` 切引擎不可用**：`raft/` 与 `raft-lsm/` 物理隔离 | 运维在已有目录上换 `--log-engine` | 看不到另一引擎日志；R5 情形（空日志 + 边界）**推理未验证** | 设计 W6：**明确不支持**；A/B 脚本每格用全新临时目录 | `m6-design.md` §7 W6（**推理，未验证**） | **未复核** |

---

## §4 存量代码修改点清单（vs 实际 diff）

> 对照基线：`git diff --stat 6aabc27..HEAD`（`6aabc27` = M6 起点，其树 == `1463620` 的树 + docs）。
> 实际改动 **22 个文件、+3919 / −131**。

### 4.1 【必须新增】——设计 C1 / C8 / M6.0-D3

| 文件 | 实际 | 说明 |
|---|---|---|
| `src/raft/lsm_log_store.h`（136 行）/ `.cpp`（702 行） | ✅ 新增 | 设计 C1；M6 唯一的新逻辑 |
| `src/raft/log_entry_codec.h`（97 行） | ✅ 新增 | **C1–C9 之外的第 10 项改动（C10）**：把 `encodeEntry/decodeEntry/encodeMeta/decodeMeta` 从 `file_log_store.cpp` 的匿名 namespace 抽出，两个引擎共用（见 4.5） |
| `tests/raft_lsm_log_test.cpp`（679 行） | ✅ 新增 | 设计 §4-M6.2；含 M6.r3 的真并发用例 |
| `scripts/bench_m6_ab.sh`（226 行）/ `scripts/bench_m6_footprint.sh`（154 行） | ✅ 新增 | 设计 C8 + 空间探针 |
| `docs/m6-bench.md`（527 行）/ `docs/m6-evidence.md`（916 行） | ✅ 新增 | C8 + 证据入档（evidence 是设计外的额外产出） |

### 4.2 【必须改】——设计 C2–C7 / C9

| 文件 | 实际 | 说明 |
|---|---|---|
| `CMakeLists.txt` | ✅ +16 | C2：`lsm_log_store.cpp` 进 `raftkv_raft`；lsm 子目录接线（CMake 变量 RAFTK_LSM_DIR 指向的路径，找不到 ⇒ 不定义 `RAFTK_HAVE_LSM`，**不静默降级**） |
| `src/main_raft_node.cpp` | ✅ +39 | C3：`--log-engine=file|lsm` + `RAFTKV_LOG_ENGINE`，**默认 file**；M6.r2 加 `g_lsmStore` 只读统计挂点 |
| `tests/raft_restart_test.cpp` | ✅ +80 | C4：用例体抽模板，`FileLogStore.*` 名字/断言不变，新增 `LsmLogStore.*` 孪生 |
| `tests/raft_snapshot_test.cpp` / `raft_membership_test.cpp` | ✅ +131 / +70 | C5：Disk fixture 参数化（`DiskEngine` + `makeDiskLog`） |
| `scripts/raft_snapshot_fault.sh` | ✅ +53 | C6：`LOG_BOUND` 按引擎取口径（4 MiB → 8 MiB） |
| `scripts/raft_{e2e,fault,membership_e2e,membership_fault,snapshot_e2e}.sh` | ✅ 各 +19~21 | C7：`--log-engine` 透传（env 通道，见 M6.5-D1） |
| `README.md` / `docs/roadmap.md` | ✅ +40 / +13 | C9：文档一致性 |

### 4.3 【可选扩展】——设计未强制

`tests/raft_test_harness.h`（**实际未改**）、`src/raft/transport_tcp.*`、`src/main_raft_client.cpp`。
⇒ 与设计一致，无偏差。

### 4.4 【禁止改动】——硬边界

| 对象 | 实际 | 证据 |
|---|---|---|
| `~/raft-kv`（A/B 基线与回滚参照） | ✅ 未动 | 本轮只读；`git -C ~/raft-kv status --short` 只有那个**既有未跟踪文件** `scripts/demo_record.sh`（M6 未触碰） |
| `~/lsm-kv`（lsm 引擎仓库） | ✅ 未动 | 本轮只读；M6 构建只链接 `git archive` pin 出来的副本 |
| `src/raft/raft_node.{h,cpp}`（N1） | ✅ 未改 | `git diff --stat 6aabc27..HEAD -- src/raft/raft_node.cpp src/raft/raft_node.h` **空** |
| `src/raft/log_store.h`（契约） | ✅ 未改 | 同上 diff **空** |
| `src/kv/**`（N7） | ✅ 未改 | 同上 diff **空** |
| `message/transport/cluster_config/snapshot 格式`（N2） | ✅ 未改 | `git diff --stat 6aabc27..HEAD` 的文件列表里不含它们 |

### 4.5 实际 diff 与清单的**对照结论**

- 实际改动**全部落在 C1–C9 + 一处已登记偏差**，**没有**任何“禁止改动”被碰。
- **唯一超出 C1–C9 的改动**：`src/raft/file_log_store.cpp`（−70 行，删除本地 codec 副本改调 `log_entry_codec.h`）。
  这不是“新增 C10 之外的黑改”：设计 §2.2 要求“**逐字复用**”，而原实现位于匿名 namespace
  **外部 TU 无法链接** ⇒ 抽取是唯一可行路径。该偏差已在 `m6-evidence.md` §M6.0 **M6.0-D3** 登记为 **C10**。
  回归兜底：`ValueBytesMatchFileLogStoreOnDisk`（file 磁盘 payload == `encodeEntry()` == lsm value）。
  ⇒ **登记为“已声明的必要改动”；但对“存量代码修改点清单”本身是缺口——清单没预料到 file 引擎文件会被动。**

---

## §5 边界 case 全集、未定义行为清单、单元测试前置假设

### 5.1 边界 case 全集（实现/用例逐条回答）

| # | 边界 case | 落点（用例 / 代码） | 状态 |
|---|---|---|---|
| B1 | 单节点集群（无 peer） | 既有 e2e（`raft_e2e.sh`）；`raft_node.cpp` 未改 | 继承 |
| B2 | peer 长时间挂起（SIGSTOP） | `raft_fault.sh` / `raft_membership_fault.sh --repeat 50`（lsm 臂 PASS） | 有脚本证据 |
| B3 | 解锁窗口内 step-down / 冲突截断 | `raft_node.cpp` 未改；M5 A 组 | 继承 |
| B4 | 解锁窗口内 `truncateSuffix`/`compact` | lsm 适配层 `truncateLocked/compact`（`:450`/`:602`）；RaftNode 调点未改 | 有代码证据 |
| B5 | compact 期间并发 InstallSnapshot | `snapshotOpMu_`（`raft_node.cpp:996/1070`）；lsm 内部无独立锁 | 继承（**lsm 后台 compaction 从未触发**，§1-I8） |
| B6 | `fsync` 失败 | lsm `poisoned_` fail-stop（`lsm_log_store.cpp:369/406/441`）；RaftNode 未改 | 有代码证据；**无故障注入用例** |
| B7 | 磁盘满 / `Write` 失败 | 同上返回 false + 毒化；不假装成功 | 有代码证据；**无注入** |
| B8 | 退出时在途异步发送 | M5 L15 关闭顺序（`main_raft_node.cpp`） | 继承 |
| B9 | `removePeer` 与在途请求竞争 | M5 A6 | 继承 |
| B10 | 重复/乱序 ack | M5 A5/A11 | 继承 |
| B11 | 指标请求中途被读 | `engineStatsFragment` **只读 + `#ifdef` 空串**（`lsm_log_store.cpp:663-702`）；file 引擎不输出 `lsm_*` | 有代码证据 |
| B12 | 快照/压缩边界（小快照、单块） | `EmptyLogWithBoundary`、`CompactDropsPrefixAndSurvivesRestart` | 有单测 |
| B13 | 断点续传 | M5 范围（lsm 不涉及） | 继承 |
| B14 | meta 落盘顺序（并发投票） | `persistMeta` + `metaPersistMu_` | 继承 |
| B15 | 滑动窗口乱序/重复 ack | M5 | 继承 |
| B16 | `truncateSuffix` 四条边界 E1–E4 | `TruncateSuffixBoundaries`（`lsm_log_store.cpp:457-461`） | **有单测（逐字断言）** |
| B17 | 中间空洞 | `LoadRefusesGapInTheMiddle`（`:205-207`） | **有单测** |
| B18 | 撕裂 WAL 尾 | `LsmLogStore.TruncatesTornTail`（同注入手法 + `walTailTruncatedBytes()>0`） | **有单测** |
| B19 | 边界之下残留被跳过（R3） | `LoadSkipsEntriesBelowBoundary` | **有单测** |
| B20 | 空日志 + 边界（R5） | `EmptyLogWithBoundary` | **有单测** |
| B21 | 前缀丢失 + 快照丢 ⇒ 拒绝启动（R4） | `PrefixGoneWithoutSnapshotRefusesLoad` | **有单测** |
| B22 | 同进程二次 `Open`（LOCK 语义） | `SecondProcessOnSameDirIsRejectedByLock`（**fork 双进程**；同进程不冲突已登记 M6.0-D4） | **有单测** |
| B23 | `slice` clamp / maxEntries=0 / maxBytes=1（至少 1 条） | `SliceClampsAndHonoursLimits`；`:492`/`:522` | **有单测** |
| B24 | `compact` `upTo>lastIndex_`（InstallSnapshot） | `compact` `:614`；设计 §2.4.9 | **仅论证**（无直接单测构造 `upTo>lastIndex_`） |
| B25 | `truncateSuffixNoSync` 的**掉电**语义（后缀复活） | 只测得“kill -9 下页缓存存活”（`TruncateNoSyncIntoKillDashNineKeepsDeletionInPageCache`） | **未覆盖（掉电）**；已登记 M6.2-D2 |
| B26 | 大区间 `append` / `compact` 分块（>4096 op / >8 MiB） | `kMaxOpsPerBatch=4096` / `kMaxBytesPerBatch=8 MiB`（`:52-53`） | **仅代码；无跨批用例** |
| B27 | `lsm::WriteBatch` 上限（1<<20 / 64 MiB） | 设计 §2.4.4；实现分块留余量 | **仅论证** |

### 5.2 未定义行为（实现中必须消除 / 现状）

| # | UB 风险 | 现状 |
|---|---|---|
| U1 | 持 `LsmLogStore::mu_` 时调用**公共访问器**（非 recursive ⇒ 自死锁） | 头文件显式纪律 + 内部一律用 `*Locked`（`lsm_log_store.h` “调用方必须持 mu_”）；已遵守 |
| U2 | 同一目录并发两个 `DB::Open`（跨进程） | lsm 的 `LOCK`（fcntl）拒绝；B22 用例 |
| U3 | 在 `RaftNode::mu_` 内调用 lsm 的**已知阻塞**路径（`compact`/`sync`）(I9) | RaftNode 调点未改，`sync/compact` 在锁外；但**间接阻塞**见 R8/L2 |
| U4 | 与析构并发的 `sync()` 读 `db_`（无锁） | `sync()` 不持 `mu_` 读 `db_`（`:396`）；正常生命周期下 `db_` 不变，**并发析构属调用方 UB**，无守卫 |
| U5 | 依赖“迭代器耗尽”终止 `slice` | **已消除**：显式 `idx > lastIndex_ ⇒ break`（`:513`） |
| U6 | 依赖“`syncedIndex_ == lastIndex()`” | 继承 M5 已消除（锁外 fsync + 夹紧） |
| U7 | 把“配置条目已生效”当“已 durable” | 继承 M5 约束；M6 未改 raft_node |

### 5.3 单元测试前置假设（本轮实读）

1. **构建假设**：C++17；GTest 在 `/usr/local`（`CMakeLists.txt` 的 `find_path/find_library`）；`RAFTK_HAVE_LSM` 仅当
   `RAFTKV_LSM_DIR` 指向的路径存在 `CMakeLists.txt` 时定义（默认 `$ENV{HOME}/lsm-kv`，A/B 用 pin 覆盖）。
   **未配置 lsm** 时 `raft_lsm_log_test.cpp` 只剩一条**真断言**用例（`WithoutLsmSupportConstructionFailsLoudly`，不是 `GTEST_SKIP`）。
2. **磁盘假设**：用例用**真实临时目录**（`tempDir()` = `temp_directory_path()/raftkv_lsm_test_<pid>_<n>`），跑完 `remove_all`。
3. **进程假设**：崩溃/LOCK 用例依赖 `fork()` + `_exit()`（不跑析构）——只在 POSIX 成立（`<sys/wait.h>/<unistd.h>`）。
4. **时间假设**：本轮不跑；`m6-evidence.md` 记录全量 `121`→`122` 用例耗时约 44–51 s（RelWithDebInfo），ASan 约 51 s，TSan 更慢（A11 期限在 M5 已 20s→90s）。
5. **覆盖率假设（已登记限制）**：`LsmLogStore` 的并发正确性只有**单进程真并发用例**
   `ConcurrentAppendSyncAndReadersStayConsistent`（1 写者 + 3 读者）；**真实 node 进程**的多线程路径（ticker/连接线程/reactor）
   **没有 sanitizer 覆盖**（TSan 不跨进程）。

---

## §6 TSan 验收口径分歧：只读查证（**本轮不跑 TSan**）

### 6.1 问题

- **我方全量 TSan 实测（转述，写入 `m6-evidence.md`/`m6-bench.md`）**：`[  PASSED  ] 122 tests.` +
  `ThreadSanitizer: reported 16 warnings`（8 × `double lock of a mutex` + 8 × `lock-order-inversion`，
  **data race 0**），rc=66；且 **M6 之前的同一棵树同样 16 条、类型逐字相同**。
- **raft-kv 在 M5 文档里声称**“TSan 94/94 且 0 报告”，M6 指令的验收口径也写“TSan/ASan 干净”。

⇒ 需要查清：**raft-kv 当初到底跑了什么？**

### 6.2 原文引用（`~/raft-kv`，只读）

**① 完整命令（`docs/m5-design.md` L677，v2.1 “TSan 收尾”）：**

```
TSAN_OPTIONS=suppressions=tests/tsan.supp setarch $(uname -m) -R ./build-tsan/bin/raftkv_raft_tests
```

**② 同一条口径在 `docs/m5-design.md` L849（v2.6 验收表）：**

```
| TSan | ✅ rc=0、**0 warning**、86/86 | TSAN_OPTIONS=suppressions=tests/tsan.supp + setarch -R |
```

**③ `docs/m5-review.md` L58（交付物表）：**

```
| 测试 | 单测 **94/94**（M5.A1–A16、R1–R6 等）；TSan **94/94 且 0 报告**
  （tests/tsan.supp 窄抑制 libstdc++ condition_variable_any 的 notify_all/wait_until 两族误报，含出处论证） |
```

**④ 抑制文件的**实际内容**（`~/raft-kv/tests/tsan.supp`，尾部 4 行有效规则；文件头为出处论证）：**

```
mutex:condition_variable_any::notify_all
deadlock:condition_variable_any::notify_all
mutex:condition_variable_any::wait_until
deadlock:condition_variable_any::wait_until
```

**⑤ ASLR 关闭的出处（`~/raft-kv/CMakeLists.txt` L14）：**

```
# M5：并发正确性门禁（M2/M3 只跑过 ASan）。TSan 需要关闭 ASLR：setarch $(uname -m) -R ./bin/...
```

**⑥ 范围口径（`docs/m5-design.md` L267）：**

```
TSan（全量 raft 用例 + `raft_perf_test`）必须 0 报告；...
```

### 6.3 逐项回答（命令、filter、TSAN_OPTIONS、perf 用例、setarch）

| 维度 | raft-kv（M5 声称“0 报告”时） | raft-kv-lsm（M6 实测 16 条时） |
|---|---|---|
| 完整命令 | `TSAN_OPTIONS=suppressions=tests/tsan.supp setarch $(uname -m) -R ./build-tsan/bin/raftkv_raft_tests` | `setarch $(uname -m) -R ./build-tsan/bin/raftkv_raft_tests`（`m6-bench.md` L171；基线 L181）；窄面加 `--gtest_filter='LsmLogStore.*'`（`m6-evidence.md` L745） |
| `--gtest_filter` | **无**（全量） | 全量时无；M6.8 的 H12 窄面**有** `LsmLogStore.*` |
| `TSAN_OPTIONS=suppressions` | **有** `tests/tsan.supp` | **无**（三处命令都没带） |
| `TSAN_OPTIONS=exitcode` / `halt_on_error` / `report_bugs` | **无**（只设了 `suppressions`） | 无 |
| 是否排除 perf 类用例 | **否**（`raft_perf_test` 在列；触发报告正是 A11/A13/A15/A16 这些 perf 用例） | 否（报了 16 条的正是同一批用例） |
| `setarch -R` | **有** | **有** |
| 抑制覆盖的告警族 | `notify_all`/`wait_until` × `mutex`/`deadlock` 共 4 族 | 报出的 16 条**恰好全在这 4 族内**（顶层帧 `raft_node.cpp:140/164/895`） |

### 6.4 结论与建议

**结论（分歧的根因）：不是 raft-kv 谎报，而是两边的 `TSAN_OPTIONS` 不同。**
raft-kv 的“0 报告”是 **在 `TSAN_OPTIONS=suppressions=tests/tsan.supp` 生效的前提下的 0 报告**；
该抑制文件**刻意只收窄** `condition_variable_any` 的 `notify_all`/`wait_until` 四个族（其余 race/mutex/deadlock 照报）。
raft-kv-lsm 的两次全量 TSan 都**没有带这个 `TSAN_OPTIONS`** ⇒ 报出的 16 条正好就是被抑制的那 4 族。
两份 `tests/tsan.supp` **内容逐字相同**（本仓 `tests/tsan.supp` 与 raft-kv 的一致，含 M5.7 的 `wait_until` 族）。

**建议（两条一起做，不矛盾）：**

1. **主口径：照原样跑同一条口径**——下一步机器空闲时，用 **raft-kv 文档里的 canonical 命令**重跑：
   ```
   TSAN_OPTIONS=suppressions=tests/tsan.supp setarch $(uname -m) -R ./build-tsan/bin/raftkv_raft_tests
   ```
   预期：`[  PASSED  ] 122 tests.`、0 报告、rc=0（因为 16 条全落在被抑制族内）。
   **这需要实测确认**（若 `ProbedMutexT` 的栈让抑制规则匹配不上，则必须转入第 2 条）。
2. **正式登记差异（无论第 1 条结果如何都要做）**：
   - 在验收文档里把“TSan 干净”的**口径写死**：必须是“带 `TSAN_OPTIONS=suppressions=tests/tsan.supp` 的全量运行”；
     不能把“窄面 `LsmLogStore.*` 0 报告”外推成“全量干净”（`m6-evidence.md` §M6.r3-C 已经做过这次更正）。
   - 把“**不带抑制时会报 16 条（0 data race，全部 `condition_variable_any` 族，与基线逐项一致）**”
     作为**已知事实**保留在文档里，绝不再写成“0 报告/无报告”。
   - “与基线 `1463620` 逐项一致”已由 `m6-evidence.md` §M6.r3-C 的原始计数行支撑（原始 /tmp 日志已丢，见 §7）。

> **本轮边界**：以上为**只读查证 + 建议**。命令的**实际执行**留给下一步；本文**不声称**补抑制后一定为 0。
> 若 raft-kv 文档里的命令定义被证不可复现，则按“未找到可复现口径”处理——本轮**找到了**完整可执行的命令定义
> （见 ①②④⑤），因此**不存在**“未找到可复现口径”的情形。

---

## §7 证据可复现性登记（`/tmp` 已因重启清空）

### 7.1 已确认：本轮 `/tmp` 为空

实测（本轮）：
```
$ ls -la /tmp/r3-tsan-full2.log     → No such file or directory
$ ls -la /tmp/lsm-pin-b1bd050       → No such file or directory
$ ls /tmp                          → 只有 systemd-private-*、VMware* 与测试临时目录
```

### 7.2 只存在于 `/tmp`、现已丢失的原始证据（逐项列出）

| 类别 | 丢失的路径（来自 `m6-evidence.md`/`m6-bench.md` 的引用） | 它当初支撑什么 |
|---|---|---|
| TSan 全量 | `/tmp/r3-tsan-full2.log`、`/tmp/r3-baseline-tsan-full.log`、`/tmp/r3-tsan-tests.log` | 122/94 的 16 条报告原文、rc=66、分类表 |
| TSan 窄面/并发 | `/tmp/r3-tsan-lsm.log`、`/tmp/r3-tsan-lsm2.log`、`/tmp/r3-tsan-conc.log` | `LsmLogStore.*` 21/21、0 报告；并发用例 ×200 |
| TSan 构建/汇总 | `/tmp/r3-tsan-build.log`、`/tmp/r3-queue-all.log` | “编译期 0 warning”与“tsan_tests_rc=66”的澄清 |
| ASan | `/tmp/r3-asan-tests2.log` | 122/122、0 ASan 报告 |
| A/B 原始行 | `/tmp/r2-*.log`、`/tmp/r2-base-*.log`、`/tmp/m6bench-logs` | §2 的 27 格原始行、§5.1 补充轮 |
| 故障注入 | `/tmp/m6r2-snapfault-lsm50.log`、`/tmp/r3-foot-*.log`、`/tmp/diag_snapfault_lsm.sh`、`/tmp/r3_fault_b1.sh` | `--repeat 50` 的 PASS 行、诊断轮 |
| **构建基座（关键）** | `/tmp/lsm-pin-b1bd050`、`/tmp/lsm-pin-f06a44d` | M6 链接的 lsm 源码副本；**丢了就无法逐字节复现当时的二进制** |
| 基线构建 | `/tmp/rk-baseline-tsan` | 基线 `1463620` 的 TSan 二进制 |
| 其他 | `/tmp/m61-*.log`、`/tmp/m62-build3.log`、`/tmp/m64-build3.log`、`/tmp/m6-smoke-*`、`/tmp/fix_m64_warning.py`、`/tmp/m65_proof.sh`、`/tmp/p.sh` | M6.1/M6.2/M6.4 构建、smoke、通道探针 |

⇒ **影响**：§1–§5 里凡引用 `m6-evidence.md`/`m6-bench.md` 的“PASS/0 报告/原始数字”，
**本轮只能引用文档里内嵌的那几行原文**；原始日志**不可复读**，二进制基座（`b1bd050`）也**不可重建**。

### 7.3 已内嵌在文档里、可直接引用的关键原文

| 证据 | 内嵌位置 |
|---|---|
| `[  PASSED  ] 122 tests.` / `[  PASSED  ] 94 tests.` + 16 条分类表 | `docs/m6-bench.md` §4.1（L168-198）；`docs/m6-evidence.md` §M6.r3-C |
| TSan 16 = 8 double-lock + 8 lock-order-inversion、0 data race、rc=66、与基线逐项一致 | `docs/m6-bench.md` L161/L884-891；`docs/m6-evidence.md` §M6.r3-C |
| ASan `122 tests` / H11 | `docs/m6-evidence.md` L740-741；`docs/m6-bench.md` L160 |
| 故障脚本 `PASS (50 iterations)` 原文行 | `docs/m6-evidence.md` §M6.6（L634-659）；`docs/m6-bench.md` §9.5（L486-527） |
| A/B 27 格原始行 + 汇总 | `docs/m6-bench.md` §2（L22-105） |
| 空间红项与重标定（4→8 MiB）原文 | `docs/m6-bench.md` §7.1/§9.3/§9.4/§9.5 |
| `lsm_compaction_rounds=0` | `docs/m6-bench.md` §5.2 D7 |
| 退出码 / `warnings=0` 的口径澄清 | `docs/m6-evidence.md` §M6.r3-C #1 |

### 7.4 建议（今后原始日志入库）

1. **原始日志落 `docs/raw/` 并随提交入库**（如 `docs/raw/m6/r3-tsan-full2.log`），
   与引用它的 `docs/m6-evidence.md`/`m6-bench.md` 同一次提交；`/tmp` 只放**可从入库物重建**的中间物。
2. **pin 基座不要只放 `/tmp`**：把 `git archive <sha>` 的产物（或至少 sha + tarball 校验和）
   写进 `docs/raw/`（或 `m6-bench.md` 的固定章节），否则“链接了哪个 lsm”不可复现。
3. 在 `docs/m6-bench.md` 顶部固定一节“**原始证据索引**”：每条硬门禁/观测 → 对应 `docs/raw/...` 路径 + 校验和。
4. 本轮**不改** `docs/m6-evidence.md`/`m6-bench.md`（纪律），只在本文件登记。

---

## §8 额外核查（本轮实做，只读）

### 8.1 `raft_node.cpp` 是否出现任何 LSM 头文件（layering 纪律）

命令：
```
$ grep -rn 'lsm/\|lsm::\|#include.*lsm' src/raft/ src/kv/ src/main_raft_node.cpp
```
原文输出（节选）：
```
src/raft/lsm_log_store.cpp:18:#include "raft/lsm_log_store.h"
src/raft/lsm_log_store.cpp:56:lsm::Slice LsmSliceOf(const std::string& s) {
...
src/raft/lsm_log_store.h:106:    void operator()(lsm::DB* db) const noexcept;
src/raft/lsm_log_store.h:110:  std::unique_ptr<lsm::DB, LsmDbDeleter> db_;
src/main_raft_node.cpp:29:#include "raft/lsm_log_store.h"
```
**结论：`raft_node.cpp` / `raft_node.h` / `src/kv/**` 中 0 处 LSM 头文件或 `lsm::` 符号。**
LSM 依赖**只出现在适配层**（`lsm_log_store.{h,cpp}`，其 `.cpp` 才 `#include "db.h"` 并受 `#ifdef RAFTK_HAVE_LSM` 包夹）
与**组合根** `main_raft_node.cpp`（它 `#include "raft/lsm_log_store.h"`，且 `lsm_log_store.h` 对 `lsm::DB` 只做前向声明）。
⇒ **layering 纪律成立**（`file:line` 证据见上）。

### 8.2 三份 GTest/测试计数口径 + 无删除/弱化既有断言

**① 计数口径（各时点原文行）**

| 二进制 / 范围 | 时点 | 计数 | 原始行来源 |
|---|---|---|---|
| `raftkv_raft_tests`（全量） | 基线 `1463620` | **94** | `m6-evidence.md` L182-183；`m6-bench.md` §4.1 |
| `raftkv_raft_tests`（全量） | M6.4 参数化后 | **114** | `m6-evidence.md` L339-340、L434-435 |
| `raftkv_raft_tests`（全量） | M6.5–M6.8 | **121** | `m6-evidence.md` L494-495、L740-741 |
| `raftkv_raft_tests`（全量） | **HEAD `924492e`** | **122** | `m6-evidence.md` §M6.r3-C（`[  PASSED  ] 122 tests.`） |
| `raftkv_raft_tests --gtest_filter='LsmLogStore.*'` | M6.8 / M6.r3 | **20 / 21** | `m6-evidence.md` L745-747（20）；§M6.r3-C（21，+并发用例） |
| `raftkv_raft_tests --gtest_filter='*Disk*'` | M6.4 | **18** | `m6-evidence.md` L490-491 |
| `raftkv_tests`（M1） | M5 收尾 | **13/13** | `~/raft-kv/docs/m5-design.md` L650/L836（**M6 文档里未找到 HEAD 计数 ⇒ 本轮未复核**） |
| `lsm_tests`（`~/lsm-kv`） | M5 收尾 | **205/205** | `~/lsm-kv/docs/m5-evidence.md` L155-157（**不在 M6 范围**；M6 只链接 pin 的 lsm） |

**从 94 → 122 的去向（机械核对）**：`grep -cE '^TEST(_F|_P)?\(' tests/*.cpp` 求和 = **123**；
其中 `raft_lsm_log_test.cpp` 有 **1 条在 `#else // !RAFTK_HAVE_LSM` 内**（`WithoutLsmSupportConstructionFailsLoudly`，
与 `#ifdef` 分支互斥）⇒ 实际构建 **122** 条，与 `[  PASSED  ] 122 tests.` 吻合。
⇒ **94（基线）+ 参数化孪生（M6.3/M6.4）+ 新增 lsm 用例（M6.2）+ 真并发 1 条（M6.r3）= 122**，
被合并/改名的两处已在 `m6-evidence.md` **M6.3-D2** 明确登记为“合并而非削减”（断言更强）。

**② 无删除/弱化既有断言的原始输出**

命令：
```
$ git diff -U0 1463620..HEAD -- tests/ | grep -E '^-.*(EXPECT_|ASSERT_)'
```
原文输出（**15 行，逐字**）：
```
-    ASSERT_TRUE(log.append(head));
-    ASSERT_TRUE(log.append(tail));
-    ASSERT_TRUE(log.load(term, votedFor, lastIndex));
-    EXPECT_EQ(log.firstIndex(), 6);
-    ASSERT_TRUE(log.append(entries));
-    ASSERT_TRUE(log.compact(3, 1));  // drop prefix <= 3
-    EXPECT_EQ(log.firstIndex(), 4);
-    ASSERT_TRUE(log.load(term, votedFor, lastIndex));
-    EXPECT_EQ(log.firstIndex(), 4);
-    ASSERT_TRUE(log.append(head));
-    ASSERT_TRUE(log.compact(4, 1));
-    ASSERT_TRUE(log.load(t, v, last));
-    ASSERT_TRUE(log.append(tail));
-    ASSERT_TRUE(log.load(term, votedFor, lastIndex));
-    EXPECT_EQ(log.firstIndex(), 5);
```

**逐条解释（15/15 都是 `log.` → `log->` 的机械改写，无一条弱化/删除）**：

| # | 原文行 | 新行（对应 `+` 侧） | 性质 |
|---|---|---|---|
| 1 | `ASSERT_TRUE(log.append(head));` | `ASSERT_TRUE(log->append(head));` | `FileLogStore log` → `auto log = makeDiskLog(eng, dir)`（`unique_ptr`）⇒ `.`→`->` |
| 2 | `... log.append(tail)` | `log->append(tail)` | 同上 |
| 3 | `... log.load(term, votedFor, lastIndex)` | `log->load(...)` | 同上 |
| 4 | `EXPECT_EQ(log.firstIndex(), 6);` | `log->firstIndex()` | 同上 |
| 5 | `... log.append(entries)` | `log->append(entries)` | 同上 |
| 6 | `... log.compact(3, 1); // drop prefix <= 3` | `log->compact(3, 1);` | 同上（注释保留在 `+` 侧） |
| 7 | `EXPECT_EQ(log.firstIndex(), 4);` | `log->firstIndex()` | 同上 |
| 8 | `... log.load(term, votedFor, lastIndex)` | `log->load(...)` | 同上 |
| 9 | `EXPECT_EQ(log.firstIndex(), 4);` | `log->firstIndex()` | 同上 |
| 10 | `... log.append(head)` | `log->append(head)` | 同上 |
| 11 | `... log.compact(4, 1)` | `log->compact(4, 1)` | 同上 |
| 12 | `... log.load(t, v, last)` | `log->load(t, v, last)` | 同上 |
| 13 | `... log.append(tail)` | `log->append(tail)` | 同上 |
| 14 | `... log.load(term, votedFor, lastIndex)` | `log->load(...)` | 同上 |
| 15 | `EXPECT_EQ(log.firstIndex(), 5);` | `log->firstIndex()` | 同上 |

**结论**：`tests/` 的 15 行“被删断言”**全部**是 `log.`→`log->` 的机械改写（`git diff` 的 `+` 侧可见同一条断言）；
**没有**任何断言被删除、弱化、改判据或 `DISABLED_`。
补充：`FileLogStore.RestartRestoresMetaAndLog` 等**既有用例名保留**；用例体抽成
`RestartRestoresMetaAndLogBody<Store>` 后**两个引擎跑同一份断言文本**（`m6-evidence.md` §M6.3 原文：
`grep -c 'RestartRestoresMetaAndLogBody<FileLogStore>' → 1`、`<LsmLogStore>' → 1`）。

### 8.3 三个 fault 脚本在 lsm 引擎下的轮数（是否满足“各 10 轮”下限）

| 脚本（lsm 引擎） | 实测轮数 | 是否 ≥10 | 原文 / 来源 |
|---|---|---|---|
| `raft_fault.sh --log-engine lsm --repeat 50` | **50** | ✅ | `m6-evidence.md` L638-640：`raft_fault: PASS (50 iterations)`；`m6-bench.md` H5 |
| `raft_membership_fault.sh --log-engine lsm --repeat 50` | **50** | ✅ | `m6-evidence.md` L642-644：`raft_membership_fault: PASS (50 iterations)`；`m6-bench.md` H7 |
| `raft_snapshot_fault.sh --log-engine lsm --repeat 50` | **50**（重标定后） | ✅ | `m6-bench.md` §9.5（L489-497）：`C) fault injection PASS (50 iterations)` + `raft_snapshot_fault: PASS` |

**但要如实登记两段历史**：
- `raft_snapshot_fault.sh --log-engine lsm --repeat 50` 在基座 `f06a44d` + **4 MiB 判据**下曾
  **FAIL（红）**：A 段空间判据 `4450355 > 4194304`，exit 1，B/C 段未执行（`m6-bench.md` §7.1/§9.3）。
  后在 **8 MiB 重标定**下四臂（lsm/file/base/b1）`--repeat 50` 全 PASS（`m6-bench.md` §9.5）。
   ⇒ “各 10 轮下限”**在最终口径下满足**，但**其中一项依赖一次判据重标定**，不是“原判据下一次通过”。
- base 臂的 `raft_snapshot_fault`/`raft_membership_fault` 在 M6.6 时点“未跑”，后来在 `m6-bench.md` §9.5 补齐。

---

## §9 未复核 / 缺口清单（**不得把未验证写成通过**）

> 这是本文件的“诚实账本”。每条格式：**缺口 → 后果 → 补测建议**。

| ID | 缺口 | 后果 | 补测建议 |
|---|---|---|---|
| G1 | **M6-I8（两套 compact 不互破坏）无任何实测** | lsm 后台 compaction 与日志 compact 的交互完全未验证 | 定向：把 `max_bytes_for_level_base` 调小 + 并发 `compact`/写/slice，断言保留后缀不变；或 `lsm_compaction_rounds>0` 的脚本 |
| G2 | **M6-L2 无直接 lockprobe 断言覆盖 `LsmLogStore`** | “锁内不 fsync”只对 RaftNode 调用点成立，对适配层内部不成立 | 给 `LsmLogStore` 加 `consensusHeld()` 探针（或在 RaftNode+LsmLogStore 组合用例里用 `SpyLogStore` 式包裹） |
| G3 | **M6-L3 compact 最坏阻塞上界未测**（且 `compact` 每 4096 条一次 fsync 持适配层锁） | 选举/心跳延迟尾部长，量级未知 | 定向基准：compact 大区间期间测 tick 延迟分布；并把 compact 的分块 fsync 改为“一次 sync”或缩小持锁窗口 |
| G4 | **M6-I2 的 lsm 内部 durable 落点仅转述论证** | “lsm `Write{sync=true}` 返回 ⟹ 已 fsync”未在本轮逐行复核（pin 已丢） | 用 `~/lsm-kv @ f06a44d` 只读复核 `DB::Write/Sync` 的三段式；或写“卡住 lsm sync ⇒ 不 ack”的注入用例 |
| G5 | **TSan 全量口径未复跑**（16 条 vs 0 报告的差异已定位为缺 `TSAN_OPTIONS`） | 验收口径仍未在本轮闭合 | 下一步：按 §6 的 canonical 命令重跑（含 `TSAN_OPTIONS=suppressions=tests/tsan.supp`）；结果无论如何都按 §6.4 登记 |
| G6 | **掉电语义（页缓存丢失）未覆盖**（`truncateSuffixNoSync`） | 崩溃一致性只测得“kill -9 下页缓存存活” | 需假文件系统/掉电模型；否则永久登记为未覆盖 |
| G7 | **R2/R3/R5/R6/R7 性能与空间风险未采集定向数字**（slice 迭代器开销、truncate 写放大、stall、启动恢复、P99） | M6 的“适配”结论缺定量支撑 | 按 `m6-design.md` §6.4 的固定章节补 D2/D4/D6/D9/D10 |
| G8 | **R4 的空间判据经过一次重标定（4 MiB → 8 MiB）** | “通过”依赖判据放宽 | 需父代理/用户裁决接受重标定；或在安静机器上多轮重取分布 |
| G9 | **A/B 数字受并发重活污染**（`m6-bench.md` §6.6-D3、M6.7-D5：loadavg 4.7–15.9） | 性能/空间结论只作定性 | 安静机器重测（设计 §5.3 的纪律） |
| G10 | **`raftkv_tests`（M1，13）在 M6 文档里没有 HEAD 计数** | M1 回归状态在 M6 证据里未落地 | 下一步跑一次 `./build/bin/raftkv_tests` 并入库原始行 |
| G11 | **`file_log_store.cpp` 被改而 C1–C9 未列**（M6.0-D3/C10） | 清单完整性缺口（但已登记 + 有回归用例） | 已在 `m6-evidence.md` §M6.0 登记；本文件 §4.5 复核确认 |
| G12 | **B24/B26/B27（`upTo>lastIndex_`、跨批 append/compact、WriteBatch 上限）仅代码论证** | 大负载边界未构造 | 定向用例 |
| G13 | **M6-L4 关闭顺序无 lsm 进程级 sanitizer 覆盖** | 关闭期 UB 只能靠 Release e2e 发现 | 增一个“启动→写入→优雅关闭”的 ASan/LSan 进程用例（非 TSan，跨进程） |
| G14 | **`m6-evidence.md`/`m6-bench.md` 引用的全部 /tmp 原始日志与 pin 基座已丢**（§7） | 关键数字只能引用文档内嵌原文 | 按 §7.4 建 `docs/raw/` 并入库 |

---

## §10 结论

1. **M6 的适配层（`LsmLogStore`）在“可观察契约”层面基本守住**：I1/I3/I4/I5/I7/I9(载荷)/I10/I11/I12
   有“代码 + 单测（部分含逐字断言）+ 文档内嵌原文”三重或两重证据；I6 是 **机制被替换**（撕裂尾由
   lsm WAL 承担、空洞由“截断”改“拒绝”），可观察行为在 R1/R2/R4/R5/R6 等价。
2. **三个明确的缺口等级**：
   - **未复核**：M6-I8（两套 compact 交互）、M6-L3（compact 阻塞上界）、R2/R5/R6/R7（性能/恢复）、R10（切引擎）、I9（引擎回滚口径未执行）。
   - **需加强**：M6-L2（**没有直接覆盖 `LsmLogStore` 的 lockprobe 断言**；且 compact 持适配层锁做 fsync 会让共识锁跨 fsync 等待）。
   - **被替换**：M6-I6（重启恢复的实现机制）。
3. **TSan 口径分歧已定位**：raft-kv 的“0 报告”是**带 `TSAN_OPTIONS=suppressions=tests/tsan.supp` 的全量运行**；
   我方 16 条是**未带该选项**的全量运行。⇒ **先照原样跑同一条口径**（下一步），同时**正式登记差异**：
   “干净”必须以 canonical 命令定义，16 条（0 data race、与基线逐项一致）作为已知事实保留。
4. **纪律遵守**：本文件是 M6 期间**唯一新增/修改**的文件；未改任何代码/测试/他人文档；
   未对 `~/raft-kv`/`~/lsm-kv` 做任何 git 写操作；未跑构建/门禁/ASan/TSan/基准。
5. **给后续实现任务的前置约束**（可直接引用）：§1 的 I 表、§2 的 L 表、§3 的 R8/R4、§9 的 G1–G3/G5
   是“下一步必须先解决或先登记”的清单；**不得**在没有补测的情况下把 G1/G2/G3/G5 写成通过。

---

## 附录 A：本文件所有可复核命令（只读，均已在本轮跑过）

```
# 仓库状态与范围
git -C ~/raft-kv-lsm log --oneline -5
git -C ~/raft-kv-lsm status --short
git -C ~/raft-kv-lsm diff --stat 6aabc27..HEAD
git -C ~/raft-kv-lsm diff --stat 6aabc27..HEAD -- src/raft/raft_node.cpp src/raft/raft_node.h src/raft/log_store.h src/kv/
git -C ~/raft-kv-lsm diff 6aabc27..HEAD -- src/raft/file_log_store.cpp src/main_raft_node.cpp

# 测试计数与断言对账
for f in ~/raft-kv-lsm/tests/*.cpp; do grep -cE '^TEST(_F|_P)?\(' "$f"; done
git -C ~/raft-kv-lsm diff -U0 1463620..HEAD -- tests/ | grep -E '^-.*(EXPECT_|ASSERT_)'

# layering
grep -rn 'lsm/\|lsm::\|#include.*lsm' ~/raft-kv-lsm/src/raft/ ~/raft-kv-lsm/src/kv/ ~/raft-kv-lsm/src/main_raft_node.cpp

# TSan 口径（raft-kv，只读）
sed -n '655,690p;825,860p' ~/raft-kv/docs/m5-design.md
cat ~/raft-kv/tests/tsan.supp
grep -rn 'setarch\|TSAN_OPTIONS\|gtest_filter' ~/raft-kv/docs/ ~/raft-kv/CMakeLists.txt
grep -n 'setarch\|TSAN_OPTIONS' ~/raft-kv-lsm/docs/m6-bench.md ~/raft-kv-lsm/docs/m6-evidence.md

# 证据丢失确认
ls -la /tmp/r3-tsan-full2.log /tmp/lsm-pin-b1bd050
```

---

## 附录 B：证据强度图例

| 图例 | 含义 |
|---|---|
| **有原始输出（内嵌）** | `m6-evidence.md`/`m6-bench.md` 里当时粘贴的命令 + stdout（原始 /tmp 日志已丢，见 §7） |
| **有用例名** | 可在 HEAD 的 `tests/` 里找到的 `TEST(...)`；本轮未跑，只确认其存在与断言文本 |
| **仅论证无测试** | 只有代码/设计论证，没有命令行输出或用例支撑 |
| **未复核** | 本轮既无实测也无完整可追溯论证 |
