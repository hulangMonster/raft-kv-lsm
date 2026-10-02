# M6 设计：把 raft-kv 的存储层适配到本项目的 lsm 引擎

> **状态**：v1.0 草案（M6 `#0` 产出，待评审）
> **改动范围**：仅 `~/raft-kv-lsm` 内的文件。**不碰** `~/raft-kv`（A/B 基线与回滚参照）、**不碰** `~/lsm-kv`（M5.3 在跑）。
> **本文档不 commit、不 push**：提交由父代理执行。
> **tag 政策**：`m6-raft-integration` 打在 **lsm 仓库 `~/lsm-kv`** 上 —— 该仓库**不在 M6 的改动范围内**，因此 **由父代理执行**（见 §8 D11）。

---

## §0 现状探测（只读侦察 + 原始输出）

本节所有结论都可追溯到「文件名 + 行号」。**未跑过的命令一律标注「未验证」。**

### 0.1 三个工作副本的实际状态

命令：

```bash
ssh ubuntu-vm 'cd ~/raft-kv-lsm && git log --oneline -3 && git remote -v && git status --short && git rev-parse HEAD'
ssh ubuntu-vm 'cd ~/raft-kv && git log --oneline -1 && git status --short | head -5 && git remote -v | head -2'
ssh ubuntu-vm 'cd ~/lsm-kv && git log --oneline -1 && git tag'
```

原始输出（节选）：

```
# ~/raft-kv-lsm
1463620 bench: scripts/bench_m5_cell.sh 支持 --nodes N（= docs/m5-bench.md §3.12 的复现口）+ 文档同步
4a4fe7b docs(m5): 节点规模（3/5/10）与引擎选择实测入档 —— N≥5 建议 reactor
origin  git@github.com:hulangMonster/raft-kv-lsm.git (fetch)
upstream        git@github.com:hulangMonster/raft-kv.git (fetch)
<git status --short 无输出 ⇒ 工作树干净>
1463620333eb2f16673f3d541aa62e1a35333c8a
origin/master
1463620333eb2f16673f3d541aa62e1a35333c8a

# ~/raft-kv（A/B 基线）
1463620 bench: scripts/bench_m5_cell.sh 支持 --nodes N（= docs/m5-bench.md §3.12 的复现口）+ 文档同步
?? scripts/demo_record.sh          ← 未跟踪文件，**M6 不得触碰**
origin  git@github.com:hulangMonster/raft-kv.git (fetch)

# ~/lsm-kv（lsm 引擎，只读）
99c417f feat(m5): M5.1 Bloom filter + M5.2 WriteBatch（含一个真实回归修复）
m1-memtable  m2-wal  m3-sstable  m4-compaction
```

**结论（事实）**：
- `~/raft-kv-lsm` 的 HEAD 与远端基线**完全一致** = `1463620333eb2f16673f3d541aa62e1a35333c8a`，工作树干净，没有 M6 的任何改动。
- `~/raft-kv` 与 `~/raft-kv-lsm` 是**同一份代码的两个克隆**（HEAD 短 hash 都是 `1463620`），因此 A/B 的两个臂在起点上是等价的 —— 这使「基线 vs 工作区」的对比只反映 M6 的改动。
- `~/lsm-kv` HEAD = `99c417f`，**没有 `m5-*` tag**（已有 tag 只到 `m4-compaction`）。父代理的检查点说 tag 有 `m1..m4`，与此一致。
- `~/raft-kv-lsm` 已有 tag：`m3-snapshot` / `m4-membership` / `m5-performance`。

> ⚠️ 父代理说 `~/lsm-kv` 的 tag 含 `m5-*`；实测**未找到** `m5-*` tag（只有 4 个）。以实测为准，登记为 §8 D11 的输入。

### 0.2 要适配的接口：`src/raft/log_store.h`

**逐方法签名**（行号 = `src/raft/log_store.h`）：

| 行号 | 签名 | 语义承诺（逐字摘自注释） |
|---|---|---|
| L14-16 | `class LogStore` | 「Durable log + metadata (term / votedFor). This is the single source of persistence truth in M2 (decision D1). **Implementations must serialize internal state** (file I/O is the only blocking operation they may do).」 |
| L23 | `bool load(Term& term, int& votedFor, Index& lastIndex)` | 「Startup recovery: read term / votedFor / last log index, **truncate a torn tail**. Returns false on unrecoverable I/O error.」 |
| L26 | `bool persistMeta(Term term, int votedFor)` | 「Must return true **only after** term/votedFor are durable (fsync'd).」 |
| L29 | `bool append(const std::vector<LogEntry>& entries)` | 「Append entries; returns true **only after** they are durable (fsync'd).」 |
| L37 | `bool appendNoSync(const std::vector<LogEntry>& entries)` | 「write entries **WITHOUT** fsync … `appendNoSync()` must still make the entries **visible to `slice()`/`lastIndex()`** and must be serialized by the caller (`RaftNode::mu_`).」 |
| L38 | `bool sync()` | 「`sync()` may be called **WITHOUT** `RaftNode::mu_` (it is the only operation that runs outside it), so every implementation must make `sync()` safe against concurrent fd/structural mutation internally.」 |
| L42 | `bool truncateSuffix(Index fromIndex)` | 「Drop **[fromIndex, lastIndex]** (conflict overwrite). 含 fsync 的截断：只允许在**不持** `RaftNode::mu_` 时调用（I9）。」 |
| L50-52 | `bool truncateSuffixNoSync(Index fromIndex)` | 「只做 ftruncate/内存截断、不 fsync 的变体，供持 mu_ 的冲突回滚路径使用（I9）。**调用方必须在同一批里随后 `sync()`；否则崩溃后变短的日志可能复活。** 默认实现转调 `truncateSuffix()`，因此**凡是 `truncateSuffix()` 会 fsync 的实现都必须同时覆写本方法**（FileLogStore 已覆写；M5.A9 会抓住漏网者）。」 |
| L54-55 | `std::vector<LogEntry> slice(Index from, size_t maxEntries, size_t maxBytes) const` | 无独立注释；语义由实现与调用方共同钉住（见 §0.4）。 |
| L57 | `Index lastIndex() const` | — |
| L58 | `Term lastTerm() const` | 「kNoTerm when empty」 |
| L59 | `Term termAt(Index index) const` | 「kNoTerm when out of range」 |
| L62 | `void setBoundary(Index lastIncludedIndex, Term lastIncludedTerm)` | 「M3: prefix compaction」 |
| L63 | `bool compact(Index upTo, Term termAtUpTo)` | 同上 |
| L64-66 | `Index firstIndex() const` / `lastIncludedIndex()` / `lastIncludedTerm()` | 「`firstIndex()` == `lastIncludedIndex() + 1`」 |

关键类型（`src/raft/types.h`）：
- L11-12 `using Term = uint64_t; using Index = uint64_t;`
- L14-15 `constexpr Index kNoIndex = 0;`（**索引 1-based**）、`constexpr Term kNoTerm = 0;`
- L19-27 `struct LogEntry { Index index; Term term; raftkv::OpCode op; std::string key; std::string value; uint64_t clientId; uint64_t requestId; };`

辅助编码（`src/common.h`）：L10 `using Byte = uint8_t;`、L11 `using Bytes = std::vector<Byte>;`、L15 `enum class OpCode : uint8_t`、L43 `putU32`、L52 `getU32`。

### 0.3 现有文件实现（`src/raft/file_log_store.cpp`）的关键语义

| 行号 | 事实 |
|---|---|
| L3-10 | 磁盘布局：`meta.dat` = `[crc32:4][len:4][term:8][votedFor:4]`（tmp+rename）；`raft.log` = 重复 `[crc32:4][len:4][payload]`，`payload = [index:8][term:8][op:1][keyLen:4][valLen:4][clientId:8][requestId:8][key][value]`，**全大端** |
| L30-32 | `kEntryFixedLen = 41`（8+8+1+4+4+8+8）、`kMetaPayloadLen = 12`、`kFrameHeaderLen = 8` |
| L105-118 | `encodeEntry()`：上面那条 payload 布局的唯一实现（**M6 直接复用**） |
| L120-142 | `decodeEntry()`：校验 `n == 41 + keyLen + valLen`，并拒绝未知 `op` |
| L146-161 | 构造函数：`create_directories(dir_ + "/raft")`，`open(raft.log, O_RDWR|O_CREAT|O_APPEND)`；失败 `throw std::runtime_error` |
| L167-245 | `load()`：先读 meta（CRC 校验，坏则退化为 `kNoTerm/-1`），再顺序扫描 `raft.log` |
| L219-222 | 索引 **< boundary 的记录被跳过**（`if (e.index < first) { ... continue; }`） |
| L223-228 | **「已压缩的前缀不见了」⇒ `return false`**：「A record above the boundary without its predecessors means the compacted prefix is gone: this log can no longer be replayed. Report it instead of silently truncating the whole file away.」判据是 `e.index != first` |
| L229-231 | 中间出现空洞 ⇒ `break`（当作撕裂尾），**截断** |
| L238-239 | `ftruncate(logFd_, validEnd)` —— 把撕裂尾物理丢弃 |
| L247-273 | `persistMeta()`：写 tmp → `fsync(fd)` → `rename` → `fsyncDir(dir_+"/raft")`；**返回 true 前已 durable** |
| L275-282 | `append()` = 持 `mu_` 调 `appendNoSyncLocked()`，**再在不持 `mu_` 的情况下调 `sync()`** |
| L284-304 | **`sync()` 只持叶子锁 `flushMu_`，fsync 期间不持 `mu_`** —— 注释 L288-291 记录了 M5.6（D-2）的实测：「实测 p=8/3 节点：leader 1650 次 fsync / 2200 条写（1.33 条/次）、batch_avg=1，吞吐掉到 M4 基线的 **0.47×**」 |
| L317-343 | `appendNoSyncLocked()`：`e.index <= lastIndex()` 时同 term ⇒ **幂等跳过**；异 term ⇒ 先 `truncateSuffixLocked(e.index, /*flush=*/false)`；然后要求 `e.index == lastIndex()+1`（D3 连续性），否则 `return false` |
| L359-391 | `truncateSuffixLocked()`：`fromIndex == kNoIndex` ⇒ true；`<= lastIncluded_` ⇒ **false**；`> lastIndex()+1` ⇒ **false**；否则 `ftruncate` 到该 offset（`fromIndex > lastIndex()` 时 truncate 到 EOF，即 no-op） |
| L393-409 | `slice()`：`start = max(from, firstIndex())`（D3 clamp）；`maxEntries` 上限；`maxBytes` 用 **`e.key.size() + e.value.size()`** 计（**不是**编码字节）；**L404 `if (!out.empty() && bytes + sz > maxBytes) break;` ⇒ 只要有可用条目就至少返回 1 条** |
| L411-431 | `lastIndex()` / `lastTerm()` / `termAt()`；`termAt(lastIncluded_)` 返回 `lastIncludedTerm_`（D3 边界条目） |
| L435-449 | `setBoundary()`：**纯内存**（不碰文件），设置边界并丢弃内存中 ≤ 边界的条目 |
| L451-511 | `compact()`：把保留后缀**整份重写**到 `raft.log.tmp` → `fsync` → `rename` → `fsyncDir` → **重开 `logFd_`**；注释 L500-503 明确「打不开就是致命失败，不是『压缩跳过』」 |
| L513-518 | `firstIndex() = lastIncluded_ + 1` |

内存实现（`src/raft/memory_adapters.cpp`）：L13-18/L20-24/L26-42/L44/L50-60/L62-77/L79-96/L100-125 是**同一语义的纯内存版**；`MemoryLogStore::all()`（`log_store.h` L92-93）是**测试专用**的「整条日志」出口，`tests/raft_log_test.cpp` L38/L41/L60/L63/L88/L90 直接依赖它。

### 0.4 `RaftNode` 如何使用 `LogStore`（`src/raft/raft_node.cpp`）

启动恢复顺序（L59-113，注释逐字列出 5 步）：

| 行号 | 事实 |
|---|---|
| L62-92 | 先 `snapshots_->load(snap)`；成功则 `sm_.restore()`，然后 **L87 `log_.setBoundary(snap.lastIncludedIndex, snap.lastIncludedTerm)`**（**在 `load()` 之前**）；注释 L65-68 说明「能解码但恢复不了」的快照**不是**撕裂快照，必须拒绝启动 |
| L90-92 | 快照缺失/损坏（magic/version/CRC）⇒ **退回全量重放**（D2 保证：只有快照 durable 之后才 compact） |
| L94-106 | `log_.load(t, v, last)`；**失败即 `throw std::runtime_error("raft: log load failed (I/O error or torn log)")`**。注意 `last` 出参**未被使用** |
| L112 | `syncedIndex_ = log_.lastIndex();  // everything recovered from disk is durable` |

运行期调用点：

| 行号 | 调用 | 上下文 |
|---|---|---|
| L335 | `log_.appendNoSync({noop})` | 持 `mu_`；leader 当选后的 §8 屏障条目 |
| L350 | `log_.sync()` | **锁外**（M5.2/I9） |
| L442-461 | `flushMetaOutsideLock()` → `log_.persistMeta(t, v)` | 持 `metaPersistMu_`、**不持 `mu_`** |
| L488-504 | `onRequestVote`：`log_.lastTerm()/lastIndex()` 判 up-to-date → `log_.persistMeta(grantedTerm, candidateId)` | **I5：没有 durable 就绝不授权**（L502-513） |
| L532-535 | `args.prevLogIndex < log_.lastIncludedIndex()` ⇒ 回 `firstIndex()/lastIncludedTerm()` | m3-design §5.5 |
| L536-538 | `args.prevLogIndex > log_.lastIndex()` ⇒ 回 `lastIndex()+1` | |
| L539-548 | `log_.termAt(prevLogIndex)` 冲突检测，**`while` 回退逐个 `termAt(conflictIndex-1)`** ← 反向逐条点查 |
| L550-576 | 冲突：`log_.truncateSuffixNoSync(e.index)`（**持 `mu_`**）→ `toAppend.push_back` → `log_.appendNoSync(toAppend)` |
| L583-593 | `needSync = (lastMatched > syncedIndex_) || truncated`；**解锁后** `log_.sync()` |
| L605 | `if (durable > log_.lastIndex()) durable = log_.lastIndex();` ← 夹紧 |
| L745-749 | `appendEntryLocked()` = `log_.appendNoSync({e})`（**持 `mu_`**） |
| L772-779 / L910-922 | `awaitCommit`：`log_.termAt(index) == term` 复核（M5.6 G1/F3 丢写修复） |
| L808 | `log_.sync()`：**ONE fsync for every entry so far**（锁外） |
| L975 | `snapTerm = log_.termAt(snapIndex)` |
| L1013-1018 | `log_.compact(snapIndex, snapTerm)`（**锁外**）；失败即 `throw` |
| L1093-1107 | InstallSnapshot：`log_.termAt(installed.lastIncludedIndex)` 不匹配则 `log_.truncateSuffixNoSync(log_.firstIndex())`；随后 `log_.compact(...)` |
| L1175-1180 | `rebuildConfigFromSeedAndLog()`：`log_.slice(first, last-first+1, SIZE_MAX)` ← **全量扫描** |
| L1359-1364 | `recomputeConfigLocked()`：同上**全量扫描**（冲突截断后触发） |
| L1779 | `hasCurrentTermCommitLocked()`：`log_.termAt(commitIndex_) == currentTerm_` |

锁纪律（`src/raft/raft_node.h` L29-34）：`mu_` 保护全部共识状态；`sync()/persistMeta()/compact()` 都在 `mu_` 之外执行（I9），并有 `lock_probe.h` 的**可断言**基础设施（L39 `consensusHeld()`、L192-239 `SpyLogStore`、L243-301 `BlockingLogStore`）。

### 0.5 门禁脚本与 CMake 目标（`CMakeLists.txt`、`scripts/`）

CMake 目标清单（`CMakeLists.txt`）：

| 行号 | 目标 |
|---|---|
| L30-37 | `raftkv_core`（M1：codec/wal/store/server） |
| L39-43 | `raftkv_server` / `raftkv_cli` |
| L46-52 | `raftkv_tests` + `add_test(raftkv_unit ...)` |
| L55-70 | **`raftkv_raft`** ← M6 要新增源文件的库；已有 `memory_adapters.cpp / raft_node.cpp / file_log_store.cpp / ...` |
| L72-76 | `raftkv_raft_node` / `raftkv_raft_cli` |
| L80-105 | `raftkv_raft_tests`（11 个 `tests/raft_*.cpp`）+ `add_test(raftkv_raft ...)` |

门禁脚本（`scripts/`，实测 greps）：

| 脚本 | 判定行 | 性质（见 §5.2） |
|---|---|---|
| `raft_e2e.sh` | L127 `raft_e2e: PASS` | **硬门禁** |
| `raft_fault.sh` | L134 `raft_fault: PASS ($REPEAT iterations)` | **硬门禁** |
| `raft_snapshot_e2e.sh` | L146 `raft_snapshot_e2e: PASS` | **硬门禁** |
| `raft_snapshot_fault.sh` | L187-188 `C) fault injection PASS` / `raft_snapshot_fault: PASS` | **硬门禁**；⚠️ L23 `LOG_BOUND=1048576`、L116 `stat -c%s "$WORK/node$id/raft/raft.log"`、L117-119 超限即 `exit 1` —— **直接依赖 `raft.log` 这个文件路径** |
| `raft_membership_e2e.sh` | 各 `FAIL`/`exit 1` + 成功即 0 | **硬门禁** |
| `raft_membership_fault.sh` | 同上 | **硬门禁** |
| `bench_m5_ab.sh` | L7-13 判据 + L100 `VERIFY_FAILED` + L159 | A/B 基准（**模板**） |
| `bench_m5_cell.sh` | L22-24 退出码 0/1/2 | 单格诊断 |
| `bench_group_commit.sh` | — | 观测 |
| `e2e.sh` | L96 `e2e: PASS` | M1 门禁（与 raft 无关） |

`bench_m5_ab.sh` 的可复用机制（**M6 直接照搬**）：L2-4「同脚本内交替测量」「每档 200 写预热丢弃」「每档重复 N 次取中位数」；L100「`verify` 必须含 `missing 0`，否则整轮作废」。

进程接线（`src/main_raft_node.cpp`）：L376-378 `FileLogStore log(dataDir); FileSnapshotStore snapshots(dataDir); KvStateMachine sm;`；L380-392 transport 引擎可切换（`--transport=reactor|sync` / `RAFTKV_TRANSPORT` ← **M6 的 `--log-engine` 照此模式**）；L413 `RaftNode node(cfg, log, sm, transport, clock, &snapshots, seed, &metrics);`。

### 0.6 lsm 侧的对外契约（**VM 上 `~/lsm-kv` @ `99c417f` 为准**；本机克隆落后，不含 M5 文档）

| 行号 | 事实 |
|---|---|
| `src/db.h` L29 | `static Status Open(const Options& options, const std::string& name, DB** dbptr);` —— **name 非空 = 持久模式（WAL + 恢复）**；「目录不存在则创建」「任何失败路径都必须 `*dbptr = nullptr`」 |
| `src/db.h` L36-40 | `Put(WriteOptions, Slice, Slice)` / `Delete(WriteOptions, Slice)` / 便捷重载 |
| `src/db.h` L46 | `Write(const WriteOptions&, WriteBatch* updates)` —— 「一个 WriteBatch = 一条 WAL record，**整批原子可见（I51）**；批内 sequence 连续且与提交顺序一致（I52）」 |
| `src/db.h` L48 | `Get(const Slice& key, std::string* value)` |
| `src/db.h` L51 | `NewIterator()` —— 「user key 升序、**每个 key 只出最新可见版本**、跳过 tombstone」 |
| `src/db.h` L54 | `Sync()` —— 「把此前**所有**已返回 kOk 的写入刷到磁盘（返回即全部 durable）；幂等」 |
| `src/db.h` L17-19 | `struct WriteOptions { bool sync = false; };` —— 「true = durable-before-ack；false = 只 write 不 fsync」 |
| `src/common.h` L285-320 | `Options`：`write_buffer_size=4MiB`、`env`、`block_size=4096`、`max_open_files=64`、`max_bytes_for_level_base=10MB`、`bloom_bits=10` 等 |
| `src/common.h` L325-345 | `Iterator`：`Valid/SeekToFirst/SeekToLast/Seek/Next/Prev/key/value/status`；**`key()` 在下一次定位调用后失效** |
| `src/common.h` L212-236 | `InternalKeyComparator`：user key 升序 → **trailer 降序**（sequence 大者在前） |
| `src/write_batch.h` L40-49 | `Put/Delete/Clear/Count/ByteSize/Data` |
| `src/write_batch.h` L31-32 | `kMaxCount = 1<<20`、`kMaxBytes = 64 MiB` |
| `docs/m2-design.md` §7.1（L879-890） | **durable-before-ack 的三段式结构性论证**：`Append` 返回 kOk ⟺ 字节已 write；`Sync` 返回 kOk ⟺ fsync 成功；`durable_seq_ >= w.end_seq` 只在同一临界区推进 ⇒ **「已 ack ⟹ 已 durable」** |
| `docs/m2-design.md` §7.2（L894-901） | 语义表：`sync=false` 崩溃后**允许丢最近的连续后缀**；**不得**出现半条生效 / 乱序生效 / 旧值覆盖新值（I19 + §7.3 三条硬约束 L903-911） |
| `docs/m2-design.md` §5.3（L592-622） | **尾部 vs 中间损坏判定**：`TAIL_RESIDUE` 或「其后无完好 record」⇒ 截断；「其后仍有完好 record」⇒ `kCorruption`、**拒绝启动**、不截断任何字节 |
| `docs/m2-design.md` D10（L238-243） | `LOCK` 文件（`Env::LockFile`）⇒ **一个数据目录同一时刻只能被一个进程 `Open`** |
| `docs/m2-design.md` D11（L245-252） | fsync/短写失败 ⇒ **粘性 fail-stop**，DB 转「写只读」 |
| `docs/m2-design.md` I11-I20（L96-115） | I15 「一条 record = 一个原子批」、I18 「恢复只读」、I19 「无撕裂值」 |
| `CMakeLists.txt` L46-78 | `add_library(lsm STATIC ...)`，`target_include_directories(lsm PUBLIC .../src)`，`target_link_libraries(lsm PUBLIC Threads::Threads)`；**已产出 `build/liblsm.a`（实测 1 054 968 字节）** |
| `src/db_impl.cpp` L154-198 | `SubmitPending()`：**L166 `std::unique_lock<std::mutex> l(commit_mu_);`** 覆盖「入队 + 等结算」全程；L178 `queue_.push_back(w)`（FIFO）；L175 `if (!bg_error_.ok()) return bg_error_;`（fail-stop 粘性） |
| `src/db_impl.cpp` L224-246 | `Write()`：先 `Validate()`（**入队前**预校验，失败不触发 fail-stop），再 `SubmitPending` |
| `src/db_impl.cpp` L409-433 | **sequence 在 flusher 的 phase B、持 `commit_mu_` 下按队列顺序分配** ⇒ **sequence 顺序 == `DB::Write` 调用顺序**（I52） |
| `src/db_impl.cpp` L436-443 | phase C（**锁外**）：`log_->Append(payload)`；`need_sync` 时才 `log_->Sync()` |
| `src/db_impl.cpp` L458-491 | memtable `Add` 发生在 Append/Sync **之后**，`w.done = true` 在 L485-489 ⇒ **`DB::Write` 返回时条目已可见**（满足 `appendNoSync` 的可见性要求） |
| `src/db_impl.cpp` L680-687 | `NewIterator()`：`snapshot = last_sequence_`（持 `mutex_` 读）⇒ **迭代器是创建时刻的一致快照** |
| `src/db_impl.cpp` L800-824 | **`Sync()`：L802 `std::lock_guard<std::mutex> ql(commit_mu_);`，L817 `log_->Sync()` —— 整个 fsync 都在 `commit_mu_` 之下** |

### 0.7 未验证清单

以下**没有**实测，本文档不据此下结论：

1. LSM 引擎下 `raftkv_raft_node` 的实际吞吐 / 延迟 / P99 —— **未验证**（这正是 M6.7 要测的）。
2. `DB::Sync()` 持 `commit_mu_` 造成的写阻塞在本机（VMware Ubuntu）上的**具体量级** —— 未验证；只有 M5 在 `FileLogStore` 上的**类比**数据（`file_log_store.cpp` L288-291：0.47×）。
3. lsm 的 `Options` 取值（`write_buffer_size` / `bloom_bits` / `max_open_files`）对 raft 日志负载是否合适 —— 未验证。
4. 「在同一个 `--data-dir` 上从 file 引擎切到 lsm 引擎」是否可行 —— **未验证**（§7 W6 给出的是推理，不是实测）。
5. `~/lsm-kv` 的 `m5-*` tag 是否存在 —— **未找到**。
6. 本机（Windows）**没有 g++/cmake**；本设计的所有构建/测试命令都必须在 `ubuntu-vm` 上执行。

---

## §1 目标 / 非目标

### 1.1 目标（M6 的全部内容）

1. 新增一个 `LogStore` 实现（`LsmLogStore`），**用 lsm 引擎作为 raft 日志与 `term/votedFor` 的唯一持久化真相源**。
2. 通过一个**开关**（默认关闭）把 `raftkv_raft_node` 切到该实现，使 `~/raft-kv`（基线）与 `~/raft-kv-lsm`（LSM 版）能在**同一套门禁**下交替 A/B。
3. 给出**可实测**的等价性与性能证据，并把「LSM 未必更快」如实入档。

### 1.2 非目标（硬边界）

| # | 非目标 | 理由 |
|---|---|---|
| N1 | **不改 Raft 算法**（选举/日志复制/提交/成员变更/线性一致读） | `raft_node.cpp` 的共识逻辑一行不动 |
| N2 | **不改网络与协议**（`message.{h,cpp}`、`transport_*`、`protocol.md`、`cluster_config`、快照格式） | 线上兼容性 |
| N3 | **不改 lsm 仓库**（`~/lsm-kv`） | 不在改动范围内；M5.3 正在那里作业 |
| N4 | **不改 `~/raft-kv`** | A/B 基线与回滚参照，必须保持原样 |
| N5 | **不删 `FileLogStore` / `MemoryLogStore`** | 它们是默认引擎与 M2-M5 门禁的载体 |
| N6 | 不做数据迁移工具（file 引擎 ⇄ lsm 引擎） | YAGNI；见 §7 W6 |
| N7 | 不引入二级索引、不引入范围删除、不改 lsm 的 `Iterator` 语义 | 最小可用映射优先（见 §7 W1/W4） |

### 1.3 允许的「必要小改」（逐条列清单 + 理由）

| # | 改动 | 位置 | 理由 |
|---|---|---|---|
| C1 | 新增 `src/raft/lsm_log_store.{h,cpp}` | 新文件 | 本里程碑的全部新逻辑 |
| C2 | `CMakeLists.txt`：把 `lsm_log_store.cpp` 加进 `raftkv_raft`；把 lsm 库接进来 | L55-70 附近 | 构建接线 |
| C3 | `src/main_raft_node.cpp`：新增 `--log-engine=file|lsm` + `RAFTKV_LOG_ENGINE`，**默认 `file`** | 仿 L314-333 的 `--transport` | 开关；默认值保证 M2-M5 门禁逐字节不变 |
| C4 | `tests/raft_restart_test.cpp`：把 `FileLogStore` 硬编码改为 `StoreFactory`，**既有 `FileLogStore.*` 用例名与断言不变**，新增 `LsmLogStore.*` 孪生用例 | L41-89 | 用同一组断言同时证明两个引擎 |
| C5 | `tests/raft_snapshot_test.cpp` / `raft_membership_test.cpp` 的 **Disk** fixture：同样参数化 | `RaftSnapshotDisk.*`（L327/L364/L551/L580/L866/L926）、`RaftMembershipDisk.*`（L1526/L1552/L1588/L1617） | 同上 |
| C6 | `scripts/raft_snapshot_fault.sh`：`LOG_BOUND` 判据**按引擎**取「LSM 数据目录总字节」而不是 `raft/raft.log` | L23/L116-119 | **lsm 引擎下 `raft.log` 不存在**；不改就是脚本直接失败 |
| C7 | `scripts/raft_*.sh`：把 `RAFTKV_LOG_ENGINE` 透传给 node 进程 | 各 `start_node()` | A/B 同一套脚本驱动两个引擎 |
| C8 | 新增 `scripts/bench_m6_ab.sh` / `docs/m6-bench.md` | 新文件 | A/B 证据与负结果入档 |
| C9 | `README.md` / `docs/roadmap.md` 增补 M6 一节 | — | 文档一致性 |

**C1-C9 之外的一切改动都需要父代理裁决。**

---

## §2 接口映射：`LogStore` × lsm

### 2.1 位级布局（key 编码）

**采用两个 tag 前缀，共享一个 lsm 键空间**（lsm 的 `Options::comparator` 默认是 `BytewiseComparator`，`Slice::compare` 逐字节**无符号**比较 —— `src/common.h` L56-65）。

```
常量：
  kLogPrefix   = 0x01                       // 1 字节 tag
  kMetaKey     = 0x02 'm' 'e' 't' 'a'       // 5 字节
  kLogBegin    = 0x01 00 00 00 00 00 00 00 00   // 闭端（index 0；真实索引 ≥ 1）
  kLogEnd      = 0x02 00 00 00 00 00 00 00 00   // 开端（== kMetaKey 的下界之前）

日志条目 user key（固定 9 字节）：
  偏移  长度  内容
  0     1     0x01
  1     8     index，uint64 大端

meta user key（固定 5 字节）：
  偏移  长度  内容
  0     1     0x02
  1     4     'm'(0x6D) 'e'(0x65) 't'(0x74) 'a'(0x61)
```

**有序性证明**：`0x01 < 0x02` ⇒ 全部日志 key **严格小于** meta key；日志 key 之间只由 8 字节大端索引区分，大端定宽编码下 `memcmp` 序 == 数值序 ⇒ **字节序 == 索引升序**。因此 `Seek(0x01 || BE64(i))` 精确命中索引 `i`（若存在），`SeekToFirst()`/`SeekToLast()` 限定在 `[kLogBegin, kLogEnd)` 内即可按索引遍历。

> **为什么用 `0x02` 而不是 `0x00`**：保留 `0x00` 段给将来的内部用途（例如二级索引 `0x03 || op || BE64(index)`，见 §7 W4），且 `0x00` 前缀在日志里不好肉眼辨识。

### 2.2 值编码

**日志条目 value** = **逐字复用** `file_log_store.cpp` L105-118 的 `encodeEntry()` 输出（`kEntryFixedLen = 41`，L30）：

```
偏移        长度        字段
0           8           index      uint64 BE   ← 与 key 冗余，用于 load() 自校验
8           8           term       uint64 BE
16          1           op         uint8
17          4           keyLen     uint32 BE
21          4           valLen     uint32 BE
25          8           clientId   uint64 BE
33          8           requestId  uint64 BE
41          keyLen      key 字节
41+keyLen   valLen      value 字节
```

**meta value** = 12 字节，与 `file_log_store.cpp` L247-258 的 `kMetaPayloadLen` 同构：

```
偏移  长度  字段
0     8     term      uint64 BE
8     4     votedFor  uint32 BE（int 的位模式；-1 ⇒ 0xFFFFFFFF）
```

**为什么 8 字节索引要冗余进 value**：① 复用已被 `raft_restart_test` 验证过的 `encodeEntry/decodeEntry`（零新编码面）；② `load()` 可以断言 `decoded.index == key index`，把「key/value 错配」变成启动期可直接报出的错误；③ 代价只是 8 字节/条。**备选（不推荐）**：省掉冗余索引，省 8B/条但丢掉自校验 —— 见 §8 D3。

### 2.3 内部状态（内存）

```cpp
class LsmLogStore : public LogStore {
  // ... 与 FileLogStore 同形的公开签名 ...
 private:
  std::string dir_;          // == <data-dir>/raft-lsm
  std::unique_ptr<lsm::DB> db_;
  mutable std::mutex mu_;    // 保护下面全部内存状态（recursive 不需要：内部不互相调用）

  Term  term_ = kNoTerm;
  int   votedFor_ = -1;
  Index lastIncluded_ = kNoIndex;
  Term  lastIncludedTerm_ = kNoTerm;

  // 术语向量：terms_[i - firstIndex()] == 索引 i 的 term。仅在 [firstIndex(), lastIndex_] 有意义。
  std::vector<Term> terms_;
  Index lastIndex_ = kNoIndex;   // == lastIncluded_ 表示日志为空
  Term  lastTerm_  = kNoTerm;    // == lastIncludedTerm_ 表示日志为空
  bool  poisoned_ = false;       // 一旦 lsm 报错，拒绝一切变更（§3.4）
};
```

**为什么保留「术语向量」**：`termAt()` 是**热路径点查**（`raft_node.cpp` L539-548 的冲突回退 `while` 循环、L772-779/L910-922 的 `awaitCommit` 复核、L1779 的读屏障），若每次都走一次 `lsm::DB::Get`（memtable → immutables → L0 逐文件 → 各级二分），代价与不确定性都不可接受。术语向量的内存是 **8 字节/条**，而 `FileLogStore` 缓存的 `entries_` 是 **整条 LogEntry**（`sizeof(LogEntry)` + key + value + 容器开销），因此 **LSM 版的内存严格更少**。

> 术语向量与 DB 的一致性由「**所有变更都经 `mu_` 串行 + 只在 `DB::Write` 返回 kOk 之后更新内存**」保证（§2.9）。

### 2.4 逐方法映射 + 语义等价性论证

约定：下表「等价性」列的判据都是「与 `FileLogStore` 在该输入下的**可观察行为**（返回值 + 后续 `slice/lastIndex/lastTerm/termAt/firstIndex/lastIncluded*` 的取值）逐条相同」。

#### 2.4.1 `bool load(Term& term, int& votedFor, Index& lastIndex)`

| 步骤 | FileLogStore（行号） | LsmLogStore |
|---|---|---|
| 打开持久化 | ctor L146-161 建 `<dir>/raft`、`open(raft.log)` | **ctor** 调 `lsm::DB::Open(opts, dir_ + "/raft-lsm", &db_)`；失败 ⇒ `throw std::runtime_error`（与 L151/L158 同形） |
| 读 meta | L173-189：读 `meta.dat`，CRC/长度校验，坏则退化为 `kNoTerm/-1` | `db_->Get(kMetaKey, &v)`；`NotFound` 或 `size() != 12` ⇒ 退化为 `kNoTerm/-1` |
| 清内存 | L192-193 `entries_.clear(); offsetOf_.clear();` | `terms_.clear();` |
| 求 first | L198 `const Index first = firstIndex();` | 同 |
| 顺序扫描 | L201-236 逐帧读 `raft.log`，CRC 校验 | `auto it = db_->NewIterator(); it->Seek(kLogBegin);` 在 `[kLogBegin, kLogEnd)` 内 `Next()` |
| 解码 + 自校验 | L216-217 `decodeEntry()`（校验 `n == 41+keyLen+valLen`、op 合法） | 从 key 解出 `idx`，`decodeEntry()` 解 value；**额外断言 `e.index == idx`**，不等 ⇒ `return false` |
| 跳过边界之下 | L219-222 `if (e.index < first) { ...; continue; }` | 同 |
| 前缀丢失 ⇒ 拒绝 | L223-228 第一条可用记录的 `index != first` ⇒ `return false` | 同（**逐字保留这条判据**） |
| 空洞 | L229-231 `e.index != entries_.back().index + 1` ⇒ `break`（撕裂尾，随后 ftruncate） | **`return false`**（理由见下 ⚠️） |
| 撕裂尾截断 | L238-239 `ftruncate(logFd_, validEnd)` | **不需要**：lsm 的 WAL 记录是原子单位（I15），`DB::Open` 已在恢复期把尾部残骸截断并**报告 `RecoveryStats::tail_truncated_bytes`**（`db_impl.h` L42）。本实现**必须把这个计数值打印/上报**，作为等价证据 |
| 出参 | L241-243 `term=term_; votedFor=votedFor_; lastIndex=this->lastIndex();` | 同 |
| 失败语义 | 唯一 `return false` 的路径（L227）会让 `RaftNode` 抛 `"raft: log load failed"`（`raft_node.cpp` L101-103） | 同 |

⚠️ **「空洞」处理的分歧（刻意的，必须写进 §8 拍板）**：`FileLogStore` 的中间空洞**只能**来自自己那套 `[crc][len]` 帧的撕裂写，所以它可以安全地「当作撕裂尾截断」。lsm 的 WAL 保证「一条 record = 一个原子批」（`m2-design.md` I15）、且 `DB::Open` 已经把「尾部残骸 vs 中间损坏」判完了（`m2-design.md` §5.3 L592-622）。因此**到 `LsmLogStore::load()` 还能看到空洞**，意味着「两个引擎的保证都已失效」的介质级损坏 ⇒ **拒绝启动**（`return false`）比「静默截断掉一段可能已提交的日志」更安全。**推荐：拒绝**（§8 D5）。

**`firstIndex/lastIndex` 的重启恢复（这是本节的核心问题之一）**：

关键事实 —— **`LogStore` 自己并不持久化 `lastIncluded_`**：`FileLogStore::setBoundary()`（L435-449）是纯内存的，重启后的边界**来自快照**：`RaftNode` 构造函数在 `load()` **之前**先 `snapshots_->load()` 再 `log_.setBoundary(...)`（`raft_node.cpp` L62-92，注释 L86「D3: the boundary must be known before `load()` validates `firstIndex()`」）。`LsmLogStore` **完全沿用这条约定**，因此三种重启情形逐一分析：

| 情形 | 磁盘事实 | `setBoundary` | `load()` 看到 | 结果 |
|---|---|---|---|---|
| R1：无快照（从未 compact） | DB 里是 `[1, N]` 全部 | 不调用 ⇒ `lastIncluded_=kNoIndex`, `first=1` | 最小 key = 1 | `firstIndex()=1`, `lastIndex_=N` ✅ |
| R2：有快照 `I`，compact 已生效 | DB 里是 `[I+1, N]`（≤I 已被 tombstone） | `setBoundary(I, T)` ⇒ `first=I+1` | 最小 **live** key = I+1 | `firstIndex()=I+1`, `lastIndex_=N` ✅ |
| R3：有快照 `I`，**crash 在快照 durable 之后、compact 之前** | DB 里仍是 `[1, N]` | `setBoundary(I, T)` ⇒ `first=I+1` | 最小 live key = 1 **< first** ⇒ 按 L219-222 **跳过** | 第一条保留记录的 index == I+1 == first ⇒ `sawFirst=true` ✅ 与 FileLogStore **逐条同构** |
| R4：**快照丢失/损坏** + compact 已生效 | DB 里是 `[I+1, N]` | **不调用** ⇒ `first=1` | 最小 live key = I+1 **≠ 1** | ⇒ `return false`，节点拒绝启动 ✅ 这正是 `raft_node.cpp` L65-68 / L90-92 与 D2 保证所要的行为 |
| R5：日志为空且边界 `I>0` | DB 里没有任何 `0x01*` key | `setBoundary(I, T)` | 无记录 ⇒ `sawFirst` 保持 false | `lastIndex_ = lastIncluded_ = I`，`firstIndex() = I+1`，`lastTerm() = lastIncludedTerm_ = T` ✅（与 L413-414/L420 的「空 ⇒ 返回边界」同构） |
| R6：全新节点 | 空 DB，无 meta | 不调用 | 无记录 | `term=kNoTerm, votedFor=-1, lastIndex=0(kNoIndex)` ✅ |

> R2/R4 还依赖一个 lsm 事实：`NewIterator()` 的用户视图**跳过 tombstone**（`db.h` L51）并且**每个 key 只出最新可见版本**，因此 `compact()` 写下的 tombstone 会让 ≤I 的索引在扫描中消失。**这是本设计成立的最关键的一条 lsm 语义依赖**，M6.2 必须有一条定向用例直接钉住它（§4-M6.2 `LsmLogStore.CompactDropsPrefixAndSurvivesRestart`）。

#### 2.4.2 `bool persistMeta(Term term, int votedFor)`

```
lsm::WriteBatch wb;
wb.Put(kMetaKey, EncodeMeta(term, votedFor));        // 12 字节，单条 Put
return db_->Write(lsm::WriteOptions{/*sync=*/true}, &wb).ok();
```

| 等价性要点 | 论证 |
|---|---|
| **必须是 fsync'd 之后才返回 true**（`log_store.h` L26） | `WriteOptions::sync = true` ⇒ lsm 的 flusher 在 `log_->Sync()` 成功并推进 `durable_seq_` 之后才置 `w.done`（`db_impl.cpp` L443、L450-452、L485-489），`DB::Write` 才返回 ⇒ **结构性成立**（`m2-design.md` §7.1 L879-890） |
| **term/votedFor 必须成对原子** | 单条 `Put` ⇒ batch 内 1 条 entry ⇒ 一条 WAL record ⇒ 一个 CRC ⇒ 原子（I15）。**比 `meta.dat` 的 tmp+rename 更简单且同样原子** |
| 持久化承诺的对齐 | 与 `FileLogStore::persistMeta`（L263-268：`write + fsync + rename + fsyncDir`）**对外承诺相同**：返回 true ⟹ 崩溃后可读 |
| 并发 | `RaftNode` 用 `metaPersistMu_` 串行化（`raft_node.cpp` L443 / L468），且 `persistMeta` **不持 `mu_`**（I9）—— lsm 的 `DB::Write` 本身线程安全 |
| 失败语义 | `.ok() == false` ⇒ 返回 false，且 `poisoned_ = true`。**lsm 自身已是 fail-stop**（D11，`db_impl.cpp` L175 粘性 `bg_error_`），与 `raft_node.cpp` L502-513「没有 durable 就绝不授权 / 绝不 ack」天然一致 |

> **备选（不推荐）**：把 `meta.dat` 原样搬过来复用 `FileLogStore::persistMeta`。代价是同一份日志用两套持久化机制（两个 fsync 目标、两处 fsyncDir），且 `load()` 要同时处理两条恢复路径。见 §8 D2。

#### 2.4.3 `bool append(const std::vector<LogEntry>& entries)`

```
if (!appendNoSync(entries)) return false;   // 持 mu_
return sync();                              // 不持 mu_（与 L280-281 同形）
```

与 `FileLogStore::append`（L275-282）**逐字同构**：先写、再在不持 `mu_` 的情况下 flush。返回 true ⟹ 已 durable。

#### 2.4.4 `bool appendNoSync(const std::vector<LogEntry>& entries)`

```
std::lock_guard<std::mutex> lk(mu_);
if (poisoned_) return false;
lsm::WriteBatch wb;
for (const LogEntry& e : entries) {
  if (e.index <= lastIndex_) {
    if (termAtLocked(e.index) == e.term) continue;        // 幂等（L321-322）
    if (!truncateLocked(e.index, wb)) return false;       // 冲突：把 Delete 追加进**同一个** batch
  }
  if (e.index != lastIndex_ + 1) return false;            // 连续性（D3，L327）
  wb.Put(EntryKey(e.index), EncodeEntry(e));
  // 同步维护 terms_/lastIndex_/lastTerm_
}
if (wb.Count() == 0) return true;
if (!db_->Write(lsm::WriteOptions{/*sync=*/false}, &wb).ok()) { poisoned_ = true; return false; }
return true;                                             // 不 durable；由调用方 sync()（组提交）
```

| 语义 | 等价性论证 |
|---|---|
| **幂等追加**（同 index 同 term 跳过） | 与 L320-322 逐字相同 |
| **冲突覆盖**（同 index 异 term ⇒ 先截断再追加） | 与 L323-326 相同；**差异是更强的保证**：`FileLogStore` 的截断是 `ftruncate`（立即生效），本实现把 `Delete(i)` 与后续 `Put(i, …)` 放进**同一个 WriteBatch**，sequence 在批内按 entry 顺序递增（`db_impl.cpp` L414-419）⇒ **后写的 Put 一定覆盖先写的 Delete**，且整批原子（I15）。因此「截断后追加」在本实现里是**一次原子提交**，而 `FileLogStore` 是「ftruncate + write」两次动作 |
| **连续性要求** | 与 L327 逐字相同（`e.index != lastIndex()+1` ⇒ false） |
| **返回时条目已可见** | `DB::Write(sync=false)` 返回时条目已进 memtable（`db_impl.cpp` L458-489 的 `Add` 在 `done=true` 之前），满足 `log_store.h` L32-33 的「must still make the entries visible to `slice()`/`lastIndex()`」 |
| **返回时不 durable** | `sync=false` ⇒ flusher 不做 fsync（`db_impl.cpp` L443 `if (s.ok() && need_sync)`；`need_sync` 取自组内 OR，`db_impl.cpp` L296 + L238）⇒ 满足 L31「write entries WITHOUT fsync」 |
| **调用方串行化** | `RaftNode` 在 `mu_` 下调用（L335/L556-L576/L746），本实现另有一把 `mu_` 自保 |
| **`WriteBatch` 上限** | `kMaxCount = 1<<20`、`kMaxBytes = 64 MiB`（`write_batch.h` L31-32）。`cfg_.maxEntriesPerAppend = 128`、`maxBytesPerAppend = 1 MiB`（`types.h` L111-112）⇒ **远低于上限**。但 `append()`（非 NoSync）可能被外部以更大批调用；实现必须**分批**（每批 ≤ `kMaxCount` 条且 ≤ 64 MiB），否则 `Validate()` 直接返回 `kInvalidArgument` |

#### 2.4.5 `bool sync()`

```
return db_->Sync().ok();      // **不取本类任何锁**
```

| 语义 | 等价性论证 |
|---|---|
| 覆盖「此前全部已 ack 的写」 | `DB::Sync()`（`db_impl.cpp` L800-824）先取 `log_last_appended_seq_` 再 `log_->Sync()` 再单调发布 `durable_seq_`，其契约是「把此前**所有**已返回 kOk 的写入刷到磁盘」（`db.h` L54）；`m2-design.md` §7.1 论证任何在本次 Sync 之前已返回 kOk 的写，其 `Append` 必然早于本次快照点 |
| **可以在不持 `RaftNode::mu_` 时调用** | `DB::Sync()` 是 `DB` 的线程安全方法；本实现**不持 `mu_`**，因此不存在「`sync()` 阻塞 `appendNoSync()`」的**本类内**耦合。（⚠️ lsm **内部**仍有一个阻塞点，见 §6 R1 —— 那是本设计最重要的性能风险） |
| 保守性 | `RaftNode` 在 `sync()` 返回后用 `durable = min(flushTarget, log_.lastIndex())` 夹紧（`raft_node.cpp` L820-821、L357、L605），因此 lsm 多刷了也不会导致**多报** durable |
| 幂等 / 空库 | `DB::Sync()` 幂等（`db.h` L54）；空库返回 `kOk` |

#### 2.4.6 `truncateSuffix(Index)` / `truncateSuffixNoSync(Index)`

```
bool truncateSuffix(Index fromIndex) override { return truncateLocked(fromIndex, /*flush=*/true); }

bool truncateSuffixNoSync(Index fromIndex) override {   // ★ 必须覆写（log_store.h L48-49）
  return truncateLocked(fromIndex, /*flush=*/false);
}
```

`truncateLocked(fromIndex, flush)`（持 `mu_`）：

```
1. if (fromIndex == kNoIndex) return true;                 // L360 同
2. if (fromIndex <= lastIncluded_) return false;            // L361 同「cannot cut below boundary」
3. if (fromIndex > lastIndex_ + 1) return false;            // L362 同
4. if (fromIndex <= lastIndex_) {
     lsm::WriteBatch wb;
     for (Index i = fromIndex; i <= lastIndex_; ++i) wb.Delete(EntryKey(i));
     if (!db_->Write({.sync = flush}, &wb).ok()) { poisoned_ = true; return false; }
     terms_.resize(fromIndex - firstIndex());
     lastIndex_ = fromIndex - 1;
     lastTerm_  = (terms_.empty() ? lastIncludedTerm_ : terms_.back());
   }
   // else: fromIndex == lastIndex_+1 ⇒ 无条目可删（等价于 L366 「fromIndex > lastIndex()」分支的
   //       “ftruncate 到 EOF” 即 no-op）
5. return true;
```

**语义等价性论证（逐条）**

| # | `FileLogStore` 行为（行号） | `LsmLogStore` 行为 | 等价性 |
|---|---|---|---|
| E1 | L360 `kNoIndex` ⇒ `true`（no-op） | 同 | ✅ |
| E2 | L361 `<= lastIncluded_` ⇒ `false` | 同（内存判据） | ✅ 同一个错误码/返回值 |
| E3 | L362 `> lastIndex()+1` ⇒ `false` | 同（内存判据） | ✅ |
| E4 | L366-375 `fromIndex > lastIndex()` ⇒ `off = EOF`，`ftruncate` 到 EOF = no-op | 无 batch，直接 `return true` | ✅ 可观察状态相同 |
| E5 | L376 `ftruncate(off)` 丢弃 `[fromIndex, lastIndex]` | `Delete(EntryKey(i))` for `i ∈ [fromIndex, lastIndex]` ⇒ 用户视图里这些索引**立即消失**（新 tombstone 的 sequence 最大，`InternalKeyComparator` 的 trailer 降序让最新版本排最前，`db.h` L51「每个 key 只出最新可见版本、跳过 tombstone」） | ✅ |
| E6 | L380 `flush && !flushFd(logFd_)` ⇒ false（含 fsync 变体） | `Write({sync=true})` ⇒ lsm 返回前已 durable | ✅ 「返回 true ⟹ 截断已 durable」 |
| E7 | L353-355 `truncateSuffixNoSync` = `truncateSuffixLocked(from, /*flush=*/false)` | `Write({sync=false})` | ✅ 「不 fsync，由同批的 `sync()` 覆盖」 |
| E8 | L382-389 `entries_.resize(...)` + 清理 `offsetOf_` | `terms_.resize(...)` + 更新 `lastIndex_/lastTerm_` | ✅ 内存态一致 |
| E9 | **L384-388 的「offset 缺失即 false」防御**：绝不把缺失 offset 当 0（否则会 ftruncate 掉整个文件） | **不需要**：本实现按**索引区间**生成 Delete，不存在「offset 查询失败」这一失效模式 | ✅（本实现少一类故障） |
| E10 | L370-371 `if (it == offsetOf_.end()) return false;` | — | 同上 |

**崩溃窗口分析（这是 `truncateSuffixNoSync` 的全部风险所在）**

`log_store.h` L47 逐字要求：「调用方必须在同一批里随后 `sync()`；否则崩溃后变短的日志可能复活。」

| 事件序列 | 崩溃点 | `FileLogStore` 重启后 | `LsmLogStore` 重启后 | 是否等价 |
|---|---|---|---|---|
| `truncateSuffixNoSync(k)` → `appendNoSync(新条目)` → `sync()` | 任意点 | 若崩溃在 `sync()` 之前：`ftruncate` 是**元数据变更**，可能已落盘也可能没有；`write` 的字节同理 | `Delete(k..)` 与 `Put(k..)` 是两条 WAL record（两次 `DB::Write`），都可能未 durable | ✅ **两者都允许「旧后缀复活」**，差别只是粒度：`FileLogStore` 以 ftruncate 为单位，lsm 以 WAL record 为单位 |
| 同上，`sync()` 成功返回后崩溃 | 之后 | `fsync(fd)` 覆盖 ftruncate 造成的 size 变更与全部字节 ⇒ 截断 + 新条目都 durable | `DB::Sync()` 的 `log_->Sync()` 覆盖两条 record 的字节 ⇒ 同 | ✅ |
| `truncateSuffix(k)`（含 fsync） | 返回后 | durable | durable | ✅ |
| 崩溃发生在 `Delete` 批次**部分**可见 | — | 不可能：`ftruncate` 是单次原子 syscall | 不可能：一条 WAL record = 一个原子批（I15），恢复时要么全重放要么全截断 | ✅（**本实现更强**：不会出现「截断了一半」） |

> ⚠️ **必须写进 §6 的一条真实差异**：`FileLogStore` 的 `truncateSuffix` 是 **O(1)**（一次 `ftruncate`），本实现是 **O(被截断条目数)** 次 `Delete`（写放大 = 每条 9B key + 编码 + WAL 帧）。raft 的典型冲突后缀很短（1~几条），但一个长时间分区的 follower 可能有很长的未提交后缀。**需要实测**（§6 R3）。

#### 2.4.7 `std::vector<LogEntry> slice(Index from, size_t maxEntries, size_t maxBytes) const`

```
std::lock_guard<std::mutex> lk(mu_);
std::vector<LogEntry> out; size_t bytes = 0;
Index start = from; if (start < firstIndex()) start = firstIndex();     // L399 clamp（D3）
if (start > lastIndex_) return out;
std::unique_ptr<lsm::Iterator> it(db_->NewIterator());                  // 创建时刻的一致快照
it->Seek(EntryKey(start));
while (it->Valid()) {
  if (out.size() >= maxEntries) break;                                  // L402
  // key 必须仍在日志命名空间内
  if (key 不在 [kLogBegin, kLogEnd) 内) break;
  LogEntry e; DecodeEntryKey(it->key(), &idx); DecodeEntry(it->value(), &e);
  if (e.index < start) { it->Next(); continue; }                        // L401
  const size_t sz = e.key.size() + e.value.size();                      // L403（**不是编码字节**）
  if (!out.empty() && bytes + sz > maxBytes) break;                     // L404
  out.push_back(std::move(e)); bytes += sz;
  it->Next();
}
return out;
```

| 语义 | 等价性论证 |
|---|---|
| `from < firstIndex()` 被 clamp | 与 L399 相同 |
| `maxEntries` 上限 | 与 L402 相同（`>=` 判定放在 `push_back` 之前 ⇒ 最多 `maxEntries` 条） |
| `maxBytes` 的**怪癖**：只要有可用条目就至少返回 1 条 | 与 L404 `if (!out.empty() && bytes + sz > maxBytes) break;` **逐字相同**（`MemoryLogStore` L72 的写法 `bytes + sz > maxBytes && !out.empty()` 等价） |
| 计量口径 = `key.size() + value.size()` | 与 L403 **逐字相同**（不是 `encodeEntry` 的字节数） |
| **一致性快照** | `slice()` 持 `mu_`，而所有变更（append/truncate/compact）也都持 `mu_`；`NewIterator()` 又在创建时取 `last_sequence_`（`db_impl.cpp` L680-686）⇒ 迭代器看到的状态 == 内存态 `lastIndex_/terms_` |
| 终止 | 必须靠 `index > lastIndex_` 或离开 `< kLogEnd` 终止，**不能**靠「迭代器耗尽」——DB 里可能有已 compact 掉但 tombstone 尚在、或边界之外的残留 |
| 空/越界 | `start > lastIndex_` ⇒ 空（与 L400-408 遍历空 `entries_` 同） |

> ⚠️ **这是本设计的第二号性能风险**：每次 `slice()` 都要创建并析构一个 `lsm::Iterator`（`db_impl.cpp` L694-720：ref memtable + immutables + `Version`，并对每一层建 iterator）。心跳路径 `buildPeerJobLocked()` 会调 `slice(next, 1, SIZE_MAX)`（`raft_node.cpp` L250）—— 一个只取 1 条的热路径。见 §6 R2 与 §7 W5。

#### 2.4.8 只读访问器

```
Index lastIndex() const   { lock; return lastIndex_; }
Term  lastTerm()  const   { lock; return lastTerm_; }        // 空 ⇒ lastIncludedTerm_（L420）
Term  termAt(Index i) const {
  lock;
  if (i == kNoIndex) return kNoTerm;                          // L425
  if (i == lastIncluded_ && lastIncluded_ != kNoIndex) return lastIncludedTerm_;  // L426-428 边界条目
  if (i < firstIndex() || i > lastIndex_) return kNoTerm;     // L429
  return terms_[static_cast<size_t>(i - firstIndex())];       // L430
}
Index firstIndex() const { lock; return lastIncluded_ + 1; }  // L514 / L64
Index lastIncludedIndex() const { lock; return lastIncluded_; }
Term  lastIncludedTerm()  const { lock; return lastIncludedTerm_; }
```

与 `file_log_store.cpp` L411-431、L513-518 与 `memory_adapters.cpp` L79-96 **逐条同构**（含 L425 的 `kNoIndex` 特判、L426-428 的边界条目特判、L429 的范围判定）。**注意**：`lastIndex()`/`lastTerm()` 在「日志为空」时返回 `lastIncluded_`/`lastIncludedTerm_`（L413-414/L420 的 `entries_.empty()` 分支），术语向量必须显式维护这一点，**不能**用 `terms_.empty()` 直接当「日志为空」——当 `lastIncluded_ > 0` 且日志为空时两者含义不同。

#### 2.4.9 `void setBoundary(Index, Term)` 与 `bool compact(Index, Term)`

**`setBoundary`** —— 与 L435-449 **逐字同构**（**纯内存，不做任何 IO**）：

```
lock;
lastIncluded_ = lastIncludedIndex; lastIncludedTerm_ = lastIncludedTerm;
size_t drop = 0; while (drop < terms_.size() && (firstIndex() + drop) <= lastIncludedIndex) ++drop;
terms_.erase(terms_.begin(), terms_.begin() + drop);
```

> 调用时机是**启动时、`load()` 之前**（`raft_node.cpp` L87）。此时 `terms_` 是空的，真正的效果只是设置边界 —— 与 `FileLogStore` 完全相同。

**`compact(upTo, termAtUpTo)`**：

```
std::lock_guard<std::mutex> lk(mu_);
if (upTo == kNoIndex) return true;             // L456
if (upTo <= lastIncluded_) return true;        // L457 already compacted: no-op
// upTo 可以超过 lastIndex_（InstallSnapshot）⇒ 保留后缀为空（L458 注释）
Index hi = std::min(upTo, lastIndex_);
if (hi >= firstIndex()) {
  lsm::WriteBatch wb;
  for (Index i = firstIndex(); i <= hi; ++i) wb.Delete(EntryKey(i));
  if (!db_->Write({.sync = true}, &wb).ok()) { poisoned_ = true; return false; }
}
lastIncluded_ = upTo; lastIncludedTerm_ = termAtUpTo;
// 丢弃 terms_ 中 ≤ upTo 的前缀；若 upTo > lastIndex_ 则日志清空
return true;
```

| 语义 | 等价性论证 |
|---|---|
| `upTo == kNoIndex` / `upTo <= lastIncluded_` 的 no-op | 与 L456-457 逐字相同 |
| `upTo > lastIndex_` 合法（InstallSnapshot） | 与 L458 注释 + L460-463（`keep` 为空）相同：本实现 `hi = lastIndex_`，删除全部，边界推进到 `upTo` |
| **返回 true ⟹ durable** | L485 `fsync(fd)` + L491 `rename` + L495 `fsyncDir` + L499-504 重开 fd；本实现 `Write({sync=true})` ⇒ 返回前 Tombstone 记录已 fsync。二者承诺相同 |
| **崩溃原子性** | `FileLogStore` 用 tmp+rename 保证「要么旧文件要么新文件」；本实现用「一条 WAL record = 一个原子批」（I15）保证「要么全删要么没删」。**两者都满足 D3/B12 的「只有 compact 成功才推进边界」**（`raft_node.cpp` L1013-1018 的 `throw`） |
| **失败语义** | L500-503「文件已被替换但打不开 ⇒ 致命失败，不是『压缩跳过』」；本实现写入失败 ⇒ `return false` + `poisoned_`，`RaftNode` 抛 `"raft: log compaction failed"`。承诺相同 |
| **与 `snapshotOpMu_` 的关系** | `raft_node.cpp` L993-996 用 `snapshotOpMu_` 串行化 `maybeSnapshot` 与 `onInstallSnapshot` 的 compact，本实现**不需要** `flushMu_` 级别的叶子锁（因为不换 fd、不 rename），但仍受 `RaftNode` 的 `snapshotOpMu_` 保护（未改） |
| ⚠️ **不成立的一条** | `FileLogStore::compact` 之后 **`raft.log` 的物理字节立即缩小**；本实现只写 tombstone，**物理空间要等 lsm 后台 compaction 才回收**。这直接冲击 `scripts/raft_snapshot_fault.sh` 的 `LOG_BOUND`（L23/L116-119）⇒ 见 C6 与 §6 R4 |

**`compact` 与 `load` 的配合**（D3 的完整闭环）：`compact` 只写 tombstone，`load()` 依赖用户视图跳过 tombstone ⇒ 「已被压缩掉的前缀」在重启后**确实不可见**。这条依赖必须由 M6.2 的一条定向用例证明（§4-M6.2）。

### 2.5 `LsmLogStore` 的完整接口草案（签名级，不含实现）

```cpp
// src/raft/lsm_log_store.h
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "raft/log_store.h"

namespace lsm { class DB; }

namespace raftkv::raft {

// lsm 引擎承载的 raft 日志 + term/votedFor。
// 目录：<dir>/raft-lsm（与 FileLogStore 的 <dir>/raft 物理隔离）。
// 键空间：0x01 || BE64(index)  = 日志条目；0x02 "meta" = term/votedFor。
class LsmLogStore : public LogStore {
 public:
  explicit LsmLogStore(std::string dir);          // Open 失败 ⇒ throw std::runtime_error
  ~LsmLogStore() override;                        // Close + 释放 LOCK

  bool load(Term& term, int& votedFor, Index& lastIndex) override;
  bool persistMeta(Term term, int votedFor) override;
  bool append(const std::vector<LogEntry>& entries) override;
  bool appendNoSync(const std::vector<LogEntry>& entries) override;
  bool sync() override;
  bool truncateSuffix(Index fromIndex) override;
  bool truncateSuffixNoSync(Index fromIndex) override;   // ★ 必须覆写（log_store.h L48-49）
  std::vector<LogEntry> slice(Index from, size_t maxEntries, size_t maxBytes) const override;
  Index lastIndex() const override;
  Term lastTerm() const override;
  Term termAt(Index index) const override;
  void setBoundary(Index lastIncludedIndex, Term lastIncludedTerm) override;
  bool compact(Index upTo, Term termAtUpTo) override;
  Index firstIndex() const override;
  Index lastIncludedIndex() const override;
  Term lastIncludedTerm() const override;

  // 仅测试/诊断（与 MemoryLogStore::all() 同形的 seam）
  std::vector<LogEntry> all() const;
  // 诊断：口径与 lsm 的 RecoveryStats 对齐（尾部截断字节数等）
  struct Stats { uint64_t loads = 0, appends = 0, syncs = 0, truncates = 0,
                 compacts = 0, meta_persists = 0, log_dir_bytes = 0;
                 uint64_t wal_tail_truncated_bytes = 0; };
  Stats stats() const;
 private:
  bool truncateLocked(Index fromIndex, bool flush);   // 调用方持 mu_
  Term termAtLocked(Index index) const;               // 调用方持 mu_
  std::string dir_;
  std::unique_ptr<lsm::DB> db_;
  mutable std::mutex mu_;
  Term term_ = kNoTerm; int votedFor_ = -1;
  Index lastIncluded_ = kNoIndex; Term lastIncludedTerm_ = kNoTerm;
  std::vector<Term> terms_;
  Index lastIndex_ = kNoIndex; Term lastTerm_ = kNoTerm;
  bool poisoned_ = false;
};

}  // namespace raftkv::raft
```

> `Stats` 的存在理由是 `docs/m3-design.md`/`m2-design.md` §8.4 的**计数纪律**：「任何丢弃/跳过/截断/删除都必须有一个计数落点」。`wal_tail_truncated_bytes` 直接对齐 lsm 的 `RecoveryStats::tail_truncated_bytes`（`db_impl.h` L42）。

---

## §3 崩溃与持久化

### 3.1 操作 × 持久化承诺矩阵

| 操作 | 是否 fsync | 返回 true 的含义 | 崩溃（`kill -9`）后 | 崩溃（掉电）后 |
|---|---|---|---|---|
| `persistMeta(t, v)` | ✔（`Write{sync=true}`） | `(t, v)` 已 durable | 必存活 | 必存活（除介质疑失） |
| `append(es)` | ✔ | `es` 已 durable | 必存活 | 必存活 |
| `appendNoSync(es)` | ✘（`Write{sync=false}`） | 已交给内核且**已对 `slice/lastIndex/termAt` 可见** | 本 VM 实测必存活（page cache） | 可能丢最近的**连续后缀**；**不得**出现半条 / 乱序 / 旧值覆盖新值（I19 + §7.3） |
| `sync()` | ✔ | 此前全部已返回 kOk 的写都已 durable | 全部存活 | 全部存活 |
| `truncateSuffix(i)` | ✔ | 截断已 durable | 必存活 | 必存活 |
| `truncateSuffixNoSync(i)` | ✘ | 截断**已可见**、未 durable；**调用方必须在同一批随后 `sync()`** | 可能复活被截断的后缀（与 file 引擎相同） | 同左 |
| `compact(i, t)` | ✔ | 前缀删除已 durable，边界可安全推进 | 必存活 | 必存活 |

### 3.2 崩溃后允许丢失什么

**允许**：任何尚未被某次 `sync()` 覆盖的**连续后缀** —— 即 `appendNoSync` 写入但未 flush 的条目；以及 `truncateSuffixNoSync` 的删除（导致被截断的后缀「复活」）。

**不允许**（lsm 结构性保证，均可引用）：
- 半条 WAL record（`m2-design.md` I14 L100：「不完整则截断到最后一条完整 record，被截断部分不得对查询可见」）。
- 批内部分生效（I15 L101：一条 record = 一个原子批）。
- 撕裂的 value（I19 L104：任一 key 只能是某个完整版本）。
- 乱序生效（I13 L98：重放顺序 = 写入顺序 = sequence 升序）。

**为什么「允许丢一段连续后缀」对 Raft 是安全的**：该后缀必然是**未提交**的（raft-kv 只在 `sync()` 覆盖之后才推进 `syncedIndex_` 并 ack —— `raft_node.cpp` L583-593、L603-607、L820-828，契约 I5/I11）。未提交的后缀在 raft 里是可被 leader 覆盖的，恢复后由日志复制重新补齐。

### 3.3 与 lsm 的 WAL / SSTable 恢复如何配合

```
重启路径（LsmLogStore::load 之前由 ctor 完成）：
  LsmLogStore ctor  →  lsm::DB::Open(opts, "<dir>/raft-lsm", &db_)
                          │
                          ├─ ① 取 LOCK（D10）失败 ⇒ Open 失败 ⇒ ctor throw
                          ├─ ② 扫描 *.log：尾部残骸 ⇒ ftruncate 到最后完整 record，
                          │      并把字节数记入 RecoveryStats::tail_truncated_bytes（L42）
                          │   中间损坏（其后仍有完好 record）⇒ kCorruption ⇒ Open 失败 ⇒ ctor throw
                          ├─ ③ 读 CURRENT/MANIFEST 重建 Version（SSTable 列表）
                          ├─ ④ 第二遍重放 WAL 到 MemTable
                          └─ ⑤ 孤儿 *.sst / *.tmp 清理（RecoveryStats 各有计数）
  LsmLogStore::load()  →  Get(kMetaKey) + 按索引顺序全扫描 [kLogBegin, kLogEnd)
                          └─ 连续性/边界校验（§2.4.1 的 R1-R6 表）
  RaftNode ctor        →  快照 load → sm.restore → log_.setBoundary → log_.load   （未改）
```

**对齐点**：
1. `FileLogStore::load()` 的 `ftruncate`（L238-239）在 lsm 侧等价于 `DB::Open` 内部的尾部截断。**本实现必须把 `tail_truncated_bytes` 暴露出来**（§2.5 `Stats`），作为「撕裂尾确实被处理了」的可观测证据 —— 否则「lsm 版没有截断逻辑」会被误读成「lsm 版不处理撕裂」。
2. `FileLogStore` 的「前缀丢失 ⇒ 拒绝启动」（L223-228）在 lsm 侧**必须原样保留**（§2.4.1 R4）。这是 D2「compaction 只在 durable snapshot 之后」的守门人。
3. 「中间损坏 ⇒ 拒绝启动」在 lsm 侧由 `DB::Open` 负责（`m2-design.md` §5.3 L613），比 `FileLogStore` 的 L229-231「当撕裂尾 break」**更严格**。`RaftNode` 对二者的反应都是 `throw "raft: log load failed"`（L101-103），**可观察行为一致**。

### 3.4 失败传播

| 层 | 失败 | 行为 |
|---|---|---|
| lsm WAL `Append`/`Sync` 失败 | 短写 / fsync 失败 | **粘性 `bg_error_` + fail-stop**（D11，`db_impl.cpp` L175 / L455）⇒ 后续所有 `DB::Write` 立即返回同一错误 |
| `LsmLogStore` | 任一 `DB::Write`/`DB::Sync` 返回非 ok | `poisoned_ = true`，后续变更方法一律 `return false`（**绝不在失败后继续用内存态撒谎**） |
| `RaftNode` | `appendNoSync` 返回 false | `onAppendEntries` 回 `{false}`（不 ack，L568-570）/ `appendEntryLocked` 失败 ⇒ `kErr`（L734-736） |
| `RaftNode` | `sync()` 返回 false | 绝不记 durable、绝不 ack（L600-602、L829、L859-861） |
| `RaftNode` | `compact()` 返回 false | `throw std::runtime_error("raft: log compaction failed")`（L1016-1018 / L1106-1108） |

> **与 `FileLogStore` 的差异**：`FileLogStore` 在写失败后**没有**粘性毒化（它只是返回 false，下一次调用可能又成功）；`LsmLogStore` 的毒化是**更强**的保证，且与 lsm 的 fail-stop 天然一致。这不改变 raft 的安全性，只是让「坏盘之后会不会时好时坏」的答案更确定。

---

## §4 子里程碑拆分（每步一个提交）

**通用约定**

- 工作目录：`~/raft-kv-lsm`（VM）。
- 构建：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"`
- 单测：`./build/bin/raftkv_raft_tests`
- **每一步只实现「让当前判据通过」的最小代码。**
- 每一步**结束即提交**（`git add` 具体文件 + 一条 message）；**M6 不 push**。

---

### M6.0 —— 现状探测 + 本设计文档（本次交付）

**Files**：`docs/m6-design.md`（新建）

**判据**
- 文件存在，且 §0 引用的每个「文件名 + 行号」都能用下列命令复现。
- 全文没有未标注的推测（未验证项必须显式写「未验证」）。

**证据命令（可直接粘贴）**

```bash
cd ~/raft-kv-lsm
wc -l docs/m6-design.md
sed -n '23p;26p;29p;37p;38p;42p;50,52p;54p;57,59p;62,66p' src/raft/log_store.h
sed -n '30,34p;105,118p;219,239p;359,363p;393,409p;435,449p;451,463p;513,518p' src/raft/file_log_store.cpp
sed -n '86,87p;94,106p;112p;1013,1018p' src/raft/raft_node.cpp
sed -n '154,178p;409,443p;680,687p;800,824p' ~/lsm-kv/src/db_impl.cpp
sed -n '46,78p' ~/lsm-kv/CMakeLists.txt
grep -n 'LOG_BOUND' scripts/raft_snapshot_fault.sh
```
期望：输出与 §0/§2 中摘录的行一致。

**提交**：`git add docs/m6-design.md && git commit -m "docs(m6): M6.0 设计起草 —— LogStore→lsm 接口映射 + 子里程碑 + A/B 与负结果口径"`

---

### M6.1 —— 构建接线 + 引擎开关（**零行为变更**）

**Files**
- 新建：`src/raft/lsm_log_store.h`、`src/raft/lsm_log_store.cpp`（**全部方法返回 false / 抛 `std::logic_error("not implemented")`**，只有 ctor/dtor 与 key 编码助手是完整的）
- 改：`CMakeLists.txt`（把 `lsm_log_store.cpp` 加进 `raftkv_raft`；新增 `RAFTKV_LSM_DIR` cache 变量，默认 `$ENV{HOME}/lsm-kv`；存在则 `add_subdirectory` 并链 `lsm`，否则 `raftkv_raft` 不定义 `RAFTKV_HAVE_LSM`）
- 改：`src/main_raft_node.cpp`（`--log-engine=file|lsm` + `RAFTKV_LOG_ENGINE`，**默认 file**；`--help` 增补一行）

**判据**
- 默认路径（file）**逐字节不变**：`raftkv_raft_tests` 全绿。
- `--log-engine=lsm` 在**未**编译进 lsm 时报错退出（退出码非 0，stderr 明确），**绝不静默降级到 file**。
- `--log-engine=lsm` 在编译进 lsm 后能启动（进入 ctor），随后在 `load()` 阶段以明确错误退出。

**证据命令**

```bash
cd ~/raft-kv-lsm && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAFTKV_LSM_DIR="$HOME/lsm-kv" \
  && cmake --build build -j"$(nproc)"
./build/bin/raftkv_raft_tests                                   # 期望：[  PASSED  ] 全部
./build/bin/raftkv_raft_node --id 1 --port 19991 --peers '1=127.0.0.1:19991' \
  --data-dir /tmp/m6-smoke --log-engine=lsm; echo "exit=$?"     # 期望：exit != 0 且 stderr 有明确原因
```

**提交**：`git add CMakeLists.txt src/raft/lsm_log_store.h src/raft/lsm_log_store.cpp src/main_raft_node.cpp && git commit -m "feat(m6.1): LsmLogStore 骨架 + --log-engine 开关（默认 file，零行为变更）"`

---

### M6.2 —— `LsmLogStore` 语义单测（**不接 Raft**）

**Files**
- 新建：`tests/raft_lsm_log_test.cpp`
- 改：`CMakeLists.txt`（加入 `raftkv_raft_tests` 源列表）

**Tests（每条一条断言即可，镜像 `FileLogStore` 的既有断言）**

| 用例 | 判据（对应 §2 的哪条论证） |
|---|---|
| `LsmLogStore.KeyEncodingIsBytewiseIndexOrdered` | `EntryKey(i)` 字节序 == 索引序（§2.1） |
| `LsmLogStore.MetaKeyIsOutsideLogRange` | `kMetaKey` 不在 `[kLogBegin, kLogEnd)` 内（§2.1） |
| `LsmLogStore.RestartRestoresMetaAndLog` | 与 `FileLogStore.RestartRestoresMetaAndLog`（`tests/raft_restart_test.cpp` L41-65）**同一组断言** |
| `LsmLogStore.AppendIsDurableAndVisible` | `append()` 后立刻 `lastIndex/lastTerm/termAt/slice` 正确（§2.4.3） |
| `LsmLogStore.AppendNoSyncIsVisibleButSurvivesSync` | `appendNoSync` 后可见；`sync()` 后重建 store 仍可读（§2.4.4/L32-33） |
| `LsmLogStore.IdempotentAppendSameIndexSameTerm` | 同 index 同 term 重复 append 不报错、不重复（§2.4.4） |
| `LsmLogStore.ConflictTruncateThenAppend` | 同 index 异 term ⇒ 覆盖，且**重启后**是新值（§2.4.4/E5） |
| `LsmLogStore.TruncateSuffixBoundaries` | `kNoIndex` ⇒ true；`<= lastIncluded_` ⇒ false；`> lastIndex_+1` ⇒ false；`== lastIndex_+1` ⇒ true 且无变化（§2.4.6 E1-E4） |
| `LsmLogStore.TruncateNoSyncThenSyncIsDurable` | `truncateSuffixNoSync` + `sync()` 后重建 ⇒ 后缀不复活（§2.4.6） |
| `LsmLogStore.TruncateThenCrashResurrectsSuffix` | **文档化**的反面：只做 `truncateSuffixNoSync` 不 `sync()` 就析构 ⇒ 后缀复活（证明该窗口真实存在，且与 `log_store.h` L47 的警告一致） |
| `LsmLogStore.SliceClampsAndHonoursLimits` | `from < firstIndex()` clamp；`maxEntries`；**`maxBytes` 至少返回 1 条**（§2.4.7/L404） |
| `LsmLogStore.CompactDropsPrefixAndSurvivesRestart` | `setBoundary(I)+compact(I)` 后重建 ⇒ `firstIndex()==I+1`、`slice(I+1,…)` 正确（§2.4.9/§2.4.1 R2） |
| `LsmLogStore.PrefixGoneWithoutSnapshotRefusesLoad` | 不调 `setBoundary` 就 `load()` ⇒ **false**（§2.4.1 R4，对应 L223-228） |
| `LsmLogStore.LoadSkipsEntriesBelowBoundary` | 保留 ≤I 的残留 + `setBoundary(I)` ⇒ `load()` 成功且首条 == I+1（§2.4.1 R3，对应 L219-222） |
| `LsmLogStore.EmptyLogWithBoundary` | 空日志 + 边界 I ⇒ `lastIndex()==I`、`lastTerm()==T`、`firstIndex()==I+1`（§2.4.1 R5） |
| `LsmLogStore.MetaPersistIsAtomicAndDurable` | `persistMeta` 后重建 ⇒ term/votedFor 一致（§2.4.2） |
| `LsmLogStore.TwoStoresOnSameDirConflict` | 第二个 `LsmLogStore(dir)` 构造抛异常（D10/LOCK） |

**判据**：以上全部 PASS；且 `FileLogStore` 与 `MemoryLogStore` 的既有用例**一条都没改**。

**证据命令**

```bash
cd ~/raft-kv-lsm && cmake --build build -j"$(nproc)" \
  && ./build/bin/raftkv_raft_tests --gtest_filter='LsmLogStore.*' \
  && ./build/bin/raftkv_raft_tests --gtest_filter='FileLogStore.*'
```
期望：两组都 `[  PASSED  ]`。

**提交**：`git add tests/raft_lsm_log_test.cpp CMakeLists.txt && git commit -m "test(m6.2): LsmLogStore 语义单测（17 条，逐条对齐 FileLogStore 行为）"`

---

### M6.3 —— `raft_restart_test` 参数化（磁盘用例跑两个引擎）

**Files**：改 `tests/raft_restart_test.cpp`（引入 `StoreFactory`；既有 `FileLogStore.*` 用例名与断言**不变**，新增 `LsmLogStore.*` 孪生）

**判据**：`FileLogStore.*` 与 `LsmLogStore.*` 都 PASS，且两个 group 的断言文本**逐字相同**。

**证据命令**

```bash
cd ~/raft-kv-lsm && cmake --build build -j"$(nproc)" \
  && ./build/bin/raftkv_raft_tests --gtest_filter='*RestartRestoresMetaAndLog:*TruncatesTornTail'
```
期望：4 个用例（2 引擎 × 2 场景）全 PASS。⚠️ `TruncatesTornTail` 在 lsm 引擎下**没有直接的等价注入点**（不能往 lsm 的 WAL 尾部追加垃圾字节而不被 `ResyncScan` 判成中间损坏）—— 该用例**只保留 file 引擎版本**，lsm 侧的等价证据改为「构造 `tail_truncated_bytes > 0` 的场景，或在 M6.6 用 `kill -9` 真实注入」。**若无法构造，如实标注「未覆盖」并写进 §7。**

**提交**：`git add tests/raft_restart_test.cpp && git commit -m "test(m6.3): raft_restart_test 参数化（file/lsm 双引擎同断言）"`

---

### M6.4 —— `RaftSnapshotDisk.*` / `RaftMembershipDisk.*` 参数化

**Files**：改 `tests/raft_snapshot_test.cpp`、`tests/raft_membership_test.cpp`（Disk fixture 抽出「建 store」的工厂）

**判据**：8 个 Disk 用例（`raft_snapshot_test.cpp` L327/L364/L551/L580/L866/L926、`raft_membership_test.cpp` L1526/L1552/L1588/L1617）× 2 引擎 = 16 条全绿。
⚠️ `RaftSnapshotDisk.TornTailAfterCompact`（L551）与 `FileStoreClusterKeepsCommittingAfterCompaction`（L866，名字里就带 `FileStore`）的语义在 lsm 侧需要重新表述（一个注入「归档尾部撕裂」、一个断言「compact 后仍能提交」）—— 前者可能**不适用**，如实标注。

**证据命令**

```bash
cd ~/raft-kv-lsm && cmake --build build -j"$(nproc)" \
  && ./build/bin/raftkv_raft_tests --gtest_filter='*Disk*'
./build/bin/raftkv_raft_tests        # 全量，期望全绿（M2-M5 门禁不退化）
```

**提交**：`git add tests/raft_snapshot_test.cpp tests/raft_membership_test.cpp && git commit -m "test(m6.4): Disk 系列 fixture 参数化（file/lsm）"`

---

### M6.5 —— 端到端脚本支持双引擎

**Files**：改 `scripts/raft_e2e.sh`、`scripts/raft_snapshot_e2e.sh`、`scripts/raft_membership_e2e.sh`（`start_node()` 透传 `RAFTKV_LOG_ENGINE`）

**判据**：三个脚本在 `RAFTKV_LOG_ENGINE=file`（默认）与 `=lsm` 下都打印各自的 PASS 行；`verify` 全部 `missing 0`。

**证据命令**

```bash
cd ~/raft-kv-lsm && cmake --build build -j"$(nproc)"
for eng in file lsm; do
  echo "=== engine=$eng ==="
  RAFTKV_LOG_ENGINE=$eng bash scripts/raft_e2e.sh
  RAFTKV_LOG_ENGINE=$eng bash scripts/raft_snapshot_e2e.sh
  RAFTKV_LOG_ENGINE=$eng bash scripts/raft_membership_e2e.sh
done
```
期望：6 条 PASS。

**提交**：`git add scripts/raft_e2e.sh scripts/raft_snapshot_e2e.sh scripts/raft_membership_e2e.sh && git commit -m "test(m6.5): e2e 脚本支持 RAFTKV_LOG_ENGINE"`

---

### M6.6 —— 故障注入 + `LOG_BOUND` 口径修正

**Files**：改 `scripts/raft_fault.sh`、`scripts/raft_snapshot_fault.sh`（**C6**：`LOG_BOUND` 按引擎取目录字节）、`scripts/raft_membership_fault.sh`

**判据**
- `RAFTKV_LOG_ENGINE=lsm` 下三个 fault 脚本各 PASS（默认 `--repeat 50`）。
- `LOG_BOUND` 在 lsm 引擎下改为「`<data-dir>/raft-lsm` 目录总字节 ≤ 阈值」，且**该阈值必须由实测标定**并写进 `docs/m6-bench.md`（file 引擎的 1 MiB 不能直接照搬 —— lsm 的 WAL + SST + MANIFEST 是多个文件）。

**证据命令**

```bash
cd ~/raft-kv-lsm && cmake --build build -j"$(nproc)"
RAFTKV_LOG_ENGINE=lsm bash scripts/raft_fault.sh --repeat 50
RAFTKV_LOG_ENGINE=lsm bash scripts/raft_snapshot_fault.sh --repeat 50
RAFTKV_LOG_ENGINE=lsm bash scripts/raft_membership_fault.sh --repeat 50
# 对照：file 引擎不退化
RAFTKV_LOG_ENGINE=file bash scripts/raft_fault.sh --repeat 50
```

**提交**：`git add scripts/raft_fault.sh scripts/raft_snapshot_fault.sh scripts/raft_membership_fault.sh && git commit -m "test(m6.6): 故障注入支持 lsm 引擎 + LOG_BOUND 按引擎取口径"`

---

### M6.7 —— A/B 基准（§5）

**Files**：新建 `scripts/bench_m6_ab.sh`；新建 `docs/m6-bench.md`（原始行 + 结论 + 负结果）

**判据**：脚本跑完输出 §5.3 的固定格式表；**每个格子 `verify` 含 `missing 0`**；基线臂（`~/raft-kv`）全部 PASS。

**证据命令**

```bash
cd ~/raft-kv-lsm && bash scripts/bench_m6_ab.sh --quick        # 冒烟
bash scripts/bench_m6_ab.sh --repeats 3                        # 正式
```

**提交**：`git add scripts/bench_m6_ab.sh docs/m6-bench.md && git commit -m "bench(m6.7): A/B（基线 ~/raft-kv vs file vs lsm）同轮交替 + 负结果入档"`

---

### M6.8 —— 文档收尾

**Files**：改 `README.md`、`docs/roadmap.md`；新建 `docs/m6-review.md`（可选，评审产出）

**判据**：README 说明 `--log-engine` 与 lsm 的前置条件；roadmap 的 M6 一节指向 `docs/m6-design.md` / `docs/m6-bench.md`。

**提交**：`git add README.md docs/roadmap.md && git commit -m "docs(m6.8): README/roadmap 增补 M6"`

---

## §5 A/B 验证方案

### 5.1 三个臂

| 臂 | 二进制 | 日志引擎 | 作用 |
|---|---|---|---|
| `base` | `~/raft-kv/build/bin/raftkv_raft_node` | `FileLogStore` | **基线**（远端 `1463620`，一个字节都不改） |
| `file` | `~/raft-kv-lsm/build/bin/raftkv_raft_node` | `FileLogStore`（默认） | **无回归对照**：证明 M6 的改动没有意外影响原有引擎 |
| `lsm` | `~/raft-kv-lsm/build/bin/raftkv_raft_node --log-engine=lsm` | `LsmLogStore` | **被测量对象** |

> `base` vs `file` 这一对是本方案的关键设计：它把「M6 引入的回归」与「LSM 引擎本身的差异」分离开。缺了它，`lsm` 的劣化无法归因。

### 5.2 硬门禁 vs 观测（这是 §5 的核心分类）

**硬门禁（H）—— 必须通过；任一失败即 M6 不达标**

| # | 门禁 | 判据字符串 / 断言 |
|---|---|---|
| H1 | `./build/bin/raftkv_raft_tests`（`raftkv_raft` CTest） | `[  PASSED  ]` 且 0 FAILED |
| H2 | `scripts/raft_e2e.sh` | `raft_e2e: PASS` |
| H3 | `scripts/raft_snapshot_e2e.sh` | `raft_snapshot_e2e: PASS` |
| H4 | `scripts/raft_membership_e2e.sh` | 退出码 0（脚本内所有 `FAIL` 分支未触发） |
| H5 | `scripts/raft_fault.sh --repeat 50` | `raft_fault: PASS (50 iterations)` |
| H6 | `scripts/raft_snapshot_fault.sh --repeat 50` | `raft_snapshot_fault: PASS` |
| H7 | `scripts/raft_membership_fault.sh --repeat 50` | 退出码 0 |
| H8 | **`verify` 的 `missing 0`** | 每次 `fill` 之后的 `verify` 输出必须含 `missing 0`（`bench_m5_ab.sh` L100 的既有口径） |
| H9 | **`base` 臂不退化** | `base` 臂在**同一套脚本**下必须全部通过；H2-H7 的判据字符串在 `base`/`file`/`lsm` 三个臂上**逐字相同** |
| H10 | 无多数派时写必须不返回 OK | 已内嵌于 H2/H5/H6（`raft_e2e.sh` L121-125、`raft_fault.sh` L124-128、`raft_snapshot_e2e.sh` L142） |
| H11 | ASan 构建下 H1 无报告 | `cmake -B build-asan -DENABLE_ASAN=ON` 后跑 H1 |
| H12 | TSan 构建下 `LsmLogStore` 相关用例无报告 | `cmake -B build-tsan -DENABLE_TSAN=ON`；TSan 需 `setarch $(uname -m) -R ./bin/...`（`CMakeLists.txt` L14） |

**观测（O）—— 只记录，不判定成败**

| # | 观测项 | 单位 | 采集方式 |
|---|---|---|---|
| O1 | 吞吐 / ms-per-write / P99 | qps / ms / ms | `bench_m6_ab.sh` 墙钟；P99 需要 node 侧新增直方图或外部采样（**未定**，见 §8 D12） |
| O2 | 日志目录字节数 | bytes | `du -sb <data-dir>/raft`（file）/ `du -sb <data-dir>/raft-lsm`（lsm） |
| O3 | 启动恢复耗时 `OPEN_MS` | ms | 节点启动到 `status` 可用的墙钟；或 `LsmLogStore::Stats` + `RecoveryStats` |
| O4 | fsync 次数 / 总耗时 | 次 / us | 对 leader 用 `strace -f -e trace=fsync,fdatasync -c`（`bench_m5_cell.sh --strace-leader` 的既有手法） |
| O5 | LSM 层统计 | files/bytes per level | `PersistentDBImpl::GetLevelStats()`（`db_impl.h` L202） |
| O6 | 空间放大 | 比值 | `GetAmplificationStats()`（`db_impl.h` L203）或 `du` |
| O7 | 写停顿 | `stall_events` / `stall_micros` | `GetFlushStats()`（`db_impl.h` L286） |
| O8 | WAL 尾部截断字节 | bytes | `RecoveryStats::tail_truncated_bytes`（`db_impl.h` L42） |

> **纪律（必须写进 `docs/m6-bench.md`）**：**O1 劣化不构成 M6 失败**（「LSM 未必更快」是本里程碑预先接受的结论形态）。但 O1-O8 **必须全部有数字**，缺项要显式写「未采集 + 原因」。硬门禁只有 H1-H12。

### 5.3 交替执行与固定行格式

**交替纪律**（照搬 `bench_m5_ab.sh` L2-4 的三条）：同一脚本内 `base → file → lsm → base → file → lsm …` 交替；每档先跑 200 写预热（丢弃）；每档重复 `REPS` 次取**中位数**；每次 `fill` 后必须 `verify`。

**原始行格式（KEY=VALUE，空格分隔；前缀列冻结，只允许行尾追加）**

```
eng=<base|file|lsm> p=<int> rep=<int> n=<int> ms=<%.1f> ms_per_write=<%.3f> qps=<%d> verify=[<verify 原文>] logdir_bytes=<int> open_ms_max=<int> fsync_calls=<int> fsync_us_total=<int>
```

对齐既有 `bench_m5_ab.sh` L97 的行格式（`$eng p=$p rep=$rep n=$N ms=$ms ms_per_write=$lat qps=$qps verify=[$verify]`），**只允许在行尾追加列**。

**汇总表（固定表头，M6 版）**

```
| pipeline | n | base ms/w | file ms/w | lsm ms/w | base qps | file qps | lsm qps | lsm/file 延迟 | lsm/file 吞吐 | verify | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|---|
```

> ⚠️ **本里程碑不设比值判定**（不像 `bench_m5_ab.sh` L7-9 的 1.2×/0.8× 阈值）。理由：M6 的目标是**适配**而不是**提速**；先拿到可比数字，再由用户决定是否要求「不劣化 N%」。**这一条本身是 §8 D13 的拍板项。**

### 5.4 一键回滚到远端基线 `1463620`

```bash
# —— 回滚（在 ~/raft-kv-lsm 内；完全不碰 ~/raft-kv）——
git -C ~/raft-kv-lsm fetch origin
git -C ~/raft-kv-lsm stash list                     # 期望空；若非空先人工确认
git -C ~/raft-kv-lsm checkout --detach 1463620333eb2f16673f3d541aa62e1a35333c8a
rm -rf ~/raft-kv-lsm/build
cmake -S ~/raft-kv-lsm -B ~/raft-kv-lsm/build -DCMAKE_BUILD_TYPE=Release
cmake --build ~/raft-kv-lsm/build -j"$(nproc)"
git -C ~/raft-kv-lsm rev-parse HEAD
#   期望输出：1463620333eb2f16673f3d541aa62e1a35333c8a
git -C ~/raft-kv-lsm status --short                 # 期望空
bash ~/raft-kv-lsm/scripts/raft_e2e.sh              # 期望 raft_e2e: PASS

# —— 回到工作分支 ——
git -C ~/raft-kv-lsm checkout master
```

**回滚演示的门禁**：回滚后 `git rev-parse HEAD` == 远端基线；`raft_e2e.sh` PASS；`~/raft-kv` 的 `git status --short` 仍然只有那个未跟踪的 `scripts/demo_record.sh`（**未被 M6 触碰的证据**）。

> ⚠️ 该回滚命令**由父代理/用户执行**，M6 `#0` 不执行任何 `git checkout`/`reset`（只读侦察纪律）。

### 5.5 脚本骨架（设计级）

`scripts/bench_m6_ab.sh` 与 `bench_m5_ab.sh` 的差异仅限于：

1. 基线来源：`BASE_SRC=~/raft-kv`（**不用 `git worktree`** —— `~/raft-kv` 已经是独立克隆，避免了 `bench_m5_ab.sh` L41-52 那套 worktree 残留处理）。
2. 三个臂的二进制路径与 `extra args`（`lsm` 臂加 `--log-engine=lsm`）。
3. 行格式增加 `logdir_bytes` / `open_ms_max` / `fsync_calls` / `fsync_us_total` 四列（行尾追加）。
4. 汇总表换成 §5.3 的表头。
5. 退出码：任一格 `verify` 非 `missing 0` ⇒ `exit 1`（同 `bench_m5_ab.sh` L100）。

---

## §6 负结果与风险（**必须预留位置如实记录**）

> 本节的每一条都给出「预测 → 为什么 → 用什么数字判死 → 在哪一节入档」。**所有预测都必须在 M6.7 用实测替换或确认。** 在拿到数字之前，`docs/m6-bench.md` 的对应行必须写「未验证」。

### 6.1 结论的预期形态：**LSM 未必更快**

本里程碑**不承诺**任何性能收益。可以预期的是：

| 维度 | 预期方向 | 依据 |
|---|---|---|
| 小并发（p=1）延迟 | 可能**劣化**（每条多一层 batch/queue/manifest 开销） | lsm 的 `DB::Write` 要走 `SubmitPending` 队列 + flusher 选举 + `EncodeGroup`（`db_impl.cpp` L154-198、L409-433），`FileLogStore::appendNoSync` 是直接 `write()` |
| 高并发（p=8/64）吞吐 | **风险最大的一项，可能显著劣化** | 见 R1 |
| 前缀压缩（snapshot/compact） | 可能**改善** | `FileLogStore::compact` 是 O(保留后缀) 的**整份重写**（L465-495）；lsm 只写 tombstone，回收交给后台（但**当次不省空间**，见 R4） |
| 启动恢复 | 不确定（**需要实测**） | lsm 只需重放最后一次 flush 之后的 WAL（`m2-design.md` §5.5 L643），但 `DB::Open` 还要读 MANIFEST、清孤儿（`RecoveryStats` 的 6a/6b/6c）；`FileLogStore::load` 是纯顺序全文件扫描（L201-236） |
| 内存占用 | **改善** | 术语向量 8B/条 vs `entries_` 的整条 `LogEntry` + key + value |

### 6.2 需要实测的分母（**缺一不可**）

| ID | 分母 | 为什么必须 | 采集 |
|---|---|---|---|
| D1 | `ms_per_write`（p=1/8/64） | 吞吐的主指标 | `bench_m6_ab.sh` |
| D2 | P99 / 最大延迟 | 均值会掩盖 fsync 与后台 compaction 造成的长尾 | 需新增采样（§8 D12） |
| D3 | `logdir_bytes`（稳态 + compact 后） | 空间放大；也是 `LOG_BOUND` 的标定输入 | `du -sb` |
| D4 | `open_ms_max`（100k 条日志后重启） | 启动恢复代价 | 节点启动墙钟 |
| D5 | `fsync_calls` / `fsync_us_total`（leader） | 直接验证 R1 的组提交是否被打散 | `strace -c` |
| D6 | LSM 的 `stall_events` / `stall_micros` | R5 的写停顿 | `GetFlushStats()` |
| D7 | LSM 的 `compaction_rounds` / `compaction_round_max_us` | 后台 compaction 的抖动来源 | `GetAmplificationStats()` |
| D8 | `tail_truncated_bytes` | 证明 lsm 恢复路径确实处理了撕裂尾（否则「没截断」会被误读） | `RecoveryStats` |
| D9 | 单次 `truncateSuffix` 的条目数分布 | R3 的写放大到底多大 | `LsmLogStore::Stats` 的 `truncates` + 计数器 |
| D10 | 每次 `slice()` 的条数与调用次数 | R2 的迭代器开销到底多大 | `LsmLogStore::Stats` |

### 6.3 逐条风险

**R1（最高优先级）—— `lsm::DB::Sync()` 在 `commit_mu_` 之下做 fsync，会把组提交彻底打散。**

- **证据（可追溯到行号）**：`PersistentDBImpl::Sync()` 在 `db_impl.cpp` **L802** 取 `std::lock_guard<std::mutex> ql(commit_mu_);`，在 **L817** 调 `log_->Sync()`，**整个 fsync 期间持有 `commit_mu_`**。而 `PersistentDBImpl::SubmitPending()` 在 **L166** 用 `std::unique_lock<std::mutex> l(commit_mu_);` 覆盖入队。⇒ **在 `DB::Sync()` 的 fsync 窗口内，任何 `DB::Write`（包括 raft 的 `appendNoSync`）都无法入队。**
- **这正是 raft-kv 在 M5.6（D-2）已经踩过并修好的坑**：`file_log_store.cpp` L284-304 的注释逐字记录了实测结果 —— 「实测 p=8/3 节点：leader 1650 次 fsync / 2200 条写（1.33 条/次）、batch_avg=1，吞吐掉到 M4 基线的 **0.47×**」。
- **影响**：`cfg_.maxInflightPerPeer`、`groupCommitLingerUs`（`types.h` L104/L109）这些 M5 的组提交调优手段在 lsm 引擎下**可能全部失效**。
- **判定数字**：D5（`fsync_calls` 与「写条数 / fsync 次数」的比值）。`FileLogStore` 修复前的比值是 1.33 条/次；若 lsm 臂落在同一量级，则该风险**确认发生**。
- **可选的缓解（不在 M6 改动范围内）**：把 `log_->Sync()` 移出 `commit_mu_`（照搬 `FileLogStore` 的 `flushMu_` 叶子锁设计：`file_log_store.cpp` L296-303）。**这是 lsm 仓库的改动 ⇒ 由父代理裁决（§8 D10）**。
- **入档位置**：`docs/m6-bench.md` §"负结果 R1"。

**R2 —— `slice()` 每次创建 lsm 迭代器。**

- **证据**：`slice()` 必须走 `PersistentDBImpl::NewIterator()`（`db_impl.cpp` L680-687 → `BuildIterator` L694-720），它要 ref memtable + 全部 immutables + 当前 `Version`，并逐层构建 iterator。而 `buildPeerJobLocked()`（`raft_node.cpp` L250）在**心跳路径**上就会 `slice(next, 1, SIZE_MAX)`。
- **判定数字**：D10 + p=1 的 D1。
- **可选的缓解**：缓存「最近一次 append 的那批条目」（leader 的复制目标几乎总是命中它）。**M6 最小映射不做**（YAGNI），列为 §7 W5。

**R3 —— `truncateSuffix` 从 O(1) 退化为 O(被截断条目数)。**

- **证据**：`FileLogStore::truncateSuffixLocked` 是一次 `ftruncate`（L376）；`LsmLogStore` 要为每个被丢弃的索引写一条 `Delete`。
- **判定数字**：D9 + H5/H7（故障注入下的 `SIGSTOP`/`kill -9` 会制造长冲突后缀）。

**R4 —— 空间放大 + 物理空间不即时回收。**

- **证据**：`FileLogStore::compact` 之后 `raft.log` 立即变小（rename 新文件）；`LsmLogStore::compact` 只写 tombstone，空间由 lsm 后台 compaction 回收。`scripts/raft_snapshot_fault.sh` 的 `LOG_BOUND = 1048576`（L23）**直接读 `raft/raft.log` 的 `stat` 大小**（L116），在 lsm 引擎下没有这个文件。
- **判定数字**：D3（稳态 / compact 后 / 后台 compaction 完成后三个时点的目录字节数）。
- **处置**：C6（改 `LOG_BOUND` 口径）+ §8 D9（阈值标定）。

**R5 —— 写停顿（`WaitForImmutableCapacity`）。**

- **证据**：`db_impl.h` L284 `while (immutables_.size() >= kMaxImmutableMemTables && ...) bg_cv_.wait(l);` ⇒ memtable 冻结后若后台 flush 跟不上，**写者会停等**。`Options::write_buffer_size` 默认 4 MiB（`common.h` L287），而 `raft_snapshot_fault.sh` 会灌 **100 000 条**（L111）。
- **判定数字**：D6（`stall_events` / `stall_micros`）+ D2（P99）。

**R6 —— 启动恢复代价。**

- **证据**：lsm 恢复要过 `LOCK` → WAL 扫描（尾部判定 + `ResyncScan`）→ CURRENT/MANIFEST → 第二遍重放 → 孤儿清理（`RecoveryStats` 的 6a/6b/6c）；`FileLogStore::load` 是一遍顺序扫描。二者量级都不确定。
- **判定数字**：D4。

**R7 —— P99 抖动来自后台线程。**

- **证据**：lsm 有**两条**后台线程（flush + compaction，`db_impl.h` L389、L407），与 raft 的 ticker / reactor / 客户端线程争 CPU。本机是**单机多节点**（`bench_m5_ab.sh` 起 3 个 node、`bench_m5_cell.sh --nodes N`），CPU 争用会被放大。
- **判定数字**：D2 + `bench_m5_cell.sh` 既有的「节点 CPU 与机器忙度」列。

### 6.4 `docs/m6-bench.md` 的固定章节（预留位置）

```markdown
## 1. 环境（uname / nproc / loadavg / 文件系统 / 是否 tmpfs）
## 2. 原始行（§5.3 格式，逐行粘贴，不删不改）
## 3. 汇总表（§5.3 表头）
## 4. 硬门禁 H1-H12 的结果（每项一行：门禁 | base | file | lsm | 判据字符串）
## 5. 观测 O1-O8 / D1-D10 的数字
## 6. 负结果
   6.1 R1 组提交是否被打散（D5；结论：确认 / 未确认 / 未采集）
   6.2 R2 slice 开销（D10）
   6.3 R3 truncate 写放大（D9）
   6.4 R4 空间放大（D3）
   6.5 R5 写停顿（D6）
   6.6 R6 启动恢复（D4）
   6.7 R7 P99 抖动（D2/D7）
## 7. 未验证清单（每条一行 + 原因）
## 8. 结论（明确写：LSM 在哪些格子更快 / 更慢 / 无法区分）
```

---

## §7 已知薄弱点

| ID | 薄弱点 | 影响 | 缓解 / 登记 |
|---|---|---|---|
| W1 | lsm 的 `WriteBatch` **只有 `Put`/`Delete`**（`write_batch.h` L39-42），**没有范围删除** | `truncateSuffix` / `compact` 必须逐条 `Delete` ⇒ 写放大、延迟放大 | 登记；可选改进是 lsm 侧加 range tombstone（跨仓，§8 D10） |
| W2 | 术语向量（`terms_`）与 DB 的一致性**由手工维护** | 缓存 bug 在 DB 层**看不出来**，只能通过单测发现 | M6.2 的 17 条用例逐条覆盖；`all()` seam 便于对拍 |
| W3 | `slice()` 无法避免本地迭代器构造 | 心跳热路径开销 | §6 R2；W5 |
| W4 | 没有 `op == kConfig` 的二级索引 | `rebuildConfigFromSeedAndLog`（`raft_node.cpp` L1175-1180）与 `recomputeConfigLocked`（L1359-1364）会**全量扫描**日志并解码每条 value | 最小映射不做；可选 `0x03 || op || BE64(index)` 二级索引 ⇒ **但这会让 truncate/compact 要删两份**，复杂度不划算。登记为「不做，除非实测显示这是瓶颈」 |
| W5 | 没有「最近 append 批次」的内存缓存 | 见 W3 | 可选优化，M6 不做 |
| W6 | `<data-dir>/raft`（file 引擎）与 `<data-dir>/raft-lsm`（lsm 引擎）**物理隔离** | 同一 `--data-dir` 上切换引擎会「看不到」另一引擎的日志。**推理**：由于 `lastIncluded_` 来自快照而快照目录是共享的，切到 lsm 后 `load()` 会看到「空日志 + 边界 I」= R5 情形，可能是可用状态。**这是推理，未验证。** | 明确**不支持**在已有数据目录上切换引擎；A/B 脚本每格使用全新临时目录 |
| W7 | lsm 的 `LOCK` 文件（D10） | 同一数据目录不能被两个进程 `Open`；单测里连续建/销毁 store 必须确保前一个已析构 | M6.2 `TwoStoresOnSameDirConflict` |
| W8 | lsm 的**中间损坏 ⇒ 拒绝启动**（`m2-design.md` §5.3）比 `FileLogStore` 的「空洞当撕裂尾截断」严格 | 可用性上更保守 | 视为**改进**；登记，需在 `docs/m6-bench.md` §7 说明 |
| W9 | `TruncatesTornTail` 类用例**在 lsm 引擎下没有等价的注入点** | 该场景的等价性可能**未被覆盖** | M6.3 若无法构造，如实标注「未覆盖」，改由 M6.6 的 `kill -9` 真实注入覆盖 |
| W10 | `DB::Sync()` 与 `RaftNode` 的「夹紧 durable 到 `min(flushTarget, lastIndex())`」的**交错正确性** | `raft_node.cpp` L820-821/L357/L605 的夹紧是为 `FileLogStore` 写的；对 lsm 是否仍然充分，**需要一次论证 + 一条定向用例** | 论证：`DB::Sync()` 的承诺是「此前所有已返回 kOk 的写」，是 `FileLogStore::sync()` 承诺的**超集**，夹紧只可能少报 ⇒ 安全。**但这是论证，不是实测**，登记待评审 |
| W11 | `Session/tag` 政策 | `m6-raft-integration` 要打在 `~/lsm-kv`，而该仓库**不在 M6 改动范围内** | **由父代理执行**（§8 D11） |
| W12 | 本机无 g++/cmake | 所有构建/测试都必须在 `ubuntu-vm` 上跑；M6 `#0` 只做只读侦察 | 已在 §0.7 登记 |

---

## §8 需拍板清单

> 格式：`D<n> <问题>` → **推荐值** → 状态：**用户已授权按推荐执行**。

| ID | 问题 | 推荐值 | 状态 |
|---|---|---|---|
| **D1** | LSM 数据目录放哪？ | `<data-dir>/raft-lsm`（与 `FileLogStore` 的 `<data-dir>/raft` 隔离）；由可选的 `--log-engine` 决定用哪个 | **用户已授权按推荐执行** |
| **D2** | `term/votedFor` 存哪？ | **存进 lsm**（key `0x02 "meta"`，12 字节 value，单条 `Put` + `Write{sync=true}`）；不保留 `meta.dat` | **用户已授权按推荐执行** |
| **D3** | 日志条目的 value 是否冗余携带 `index`？ | **携带**（复用 `encodeEntry`/`decodeEntry`，`load()` 可自校验 `key.index == value.index`），代价 8B/条 | **用户已授权按推荐执行** |
| **D4** | key 前缀方案 | `0x01 \|\| BE64(index)`（日志）、`0x02 "meta"`（元数据）；`0x00` 段保留 | **用户已授权按推荐执行** |
| **D5** | `load()` 遇到「索引空洞」怎么办？ | **拒绝启动**（`return false`），因为 lsm 的 WAL 保证下空洞只可能来自介质级损坏 | **用户已授权按推荐执行** |
| **D6** | `truncateSuffix(fromIndex == lastIndex()+1)`（无条目可删）是否仍要 fsync？ | **不 fsync**，直接 `return true`（没有任何变更需要持久化） | **用户已授权按推荐执行** |
| **D7** | 引擎开关的名字与默认值 | `--log-engine=file\|lsm` + 环境变量 `RAFTKV_LOG_ENGINE`，**默认 `file`**（保证 M2-M5 门禁逐字节不变） | **用户已授权按推荐执行** |
| **D8** | lsm 怎么接进 CMake | cache 变量 `RAFTKV_LSM_DIR`（默认 `$ENV{HOME}/lsm-kv`）+ `add_subdirectory`；目录不存在则**不定义 `RAFTKV_HAVE_LSM`**，`--log-engine=lsm` **报错退出**（绝不静默降级） | **用户已授权按推荐执行** |
| **D9** | `raft_snapshot_fault.sh` 的 `LOG_BOUND` 在 lsm 引擎下的口径 | 改为「`<data-dir>/raft-lsm` 目录总字节 ≤ 阈值」，阈值由 M6.6 实测标定后写进 `docs/m6-bench.md`；file 引擎的 1 MiB 判据**保持原样** | **用户已授权按推荐执行** |
| **D10** | 是否要 lsm 侧修「`DB::Sync()` 持 `commit_mu_` 做 fsync」（R1） | **M6 不做**（跨仓、且 `~/lsm-kv` 不在改动范围）。登记为 M7 候选，**由父代理裁决** | 需父代理裁决 |
| **D11** | tag `m6-raft-integration` 打在 `~/lsm-kv` | **由父代理执行**（该仓库不在 M6 改动范围内）。另：实测 `~/lsm-kv` **没有 `m5-*` tag**（只有 `m1-memtable`/`m2-wal`/`m3-sstable`/`m4-compaction`），打 M6 tag 前请确认基线 tag 口径 | 需父代理执行 |
| **D12** | 是否为本里程碑引入 P99 延迟采样 | **是**（R7/D2 需要）；最小做法：node 的 `status` 输出增加 `p99_ms` 与 `max_ms`（或在 `Metrics` 里加直方图），只读、不参与判定 | **用户已授权按推荐执行** |
| **D13** | A/B 是否设**比值硬门禁**？ | **不设**。M6 是适配而非提速；先拿到 §5.3 的固定格式数字，再由用户决定是否加「不劣化 N%」的阈值。**「LSM 未必更快」的结论必须如实入档** | **用户已授权按推荐执行** |
| **D14** | 是否新增 `docs/m6-bench.md` 与 `docs/m6-review.md` | `m6-bench.md` **必须有**（§6.4 的固定章节）；`m6-review.md` 可选 | **用户已授权按推荐执行** |
| **D15** | `tests/raft_*` 的 `Disk` fixture 参数化范围 | 只参数化**已经在用真实临时目录**的 Disk 系列；**不动** `makeCluster`（MemoryLogStore + FakeClock 的确定性用例） | **用户已授权按推荐执行** |

---

## §9 修订记录

| 版本 | 日期 | 作者 | 变更 |
|---|---|---|---|
| v1.0 | 2026-10-01 | M6 `#0` | 初稿：§0 只读侦察（raft-kv-lsm@1463620 / raft-kv@1463620 / lsm-kv@99c417f）；§1 目标与非目标 + C1-C9 必要小改；§2 位级布局 + 逐方法映射 + `truncateSuffix` 与重启恢复的等价性论证；§3 崩溃与持久化矩阵；§4 M6.0-M6.8 子里程碑；§5 三臂 A/B + 硬门禁/观测分类 + 一键回滚；§6 负结果预留（R1 为最高优先级）+ 固定行格式；§7 12 条薄弱点；§8 15 条拍板项 |

---

## 附录 A：本次只读侦察实际跑过的命令

全部通过 `timeout 120 ssh -o BatchMode=yes ubuntu-vm '<cmd>'` 执行；**没有任何写操作、没有 commit/push、没有碰 `~/raft-kv` 与 `~/lsm-kv` 的工作树**。

```bash
# A. 三个副本的状态
cd ~/raft-kv-lsm && pwd && git log --oneline -5 && git remote -v && git status --short && git rev-parse HEAD
cd ~/raft-kv     && git log --oneline -1 && git status --short && git remote -v
cd ~/lsm-kv      && ls -la && git log --oneline -3 && git tag

# B. 要适配的接口
cd ~/raft-kv-lsm && cat -n src/raft/log_store.h
cd ~/raft-kv-lsm && cat -n src/raft/file_log_store.cpp
cd ~/raft-kv-lsm && cat -n src/raft/memory_adapters.cpp && cat -n src/raft/lock_probe.h && cat -n src/raft/types.h
cd ~/raft-kv-lsm && cat -n src/raft/raft_node.h
cd ~/raft-kv-lsm && grep -n "log_\.\|LogStore\|persistMeta\|truncateSuffix\|appendNoSync\|\.sync()\|setBoundary\|compact\|firstIndex\|lastIncluded" src/raft/raft_node.cpp
cd ~/raft-kv-lsm && sed -n '30,135p;310,380p;440,620p;718,760p;765,935p;960,1035p;1160,1200p;1350,1380p' src/raft/raft_node.cpp

# C. 门禁与目标清单
cd ~/raft-kv-lsm && cat -n CMakeLists.txt
cd ~/raft-kv-lsm && cat -n scripts/e2e.sh scripts/raft_e2e.sh scripts/raft_fault.sh scripts/bench_m5_ab.sh
cd ~/raft-kv-lsm && wc -l tests/*.cpp tests/*.h scripts/*.sh CMakeLists.txt
cd ~/raft-kv-lsm && cat -n tests/raft_log_test.cpp tests/raft_restart_test.cpp tests/raft_test_harness.h
cd ~/raft-kv-lsm && grep -hn '^TEST(\|^TEST_F(' tests/raft_*.cpp
cd ~/raft-kv-lsm && grep -n 'PASS\|FAIL\|LOG_BOUND' scripts/raft_snapshot_fault.sh scripts/raft_membership_e2e.sh
cd ~/raft-kv-lsm && sed -n '1,40p' scripts/bench_m5_cell.sh

# D. lsm 的对外契约（**用 git show HEAD: 读提交内容，避开 M5.3 的脏工作树**）
cd ~/lsm-kv && git show HEAD:src/db.h
cd ~/lsm-kv && git show HEAD:src/write_batch.h
cd ~/lsm-kv && git show HEAD:src/common.h
cd ~/lsm-kv && git show HEAD:src/db_impl.h
cd ~/lsm-kv && git show HEAD:src/db_impl.cpp | sed -n '120,224p;224,300p;340,500p;675,720p;795,835p'
cd ~/lsm-kv && git show HEAD:docs/m2-design.md | sed -n '90,120p;238,266p;525,545p;592,660p;877,935p'
cd ~/lsm-kv && sed -n '40,100p' CMakeLists.txt && ls -la build/*.a

# E. raft-kv 侧符号确认（避免引用不存在的符号）
cd ~/raft-kv-lsm && grep -n 'void putU32\|uint32_t getU32\|using Bytes\|enum class OpCode\|using Byte' src/common.h
cd ~/raft-kv-lsm && grep -n 'FileLogStore\|FileSnapshotStore\|KvStateMachine\|--transport' src/main_raft_node.cpp
```

**未跑过、因此未验证的命令**：任何 `cmake`/`make`/测试/基准；任何 `git commit`/`push`/`checkout`/`reset`；任何对 `~/raft-kv` 或 `~/lsm-kv` 的写操作。

---

## §10 M6.9 追加修订：状态机数据落到 LSM（状态机后端）

> 本节是**纯追加**修订，不改动 §0–§9 与附录 A 的任何一行。M6.9.0 交付本节设计。

### 10.1 背景与已裁决决策

- `RaftNode` 只依赖抽象 `raft::StateMachine&`；进程内实现是纯内存的 `KvStateMachine`（M2 决策 D1）。
- M6.1–M6.8 只把 **LogStore** 换成了 lsm；状态机数据仍在内存。M6 目标 2 要求状态机数据落到 lsm。

### 10.2 目标 / 非目标

**目标**
1. 新增 `LsmKvStateMachine`，让状态机的 `数据 / 去重表 / lastApplied` 由 lsm 引擎持久承载。
2. `apply` 对同一 `(clientId, requestId)` 幂等；三者**同一原子批**落盘（M6-I10）。
3. 快照边界不变：`snapshotView()` 仍是锁内纯内存拷贝，`restore()` 仍是整体替换；`restore` 后**权威** `lastApplied`（`RaftNode::lastApplied_`）不低于快照边界（归属见 §10.14）。
4. 开关切换；默认 mem 路径**零行为变更**；两条路径都可跑通 M6 门禁。
5. 编解码安全（M6-I11）：畸形/越界载荷不得越界读、不得 panic。

**非目标（硬边界）**

| # | 非目标 | 理由 |
|---|---|---|
| N-A | 不改 `raft_node.cpp` 的共识逻辑 | 与 N1 一致；状态机替换只经由既有 `StateMachine` 接口 |
| N-B | 不改快照文件格式与 `SnapshotStore` 接口 | 线上/磁盘兼容 |
| N-C | 不改 `~/lsm-kv` | M6 范围外 |
| N-D | **不降低内存占用**：本里程碑保留内存镜像（read-side mirror） | `snapshotView()` 契约要求锁内纯内存；`get()` 在共识锁内 ⇒ 不能在锁内读盘。降 RSS 是后续独立课题 |
| N-E | 不做 file→lsm 状态机数据迁移 | YAGNI，同 N6 |
| N-F | 不支持 >64 MiB 的单次 `restore` 差量批（lsm 单批上限） | 超出时 `restore()` 明确返回 false，绝不静默截断 |

### 10.3 类与物理布局

- `src/kv/lsm_kv_state_machine.h`：只前置声明 `namespace lsm { class DB; }`，持有 `unique_ptr<lsm::DB, LsmDbDeleter>`（pimpl）。**不 include 任何 lsm 头**（分层纪律：LSM 类型只允许出现在适配层实现文件内）。
- `src/kv/lsm_kv_state_machine.cpp`：唯一 include lsm 头（`db.h` / `write_batch.h`）的适配层实现文件。`raft_node.cpp` 不出现任何 LSM 头。
- 数据目录：`<data-dir>/kv-lsm`。与 `FileLogStore` 的 `<data-dir>/raft`、`LsmLogStore` 的 `<data-dir>/raft-lsm` **物理隔离**：三个 DB 互不干扰，`raft_snapshot_fault.sh` 的 `LOG_BOUND` 口径（只看 `raft-lsm`）不受影响。
- 键空间（**内部格式，非协议**：不进快照文件、不上网）：

| key | value | 说明 |
|---|---|---|
| `0x00` + `applied`（8B） | BE64(lastApplied) | 同批的 applied 水位 |
| `0x01` + userKey | value | KV 数据（前缀保证与其余两类不冲突） |
| `0x02` + BE64(clientId)（9B） | BE64(requestId) | 去重表 |

- 读侧镜像（mirror）：构造时全量扫描 lsm 重建 `data_ / lastRequest_ / lastApplied_`；`get()/snapshotView()/lastApplied()` 只读 mirror（纯内存、无 I/O）。

### 10.4 与既有 KvStateMachine 的等价性对照表

| 方法 | `KvStateMachine`（mem） | `LsmKvStateMachine`（lsm） | 等价性 |
|---|---|---|---|
| `apply(kPut/kDel)` | 改 `data_`；更新 `lastRequest_`；`lastApplied_=index` | 同一 `WriteBatch`：数据 Put/Delete + 去重 Put + applied Put；**写成功后再改 mirror** | 数据/去重语义等价；多一条 WAL 写（代价见 §10.12） |
| `apply(重复 (cid,rid))` | 丢弃（直接 return） | 丢弃（不写批、不推进） | 两者都**不推进** `lastApplied_`（见 §10.14） |
| `apply(kGet/kConfig)` | no-op，仅推进 `lastApplied_` | 只写 applied 的批 | 等价 |
| `get(k)` | `data_.find` | `data_.find`（mirror） | 等价 |
| `lastApplied()` | mirror 值 | mirror 值 | 等价 |
| `snapshotView()` | 拷贝 `data_/lastRequest_/lastApplied_` | 同上（从 mirror 拷贝） | 载荷逐字节相同（测试钉住） |
| `restore(payload)` | 解析并整体替换三张表 | 解析 → 与 mirror 求差 → **一个 WriteBatch** → 替换 mirror | 载荷格式相同；lsm 版多一次原子批写 |

注（已按 §10.14 修订）：重复条目按 m2-design §6.5 是「直接丢弃」，**不改数据也不推进 applied 水位**。既有 `KvStateMachine` 保持原样（基线不动），LSM 后端逐字镜像它。

### 10.5 原子落盘与 durable 落点表

**核心事实**：Raft 的 ack 由 **raft 日志的 fsync** 保证；状态机是从 durable 快照 + durable 日志可**确定性重建**的派生态。因此状态机自身的 fsync **不在 ack 关键路径**（与 TiKV 等把 raftdb 作为真相源、state db 异步落盘的做法同构）。

| 操作 | 锁 | WriteBatch 内容 | fsync 落点 | ack 依赖 |
|---|---|---|---|---|
| `apply(kPut)` | 持 `mu_`（`advanceCommitAndApply`） | 数据 Put + 去重 Put + applied Put（1 条 record） | **无**（`WriteOptions.sync=false`，只写 WAL/memtable，等同 `LogStore::appendNoSync`） | 不依赖；ack 依赖 `log_.sync()` |
| `apply(kDel)` | 同上 | 数据 Delete + 去重 Put + applied Put | 无 | 同上 |
| `apply(kGet/kConfig)`（no-op 标记） | 同上 | applied Put | 无 | 同上 |
| `apply(重复 (cid,rid))` | 同上 | **不写**（直接丢弃） | 无 | 同上 |
| `restore(payload)` | 持 `mu_`（构造期不持；`onInstallSnapshot` 持） | Put/Delete 差量 + applied Put（1 条 record） | 无 | 同上 |
| 优雅关闭 | 无 | — | 显式 `LsmKvStateMachine::sync()`（`main` 在 `_Exit` 前调用） | — |
| `sync()`（测试/诊断缝） | **不得持 `mu_`** | — | `DB::Sync()` | — |

**同一原子批落盘的口径**：三者进同一条 lsm WAL record，`DB::Open` 的恢复（含尾部截断）保证**要么整条都在、要么整条都不在** ⇒ 崩溃重启后绝不会出现「数据在、去重表丢」或反之。重启一致性由「lsm 原子批 + 从日志重放幂等」双重覆盖。

**为什么不在 ack 前 fsync 状态机**：
1. M6-L2 禁止持 `mu_` 做 fsync；而 `apply` 在 `mu_` 内被调用（`advanceCommitAndApply`）。
2. 若要在 ack 前 fsync，需要在 `raft_node.cpp` 的 group-commit 结构里、释放 `mu_` 之后与 `cv_.notify_all()` 之前插一次 SM fsync —— 动了 N1 的共识路径，收益仅是少一次重启重放，不值。
3. 正确性不依赖它：ack 前日志已 durable；重启时 `RaftNode` 按 快照边界 + 日志重放 重建状态机。

### 10.6 锁序（单向）

```text
RaftNode::mu_  ->  LsmKvStateMachine（无自有锁，全部调用由 mu_ 串行）  ->  lsm DB 内部锁（WAL/memtable/version）
```

- 固定单向，禁止反向。`snapshotOpMu_`（raft 侧叶子锁）只在 `mu_` 之外获取，与状态机无关。
- `LsmKvStateMachine` **不自带**互斥（与 `KvStateMachine` 一致）：调用方用 `mu_` 串行；`get()` 也必须在 `mu_` 内。
- `sync()` 是唯一例外：它只碰 lsm 内部锁、可无 `mu_` 调用；调用期间不读 mirror。
- 与 `LsmLogStore` 的关系：两个**独立** DB（`kv-lsm` vs `raft-lsm`），各自内部锁、无嵌套 ⇒ 无死锁环。

### 10.7 关闭顺序

期望顺序（局部对象声明顺序的逆序）：
```text
停止 accept -> 停 ticker（join）-> reactor.stop() -> RaftNode 不再使用 sm
  -> 显式 LsmKvStateMachine::sync()（优雅落盘点）
  -> 进程 _Exit（既有实现：避免 detached 连接线程触碰已析构对象）
```
既有 `main_raft_node.cpp` 末尾用 `std::_Exit(0)`，栈对象析构**不运行**，因此 `DB::Close()` 不会发生；本里程碑在 `_Exit` 前补一次显式 `sync()`（lsm 臂），把「优雅关闭 = durable」写实。崩溃（`kill -9`）时不依赖它：WAL 已在 page cache，掉电场景靠日志重放。

### 10.8 开关与回退验证方案

- CLI：`--state-engine=mem|lsm` 与 `--state-engine mem|lsm`；环境变量 `RAFTKV_STATE_ENGINE` 覆盖（沿用 `--log-engine` 的既有模式，见 M6.5-D1：本机 → VM 通道上 `VAR=value cmd` 前缀赋值不可靠，**CLI 参数是权威通道**，环境变量仍照传）。
- **默认 mem** ⇒ 不带开关时逐字节走 M2–M6.8 既有实现，零行为变更。
- 未知值 ⇒ `exit 2` 并打印 `unknown --state-engine`，**绝不静默降级**。
- 未把 lsm 编进二进制（`RAFTK_HAVE_LSM` 未定义）时，`LsmKvStateMachine` 构造函数抛明确错误（含修复提示）⇒ main catch 后非 0 退出。
- **一键回退**：去掉 `--state-engine`（或显式 `--state-engine=mem`）即回到既有实现。
- 验证：`--state-engine=mem` 与 `=lsm` 两条路径各跑一遍同组门禁（M6.9.3）。

### 10.9 测试矩阵

| 层 | 载体 | 断言要点 |
|---|---|---|
| 契约单测（mem + lsm 同一组断言） | `tests/sm_contract_body.h`（模板体）+ `tests/raft_sm_contract_test.cpp`（KvStateMachine）+ `tests/raft_lsm_state_machine_test.cpp`（LsmKvStateMachine） | 幂等重放（同 cid/rid 不同 index：数据不变、applied 不前进 —— m2-design §6.5 直接丢弃）；`kGet/kConfig` 推进 applied；快照载荷**逐字节相同**（serialize 与流式拼接）；restore 往返 / 交叉 restore；畸形载荷拒绝；空状态 |
| LSM 专属：崩溃一致性 | `tests/raft_lsm_state_machine_test.cpp` | 写 N 条 → 不析构直接重开（等价 kill -9）→ 三者一致；重放同批 → 幂等；WAL 尾部注入垃圾 → 恢复出**一致前缀**、不 panic |
| LSM 专属：restore 原子性 | 同上 | `restore` 后重开，数据/去重/applied 三者都到位 |
| 端到端门禁 | `scripts/raft_e2e.sh` / `raft_fault.sh` / `raft_snapshot_fault.sh` / `raft_membership_fault.sh` 各加 `--state-engine` | `--state-engine=lsm` 至少 10 轮 PASS；`kill -9` 后 `verify missing 0` |
| Sanitizer | ASan / TSan 构建 | 新代码路径 0 越界 / 0 data race |
| Release 干净重建 | `build/` | 0 warning + 全量用例全绿 |

### 10.10 边界 case 全集

1. 空状态：无任何 apply，`lastApplied()==kNoIndex`，`snapshotView()` 序列化只有头部。
2. 单 key 的 put/get/overwrite/del；del 不存在的 key（幂等成功、推进 applied）。
3. 同 key 不同 `(cid,rid)`：后者覆盖前者。
4. 同 `(cid,rid)` 重放到**更高 index**：数据不变、`lastApplied` 前进（C13）。
5. `rid` 相等或更小：视为重复。
6. `kGet` / `kConfig` 标记条目：只推进 applied。
7. 快照往返：restore 后再 snapshot，载荷逐字节不变；交叉 restore（mem↔lsm）成立。
8. 流式快照：`maxChunk` 极小（1 字节）时多次 `next()` 拼接结果与 `serialize()` 逐字节相同。
9. 畸形载荷全集：长度 <8、kvCount 越界、klen/vlen 之和溢出、dedupCount 越界、尾部多余字节、截断在任意字段中间 ⇒ `restore()` 返回 false、状态不变、无越界读 / panic。
10. 崩溃一致性：写入后不析构重开；WAL 尾部撕裂垃圾 ⇒ 一致前缀 + 尾部截断计数。
11. 大状态：100k 条快照往返（门禁 A 段覆盖）。
12. 重启后 `restore` 的 applied ≥ 快照边界。

### 10.11 允许的追加小改（C10–C15）

| # | 改动 | 位置 | 理由 |
|---|---|---|---|
| C10 | 新增 `src/kv/lsm_kv_state_machine.{h,cpp}` | 新文件 | M6.9 的核心逻辑 |
| C11 | `CMakeLists.txt`：`lsm_kv_state_machine.cpp` 进 `raftkv_raft`；新测试文件进 `raftkv_raft_tests` | L55-70 / L110+ | 构建接线 |
| C12 | `src/main_raft_node.cpp`：`--state-engine=mem|lsm` + `RAFTKV_STATE_ENGINE`（默认 mem）；`_Exit` 前显式 `sync()` | 仿 `--log-engine` | 开关 + 优雅落盘点 |
| C13（已按 §10.14 撤销） | **不修改** `src/kv/kv_state_machine.cpp`（基线） | — | m2-design §6.5：「直接丢弃」；LSM 适配层逐字镜像 mem |
| C14 | `scripts/raft_{e2e,fault,snapshot_fault,membership_fault}.sh` 透传 `--state-engine` | 各参数解析 + `start_node()` | 用同一套门禁驱动两条路径 |
| C15 | `docs/m6-evidence.md` / `README.md` / `docs/roadmap.md` 追加 | 文档 | M6.9.4 收口 |

C10–C15 之外的一切改动都需要父代理裁决。

### 10.12 风险 / 负结果预留（必须实测后如实回填）

| # | 风险 | 预期 / 应对 |
|---|---|---|
| R-A | 写放大：每条 `apply` 多一条 lsm WAL 写（数据 + 去重 + applied 三行） | 实测 `--state-engine=lsm` 的吞吐 / 延迟代价，作为负结果登记；不设比值硬门禁 |
| R-B | 内存未降：mirror 与 mem 臂同阶 | 明确登记为 N-D；不建议在 M6.9 内解决 |
| R-C | `restore()` 差量批可能很大（InstallSnapshot） | 单批上限 64 MiB；超限返回 false（N-F），文档登记 |
| R-D | 复制格式漂移：LSM 版与 mem 版载荷编码是两份实现 | 用「逐字节相同 + 交叉 restore」测试钉死；若漂移则测试直接红 |
| R-E | `_Exit` 下优雅落盘仅在 lsm 臂显式 `sync()` 后成立 | 崩溃一致性用重开测试独立覆盖，不依赖关闭路径 |

### 10.13 未做项（本设计时点，逐条）

1. 未在 `raft_node.cpp` 引入「SM fsync 与日志 group commit 合并」的钩子（理由见 §10.5）。
2. 未做状态机数据的 file→lsm 迁移工具（N-E）。
3. 未降低状态机内存占用（N-D）。
4. 未改 `~/lsm-kv`；若实现中发现 lsm 侧限制（例如 `DB::Write` 的批上限、`Iterator` 语义）会在此登记并停下报告。

### 10.14 M6.9.2 实现期修订：`lastApplied` 归属与 mem 基线不动

**触发**：M6.9.1 的 RED 里，`Contract_IdempotentHigherIndexAdvancesApplied` 在既有 mem 实现上失败（原始输出 `docs/raw/m6.9.1-red-mem-contract.log`）。我最初据此判定 mem 有缺陷；经父代理裁决并回查权威契约后**推翻该判定**：

- **契约原文**：`docs/m2-design.md` §6.5 —— 「`apply()` 规则：`requestId <= lastRequestId` -> **直接丢弃**（重复请求），否则应用并更新」；`src/raft/state_machine.h` 只要求 「`apply()` MUST be idempotent per (clientId, requestId)」。**没有任何契约行**要求「被丢弃的重复条目必须推进 `lastApplied`」。
- **判定**：`IdempotentHigherIndexAdvancesApplied` 是**测试侧过度断言**（发明了契约），不是 mem 基线违约。按「找不到原文引用 -> 改测试」处理：重命名为 `IdempotentHigherIndexKeepsData`，断言「数据不变 + `lastApplied` 不前进（仍为 1）」，mem/lsm 两条腿都跑。
- **mem 基线不动**：`src/kv/kv_state_machine.cpp` 保持 HEAD 原样（`git diff` 为空）；M6.9 的全部新逻辑只在新增适配层。
- **LSM 后端逐字镜像 mem**：重复分支直接 `return`（不写 WriteBatch、不推进水位）；`kGet/kConfig` no-op 分支推进并落一条 applied 批。

**`lastApplied` 的归属（谁拥有 / 谁持久化 / ack 前需到哪一步）**：

| 角色 | 归属 | 持久化 | ack 前需到哪一步 |
|---|---|---|---|
| 权威 applied 边界 | `RaftNode::lastApplied_`（`mu_` 保护） | 不单独持久化；由 durable 日志重放 + 快照边界重建 | ack 只要求日志 durable（`log_.sync()`）；applied 由 `advanceCommitAndApply` 推进后唤醒等待者 |
| 快照边界 | `RaftNode::lastIncluded_` / `SnapshotData::lastIncludedIndex` | `FileSnapshotStore`（快照文件） | 快照 <= `RaftNode::lastApplied_` 由 `maybeSnapshot()` 保证（`snapIndex = lastApplied_`） |
| SM 内部 applied 水位 | `KvStateMachine::lastApplied_` / `LsmKvStateMachine::lastApplied_`（派生镜像） | mem：无（由重放重建）；lsm：与数据同 `WriteBatch` 落 applied key | **不在 ack 关键路径**；它是「该 SM 处理到哪条」的派生水位，允许在「被直接丢弃的重复条目」上不前进 |

因此 M6-I10 的「restore 后 `lastApplied` 不低于快照边界」在本设计里按**权威 applied 边界**（`RaftNode::lastApplied_`，restore / InstallSnapshot 时被显式置为快照边界）判定并成立；SM 的派生水位不承担该不变量。**本节是对 §10.2 目标 3 / §10.4 / §10.5 / §10.9 / §10.11 (C13) 的修订，以本节为准。**

**RED 逐条分类（对应父代理规则 2）**：

| RED | 类别 | 处置 |
|---|---|---|
| `fatal error: kv/lsm_kv_state_machine.h: No such file`（`docs/raw/m6.9.1-red-compile.log`） | (a) LSM 版缺失 | 正常，M6.9.2 实现后消失 |
| `KvStateMachineContract.IdempotentHigherIndexAdvancesApplied`（`docs/raw/m6.9.1-red-mem-contract.log`） | (b) 实为测试侧过度断言 | 按 m2-design §6.5 改测试（见上），mem 基线不动 |


### 10.15 M6.10.1 修订：restore 的分块原子替换与超大值（B1）

**触发（评审 B1）**：`restore()` 把整个差量一次 `WriteBatch` 写盘；lsm 的 `WriteBatch::kMaxBytes = 64 MiB`、`kMaxCount = 2^20`，>64 MiB 的载荷直接 `kInvalidArgument`。而 `raft_node.cpp` 的 InstallSnapshot 是先 `truncateSuffixNoSync` 再 `restore`，restore 失败后 follower 永远追不上（liveness，非 Raft 安全）。此外单个值 >64 MiB 连 `DB::Put` 都存不下（`wal.h: kMaxLogicalRecordSize = 64 MiB`）。

**方案：双命名空间 + 值分块 + 单键指针提交**（不改 `raft_node.cpp`、不改快照载荷格式）。

键布局（内部格式，非协议；`nsb` 为命名空间字节 0x11/0x12）：

| key | value |
|---|---|
| `0x00 "ns"` | BE64(当前命名空间 ∈ {0,1})，缺省 0 |
| `0x00 "pending"` | BE64(目标命名空间)，仅 restore 期间存在 |
| `nsb 0x00` | BE64(lastApplied) |
| `nsb 0x01 <userKey>` | 内联值（<= 1 MiB） |
| `nsb 0x02 <BE64(cid)>` | BE64(rid) |
| `nsb 0x04 <userKey>` | BE64(valueId) BE64(totalLen) BE32(chunkCount) |
| `nsb 0x03 <userKey><BE64(valueId)><BE32(i)>` | 第 i 块（<= 4 MiB） |

常量：`kInlineValueMax=1 MiB`、`kValueChunkSize=4 MiB`、`kMaxOpsPerBatch=4096`、`kMaxBytesPerBatch=8 MiB`（与 `LsmLogStore` 同量级）。

**restore 步骤与可见性**：① 清空 target 命名空间（上次中断残留）；② 写 `pending=target`；③ 把完整新状态按 <=4096 ops / <=8 MiB 分批写入 target（大值拆 <=4 MiB chunk + header，**无 header 不可见**）；④ **提交点**：一个 `WriteBatch` 同时写 `applied(target)=la`、`ns=target`、删 `pending`（一条 WAL record，原子）；⑤ 提交成功后才替换内存 mirror 与 `ns_`；⑥ best-effort 清旧命名空间，崩溃由启动 GC 兜底。

**崩溃 / 失败语义（收敛证明）**：
- 崩在 ①–③：`ns` 未变 -> 旧命名空间完整（data/dedup/applied 同属旧 ns）-> 读到的必然是旧状态；target 为垃圾，启动 `gcNonCurrentNamespace()` 删除非当前 ns 的所有 key。**不可能出现「数据在、去重表丢」**。
- 崩在 ④ 之后：`ns=target`，而 target 在 ③ 已写全 -> 新状态完整。
- 崩在 ⑥：`ns` 指向新状态，旧 ns 为垃圾 -> 启动 GC 清理。
- **重入幂等**：每次 restore 都把完整状态写入 target 再翻转，重复调用收敛到同一状态。
- **`lastApplied` 推进时机**：只在 ④ 提交批次落盘、只在 ⑤ 后对读路径可见，绝不早于全量数据 + 去重表。

**`apply` 的超大值与原子性**：值 >1 MiB 时先写 chunk（无 header -> 不可见），再在**一个** `WriteBatch` 里写 header + dedup + applied（提交）；旧值 >阈值时同批覆盖/删除旧 header，旧 chunk 提交后 best-effort 删除（孤儿 chunk 启动/下次 restore 回收）。因此「数据可见 + 去重 + applied」仍是一次原子提交。

**兼容性**：状态机 lsm DB 的内部格式 v2。v1（M6.9）的 `0x01/0x02/0x00` key 不属于任何命名空间，启动 GC 当垃圾删除；由于状态机可由 durable 日志 + 快照确定性重建，删除旧格式不丢正确性，只是一次重建。

**已知阻塞（M6.10.1 BLOCKER，未修）**：lsm 引擎 flush 与 Close/kill -9 竞态会留下孤儿 `*.sst`（无 MANIFEST），`DB::Open` 直接 Corruption；既有 `LsmLogStore` 同样受影响。因此「apply 一个大值后立即 Close->reopen」在当前 lsm 基座下确定性失败，本里程碑**不宣布该项通过**，详见 `docs/raw/m6.10.1-BLOCKER-lsm-flush-close.md`。restore 路径的重开用例当前通过，但在该竞态下不保证长期稳定。
