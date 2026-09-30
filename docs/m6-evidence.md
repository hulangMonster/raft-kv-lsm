# M6 证据与偏差登记（docs/m6-evidence.md）

> 本文件是 M6 各子里程碑的**原始证据 + 偏差/裁决登记**的唯一落点。
> 与 `docs/m6-design.md` 的关系：设计是规格；**落地真实代码与本文件的「偏差登记」优先**
> （父代理裁决：「若设计文档与真实代码不一致，以落地真实代码为准，并把偏差登记进本文件」）。
> A/B 的原始行、汇总表、负结果（设计 §6 R1-R7）按设计 §6.4 落在 `docs/m6-bench.md`；
> 本文件在 §M6.7 只放指针与摘要，不复制数字（避免两处口径漂移）。
> 纪律：**未验证 == 未验证**，不得写成通过；每条判据必须能追溯到「命令 + 原始输出」。

## 0. 基座与口径

| 项 | 值 | 采集方式 |
|---|---|---|
| `~/raft-kv-lsm` 起点 | `6aabc27`（设计）← `1463620`（远端基线） | `git log --oneline -2` |
| **lsm 基座（pin）** | **`b1bd050`** | `git -C ~/lsm-kv rev-parse HEAD` |
| lsm pin 的取用方式 | `git -C ~/lsm-kv archive b1bd050 \| tar -x -C /tmp/lsm-pin-b1bd050`（**不读 lsm 活工作区**） | 见 §M6.1 |
| 设计文档写作时的 lsm 引用 | `99c417f`（**已过时**：lsm 已推进到 `b1bd050`） | §M6.0 偏差 M6.0-D1 |
| VM | Ubuntu 22.04.5 / 8 vCPU / 7.7 GiB | `hostname; nproc; free -h` |
| 硬性资源纪律 | 重活（cmake 构建 / 门禁 / 基准 / ASan / TSan）与 lsm 侧串行：跑前先 `ps -eo etimes,args \| grep -E '[c]make --build\|[l]sm_tests\|[b]ench_lsm'` | 每次重活前记录 |

---

## M6.0 前置复核（判据：设计 §0/§2 的每个「文件名 + 行号」可复现）

### 命令与原始输出

```bash
$ cd ~/raft-kv-lsm && wc -l docs/m6-design.md
1243 docs/m6-design.md

$ git log --oneline -1 && git status --short && git rev-parse HEAD
6aabc27 docs(m6): 冻结 M6 设计（把 raft-kv 存储层适配到本仓 lsm 引擎）+ 父代理裁决
<git status --short 无输出 ⇒ 工作树干净>
6aabc27be327600e24dc4aaacc5343dea785c2c8

$ git -C ~/lsm-kv log --oneline -3; git -C ~/lsm-kv tag; git -C ~/lsm-kv status --short
b1bd050 feat(m5): M5.3 微基准、放大探针与三条门禁腿
99c417f feat(m5): M5.1 Bloom filter + M5.2 WriteBatch（含一个真实回归修复）
fa7c328 feat(m4): B01/B02 —— compaction 中途 kill -9 对账与四注入点 SIGKILL；修复 TSan 抓到的元数据竞态
m1-memtable
m2-wal
m3-sstable
m4-compaction
m5-bloom-writebatch
<status 无输出 ⇒ 复核时点 lsm 活工作区干净>

$ ps -eo etimes,args | grep -E '[c]make --build|[l]sm_tests|[b]ench_lsm|[g]\+\+|[c]c1plus'
<无输出 ⇒ 复核时点 lsm 侧没有重活>

$ sed -n '23p;26p;29p;37p;38p;42p;50,52p;54p;57,59p;62,66p' src/raft/log_store.h
  virtual bool load(Term& term, int& votedFor, Index& lastIndex) = 0;
  virtual bool persistMeta(Term term, int votedFor) = 0;
  virtual bool append(const std::vector<LogEntry>& entries) = 0;
  virtual bool appendNoSync(const std::vector<LogEntry>& entries) = 0;
  virtual bool sync() = 0;
  virtual bool truncateSuffix(Index fromIndex) = 0;
  virtual bool truncateSuffixNoSync(Index fromIndex) {
    return truncateSuffix(fromIndex);
  }
  virtual std::vector<LogEntry> slice(Index from, size_t maxEntries,
  virtual Index lastIndex() const = 0;
  virtual Term lastTerm() const = 0;              // kNoTerm when empty
  virtual Term termAt(Index index) const = 0;     // kNoTerm when out of range
  virtual void setBoundary(Index lastIncludedIndex, Term lastIncludedTerm) = 0;
  virtual bool compact(Index upTo, Term termAtUpTo) = 0;
  virtual Index firstIndex() const = 0;  // lastIncludedIndex + 1
  virtual Index lastIncludedIndex() const = 0;
  virtual Term lastIncludedTerm() const = 0;
⇒ 与设计 §0.2 的表**逐行一致**（17 个虚方法；`truncateSuffixNoSync` 默认转调）

$ sed -n '30,34p;105,118p;219,239p;359,363p;393,409p;435,449p;451,463p;513,518p' src/raft/file_log_store.cpp
constexpr size_t kEntryFixedLen = 41;   // 8+8+1+4+4+8+8
constexpr size_t kMetaPayloadLen = 12;  // term(8) + votedFor(4)
constexpr size_t kFrameHeaderLen = 8;   // crc(4) + len(4)
constexpr uint32_t kMaxRecordPayload =
    kEntryFixedLen + 64u * 1024u * 1024u + 64u * 1024u;
Bytes encodeEntry(const LogEntry& e) { ... }          # 设计 §2.2 的位级布局逐字一致
    if (e.index < first) { ... continue; }            # L219-222 边界之下跳过
    if (e.index != first) return false;               # L223-228 前缀丢失 ⇒ 拒绝启动
    } else if (e.index != entries_.back().index + 1) { break; }  # L229-231 空洞当撕裂尾
  if (::ftruncate(logFd_, validEnd) != 0) return false;          # L238-239
bool FileLogStore::truncateSuffixLocked(Index fromIndex, bool flush) {
  if (fromIndex == kNoIndex) return true;
  if (fromIndex <= lastIncluded_) return false;  // cannot cut below boundary
  if (fromIndex > lastIndex() + 1) return false;
std::vector<LogEntry> FileLogStore::slice(...) { ... if (!out.empty() && bytes + sz > maxBytes) break; ... }
void FileLogStore::setBoundary(...) { ... }           # 纯内存
bool FileLogStore::compact(Index upTo, Term termAtUpTo) { ... }  # 整份重写 + rename
Index FileLogStore::firstIndex() const { return lastIncluded_ + 1; }
⇒ 与设计 §0.3 的表逐条一致

$ sed -n '86,87p;94,106p;112p;1013,1018p' src/raft/raft_node.cpp
      // D3: the boundary must be known before load() validates firstIndex().
      log_.setBoundary(snap.lastIncludedIndex, snap.lastIncludedTerm);
    if (!log_.load(t, v, last)) {
      throw std::runtime_error("raft: log load failed (I/O error or torn log)");
    }
  syncedIndex_ = log_.lastIndex();  // everything recovered from disk is durable
  if (!log_.compact(snapIndex, snapTerm)) {
    throw std::runtime_error("raft: log compaction failed");
  }
⇒ 与设计 §0.4 的启动顺序/抛错点一致

$ grep -n 'LOG_BOUND' scripts/raft_snapshot_fault.sh
23:LOG_BOUND=1048576  # 1 MiB: ~2000 small entries fit well under this
117:  if (( sz > LOG_BOUND )); then
118:    echo "FAIL: node$id raft.log=$sz exceeds $LOG_BOUND (not bounded)" >&2; exit 1
121:echo "A) 100000 entries applied, raft.log bounded (<= ${LOG_BOUND}B)"
⇒ 设计 §0.5 写的 L116-119 实际是 **L117-119**（只差 1 行；C6 的改动点不变）
```

### 逐条判据

| 判据 | 状态 | 证据 |
|---|---|---|
| `docs/m6-design.md` 存在且 1243 行 | ✅ 通过 | `wc -l` = 1243 |
| §0.2 `log_store.h` 行号/签名可复现 | ✅ 通过 | 上面的 `sed` 输出逐行一致 |
| §0.3 `file_log_store.cpp` 行号/语义可复现 | ✅ 通过 | 上面的 `sed` 输出逐条一致 |
| §0.4 `raft_node.cpp` 启动顺序与抛错点可复现 | ✅ 通过 | 上面的 `sed` 输出一致 |
| §0.5 `LOG_BOUND` 判据行可复现 | ⚠️ 行号偏差 1 行 | 实测 L117-119，设计写 L116-119 |
| §0.6 lsm 契约可复现 | ⚠️ 需换 sha + 换读法 | lsm HEAD 已是 `b1bd050`；且必须 `git show <sha>:<path>`，不得读活工作区（见 M6.0-D1/D6） |
| §0.7「未验证清单」的 6 条 | ✅ 仍然成立 | 本机（Windows）确无 g++/cmake；性能/切引擎可行性仍未验证 |

### 偏差与裁决登记

| ID | 设计文档写的 | 落地真实代码/环境 | 裁决（工程上最保守/最可验证） | 影响面 |
|---|---|---|---|---|
| **M6.0-D1** | lsm 契约以 `~/lsm-kv @ 99c417f` 为准 | 实测 `HEAD = b1bd050`（`99c417f` 之上有 1 个新提交：M5.3 微基准/放大探针/门禁腿） | M6 全流程以 **`b1bd050`** 为 lsm 基座，并用 `git archive` **pin 到 `/tmp/lsm-pin-b1bd050`** 后链接（父代理建议②）；M6.7 之前重新探测 lsm HEAD，若 R1 修复已落地则重新 pin + **重跑 A/B** | 全部 lsm 侧接口 |
| **M6.0-D2** | 设计 §0.1：「实测未找到 `m5-*` tag（只有 4 个）」 | 实测 tag 为 `m1-memtable/m2-wal/m3-sstable/m4-compaction/`**`m5-bloom-writebatch`** | 以实测为准；D11（父代理给 lsm 打 `m6-raft-integration` tag）按现状口径执行 | 仅文档/父代理 |
| **M6.0-D3** | 设计 §2.2：「日志条目 value = **逐字复用** `file_log_store.cpp` L105-118 的 `encodeEntry()`」 | `encodeEntry/decodeEntry` 定义在 `file_log_store.cpp` 的**匿名 namespace**，外部 TU **无法链接** | 抽出新文件 `src/raft/log_entry_codec.h`（把 `encodeEntry/decodeEntry/encodeMeta/decodeMeta` **逐字搬进**命名空间 `raftkv::raft`），并让 `file_log_store.cpp` 改用**同一份**实现（删除其本地副本）⇒ 仍是「单一实现」，但**改了 C1-C9 之外的文件**（新增 C10） | 代码面；用全量门禁 + M6.2 的「file 引擎磁盘字节 == lsm value 字节」定向用例兜住回归 |
| **M6.0-D4** | 设计 §4-M6.2 的 `LsmLogStore.TwoStoresOnSameDirConflict`：「第二个 `LsmLogStore(dir)` 构造抛异常（D10/LOCK）」 | lsm 的 LOCK 是 `fcntl(F_SETLK)`（`env_posix.cpp` L178-190）⇒ **进程级**；lsm 自己的注释也写「同一进程内二次 Open 不冲突，验证必须用双进程」 | 该用例改为 **fork 双进程**验证：父进程持有 store，子进程 `Open` 必须失败（子进程退出码非 0）；同时保留「同进程二次 Open 成功」的实测行为作为偏差证据 | 仅测试写法 |
| **M6.0-D5** | 设计 §2.5 的 `Stats` 草案含 `log_dir_bytes` | 取目录字节需遍历文件系统；A/B 脚本侧本来就用 `du -sb` | `Stats` **不含** `log_dir_bytes`（避免把 IO 塞进诊断结构）；保留 `wal_tail_truncated_bytes`（经 `dynamic_cast<lsm::PersistentDBImpl*>` 读 `RecoveryStats`，需 include lsm 的内部头 `db_impl.h`） | O2/D8 采集方式 |
| **M6.0-D6** | 设计 §0.6 的采集命令写 `sed -n ... ~/lsm-kv/src/db_impl.cpp`（读活工作区） | 父代理明令：**读 lsm 契约必须用 `git -C ~/lsm-kv show <sha>:<path>`**，活工作区可能被另一代理改动 | 全部 lsm 引用/构建都走 pin 目录或 `git show`；**不对 `~/lsm-kv` 做任何 git 写操作** | 证据可追溯性 |
| **M6.0-D7** | 设计 §0.5 写 `raft_snapshot_fault.sh` 的 `LOG_BOUND` 判据在 L116-119 | 实测 L117-119 | 以实测行号为准（C6 的改动点不变） | 仅文档 |

### 未做 / 未验证（M6.0 时点，逐条）

1. **未做**：任何 `cmake` 配置/构建、任何 gtest/脚本门禁、任何基准 —— M6.0 只做只读复核。
2. **未验证**：R1（`DB::Sync()` 持 `commit_mu_` 做 fsync）在本 VM 上的实际量级；只有设计 §6 R1 的结构性证据（`db_impl.cpp` L802/L817）+ M5.6 在 `FileLogStore` 上的类比数据。
3. **未验证**：lsm `Options` 取值（`write_buffer_size`/`bloom_bits`/`max_open_files`）对 raft 日志负载是否合适（M6.1 起用默认值，M6.7 记录）。
4. **未验证**：同一 `--data-dir` 上从 file 引擎切到 lsm 引擎是否可用（设计 §7 W6 是推理）—— M6 明确不支持，A/B 每格用全新目录。
5. **未验证**：`~/lsm-kv` 的 R1 修复是否/何时落地（复核时点 `b1bd050` 上 `Sync()` 仍持 `commit_mu_`）—— M6.7 前重探。

---

## M6.1 构建接线 + 引擎开关（**零行为变更**）

### 交付

| 文件 | 改动 |
|---|---|
| `src/raft/log_entry_codec.h` | **新增**（C1 之外的 C10，见 M6.0-D3）：`encodeEntry/decodeEntry/encodeMeta/decodeMeta` + `kEntryFixedLen/kMetaPayloadLen`，命名空间 `raftkv::raft` |
| `src/raft/lsm_log_store.h` | **新增**：`LsmLogStore` 完整签名 + 键编码助手 + `Stats`/`all()`/`walTailTruncatedBytes()` |
| `src/raft/lsm_log_store.cpp` | **新增**（本步为**骨架**：ctor/dtor/键编码完整，其余方法 `throw std::logic_error("... not implemented (M6.2)")`；`setBoundary` 是 no-op，让失败点稳定落在 `load()`） |
| `CMakeLists.txt` | `src/raft/lsm_log_store.cpp` 入 `raftkv_raft`；新增 `RAFTKV_LSM_DIR` cache 变量 + `add_subdirectory(... EXCLUDE_FROM_ALL)` + `target_link_libraries(raftkv_raft PUBLIC lsm)` + `RAFTK_HAVE_LSM=1`；目录不存在只 `WARNING`，**不定义** `RAFTK_HAVE_LSM` |
| `src/main_raft_node.cpp` | `--log-engine=file\|lsm` + `RAFTKV_LOG_ENGINE`（**默认 file**）；未知值 ⇒ stderr + exit 2；store 改为 `std::unique_ptr<LogStore>` |
| `src/raft/file_log_store.cpp` | 删除本地 `putU64/getU64/encodeEntry/decodeEntry` 与两个常量，改用共享头（**纯搬运，字节布局不变**） |

### lsm 基座 pin（父代理建议②）

```bash
$ rm -rf /tmp/lsm-pin-b1bd050 && mkdir -p /tmp/lsm-pin-b1bd050
$ git -C ~/lsm-kv archive b1bd050 | tar -x -C /tmp/lsm-pin-b1bd050
$ git -C ~/lsm-kv rev-parse b1bd050
b1bd0503979604683b064b553b06a405d835581f
$ ls /tmp/lsm-pin-b1bd050 | head
bench
CMakeLists.txt
docs
scripts
src
tests
```
⇒ **M6 所用的 lsm 基座 sha = `b1bd0503979604683b064b553b06a405d835581f`**（不读 `~/lsm-kv` 活工作区）。

### 构建（含 lsm；重活前先探针，探测结果 `none`）

```bash
$ ps -eo etimes,args | grep -E '[c]make --build|[l]sm_tests|[b]ench_lsm|[g]\+\+|[c]c1plus'
<none>
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAFTKV_LSM_DIR=/tmp/lsm-pin-b1bd050 2>&1 | tail -4
-- Found Threads: TRUE
-- M6: lsm engine enabled from /tmp/lsm-pin-b1bd050
-- Configuring done
-- Generating done
-- Build files have been written to: /home/tengyujie/raft-kv-lsm/build
$ cmake --build build -j8      # 后台化后轮询（日志 /tmp/m61-build.log）
[100%] Linking CXX executable bin/raftkv_raft_tests
[100%] Built target raftkv_raft_tests
$ grep -c 'warning:' /tmp/m61-build.log
0
```

### 判据 1：默认路径（file）**零行为变更** —— 全量单测

```bash
$ ./build/bin/raftkv_raft_tests
[==========] 94 tests from 15 test suites ran. (43295 ms total)
[  PASSED  ] 94 tests.
```
（与 M6 起点同为 94 条用例，**一条未删/未改/未禁用**；`FAILED` 计数 0。）

### 判据 2：`--log-engine=lsm`（未编译进 lsm 时）**明确报错退出，绝不静默降级**

```bash
$ cmake -S . -B build-nolsm -DCMAKE_BUILD_TYPE=Release -DRAFTKV_LSM_DIR=/nonexistent-lsm 2>&1 | grep -i 'M6:'
  M6: lsm source not found at /nonexistent-lsm; --log-engine=lsm will fail loudly
$ cmake --build build-nolsm --target raftkv_raft_node -j8   # 日志 /tmp/m61-nolsm.log，warning 计数 0
$ ./build-nolsm/bin/raftkv_raft_node --id 1 --port 19993 --peers "1=127.0.0.1:19993" \
    --data-dir /tmp/m6-smoke-nolsm --log-engine=lsm; echo "exit=$?"
fatal: LsmLogStore: this build was configured without lsm support (no lsm source tree at configure time; re-run cmake with -DRAFTKV_LSM_DIR=<pinned lsm tree>)
exit=1
```

### 判据 3：`--log-engine=lsm`（编译进 lsm 后）能启动（进入 ctor），随后在 `load()` 阶段明确失败

```bash
$ rm -rf /tmp/m6-smoke-lsm
$ ./build/bin/raftkv_raft_node --id 1 --port 19991 --peers "1=127.0.0.1:19991" \
    --data-dir /tmp/m6-smoke-lsm --log-engine=lsm; echo "exit=$?"
[raftkv-node] log-engine=lsm
[raftkv-node] transport=sync
fatal: LsmLogStore::load: not implemented (M6.2)
exit=1
$ ls -la /tmp/m6-smoke-lsm/raft-lsm
-rw-r--r-- 1 tengyujie tengyujie    0 10月  1 04:30 000001.log
-rw-r--r-- 1 tengyujie tengyujie    0 10月  1 04:30 LOCK
```
⇒ ctor 真的 `Open` 了 lsm DB（`LOCK` + WAL `000001.log`），失败点确实在 `load()`。

### 判据 4（附加，防拼写错误静默降级）：未知引擎值

```bash
$ ./build/bin/raftkv_raft_node ... --log-engine=bogus; echo "exit=$?"
unknown --log-engine: bogus (expected file|lsm)
exit=2
```

### 判据 5（附加）：默认 file 引擎仍可用（真实起进程）

```bash
$ timeout 2 ./build/bin/raftkv_raft_node --id 1 --port 19992 --peers "1=127.0.0.1:19992" \
    --data-dir /tmp/m6-smoke-file; echo "exit=$?"
[raftkv-node] log-engine=file
[raftkv-node] transport=sync
[raftkv-node] id=1 listening on 0.0.0.0:19992 data-dir=/tmp/m6-smoke-file
[raftkv-node] id=1 shutdown
exit=124          # timeout 主动杀，非节点失败
$ ls /tmp/m6-smoke-file/raft
meta.dat  raft.log
```

### 逐条判据状态

| 判据（设计 §4-M6.1） | 状态 | 证据 |
|---|---|---|
| 默认路径（file）逐字节不变：`raftkv_raft_tests` 全绿 | ✅ 通过 | 94/94 PASSED，0 FAILED |
| 两个构建均 0 warning（仓库纪律） | ✅ 通过 | `grep -c 'warning:'` = 0（build / build-nolsm） |
| `--log-engine=lsm` 未编译进 lsm 时报错退出、stderr 明确、绝不静默降级 | ✅ 通过 | exit=1 + `fatal: LsmLogStore: this build was configured without lsm support...` |
| `--log-engine=lsm` 编译进 lsm 后能启动（进 ctor），随后 `load()` 阶段明确错误退出 | ✅ 通过 | `raft-lsm/LOCK`+`000001.log` 已创建；`fatal: LsmLogStore::load: not implemented (M6.2)`；exit=1 |
| 未知引擎值拒绝 | ✅ 通过（附加判据） | exit=2 + `unknown --log-engine: bogus` |
| 「逐字节不变」的**位级**证明（file 引擎磁盘字节 == 共享 codec 输出） | ⏳ 待做（M6.2） | 本步只用「全量单测 + 纯搬运 diff」兜底；M6.2 加定向字节对拍用例 |

### 偏差登记（M6.1）

| ID | 设计写的 | 落地 | 裁决 |
|---|---|---|---|
| **M6.1-D1** | 设计 §4-M6.1 的证据命令用 `-DRAFTKV_LSM_DIR="$HOME/lsm-kv"`（活工作区） | 实测链接的是 `git archive b1bd050` 出来的 `/tmp/lsm-pin-b1bd050` | 按父代理建议②执行（活工作区可能正被另一代理改动）；CMake 默认值仍是 `$ENV{HOME}/lsm-kv`，pin 只在命令行覆盖 |
| **M6.1-D2** | 设计 §4-M6.1：「全部方法返回 false / 抛 logic_error」 | 骨架实现里 `setBoundary` 是 **no-op**（不是抛异常） | 必须如此：`RaftNode` ctor 在 `load()` **之前**调 `setBoundary`（D3 顺序），若它抛异常，失败点就不是判据要求的 `load()` |
| **M6.1-D3** | 设计未提 | `build/` 与 `build-nolsm/` 两个构建目录（后者只用于验证「无 lsm 也不静默降级」） | 登记；`build-nolsm` 不进提交（.gitignore 已覆盖 `build*`？M6.1 提交前确认） |

### 未做 / 未验证（M6.1 时点）

1. **未做**：`LsmLogStore` 的全部语义（load/append/truncate/compact/slice）—— 骨架期显式抛 `logic_error`，M6.2 落地。
2. **未做**：`tests/raft_lsm_log_test.cpp`（M6.2）。
3. **未验证**：lsm 引擎在真实 raft 集群下的任何行为（M6.5/M6.6/M6.7）。
4. **未验证**：`raft-lsm` 目录的空间/恢复代价（M6.7 的 O2/O3/D3/D4）。
