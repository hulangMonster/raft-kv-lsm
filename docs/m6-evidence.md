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

---

## M6.2 LsmLogStore 语义单测（**不接 Raft**）

### 交付

| 文件 | 改动 |
|---|---|
| `src/raft/lsm_log_store.cpp` | M6.1 骨架 → **完整语义实现**（load/persistMeta/append/appendNoSync/sync/truncateSuffix[NoSync]/slice/访问器/setBoundary/compact/诊断） |
| `src/raft/lsm_log_store.h` | 增 `firstIndexLocked()`（持锁内部用，避免 non-recursive mutex 自死锁）与 `deleteRangeLocked()` |
| `tests/raft_lsm_log_test.cpp` | **新增 20 条用例**（未配置 lsm 的构建下退化为 1 条真断言的否定用例，不是 SKIP） |
| `CMakeLists.txt` | `tests/raft_lsm_log_test.cpp` 入 `raftkv_raft_tests` |

### 实现要点（与设计草案的差异都在下面的偏差登记里）

* **append 的三阶段**：① 规划（不改内存、不碰 DB；失败 ⇒ 原样返回，**不产生半应用状态**）；
  ② 写（冲突的 `Delete` 与后续 `Put` 在**同一个** WriteBatch 里 ⇒ 一次原子提交）；
  ③ 提交内存态（只在 `DB::Write` 返回 kOk 之后）。
* **分块**：`WriteBatch` 上限（`count ≤ 1<<20`、`ByteSize ≤ 64 MiB`，`write_batch.h` L31-32）⇒ 每批 ≤ 4096 op / 8 MiB。
* **不绕过 R1**：`sync()` 不持本类 `mu_`，但 lsm 内部 `DB::Sync()` 仍持 `commit_mu_` 做 fsync（设计 §6 R1）——
  M6 不改 lsm，也不为绕过它改设计。
* **失败即毒化**：任一 `DB::Write`/`DB::Sync` 非 ok ⇒ `poisoned_ = true`，后续变更一律 `false`。

### 判据与原始输出

```bash
$ cmake --build build -j8            # 日志 /tmp/m62-build3.log
[100%] Built target raftkv_raft_tests
$ grep -c 'warning:' /tmp/m62-build3.log
0

$ ./build/bin/raftkv_raft_tests --gtest_filter='LsmLogStore.*'
[ RUN/OK] LsmLogStore.KeyEncodingIsBytewiseIndexOrdered
[ RUN/OK] LsmLogStore.MetaKeyIsOutsideLogRange
[ RUN/OK] LsmLogStore.RestartRestoresMetaAndLog
[ RUN/OK] LsmLogStore.AppendIsDurableAndVisible
[ RUN/OK] LsmLogStore.AppendNoSyncIsVisibleButSurvivesSync
[ RUN/OK] LsmLogStore.IdempotentAppendSameIndexSameTerm
[ RUN/OK] LsmLogStore.ConflictTruncateThenAppend
[ RUN/OK] LsmLogStore.TruncateSuffixBoundaries
[ RUN/OK] LsmLogStore.TruncateNoSyncThenSyncIsDurable
[ RUN/OK] LsmLogStore.TruncateNoSyncIntoKillDashNineKeepsDeletionInPageCache
[ RUN/OK] LsmLogStore.SliceClampsAndHonoursLimits
[ RUN/OK] LsmLogStore.CompactDropsPrefixAndSurvivesRestart
[ RUN/OK] LsmLogStore.PrefixGoneWithoutSnapshotRefusesLoad
[ RUN/OK] LsmLogStore.LoadSkipsEntriesBelowBoundary
[ RUN/OK] LsmLogStore.EmptyLogWithBoundary
[ RUN/OK] LsmLogStore.MetaPersistIsAtomicAndDurable
[ RUN/OK] LsmLogStore.SecondProcessOnSameDirIsRejectedByLock
[ RUN/OK] LsmLogStore.LoadRefusesGapInTheMiddle
[ RUN/OK] LsmLogStore.TornWalTailIsTruncatedByOpenAndCounted
[ RUN/OK] LsmLogStore.ValueBytesMatchFileLogStoreOnDisk
[==========] 20 tests from 1 test suite ran. (933 ms total)
[  PASSED  ] 20 tests.

$ ./build/bin/raftkv_raft_tests --gtest_filter='FileLogStore.*:MemoryLogStore.*'
[==========] 2 tests from 1 test suite ran. (55 ms total)
[  PASSED  ] 2 tests.

$ ./build/bin/raftkv_raft_tests          # 全量
[==========] 114 tests from 16 test suites ran. (43984 ms total)
[  PASSED  ] 114 tests.

$ git diff --stat -- tests/
<空>   # ⇒ FileLogStore/MemoryLogStore 的既有用例一行未改（D15 同口径）

$ cmake --build build-nolsm --target raftkv_raft_tests -j8   # 未配置 lsm 的构建
$ ./build-nolsm/bin/raftkv_raft_tests --gtest_filter='LsmLogStore.*'
[ RUN/OK ] LsmLogStore.WithoutLsmSupportConstructionFailsLoudly
[  PASSED  ] 1 test.
```

**失败-修复留档（不得省略）**：首次运行时
`LsmLogStore.TruncateNoSyncIntoKillDashNineKeepsDeletionInPageCache` 失败——
子进程退出码 3，即 `truncateSuffixNoSync(4)` 返回 false。根因：新开的 `LsmLogStore` 内存尾还是
`lastIndex_=0`（内存态来自 `load()`），`4 > lastIndex_+1` 触发 E3 越界。修复：子进程先
`load()`（与 `RaftNode` 启动顺序一致）再截断。修复后 20/20。

### 逐条用例 ↔ 设计论证映射

| 用例 | 设计出处 | 判据 |
|---|---|---|
| `KeyEncodingIsBytewiseIndexOrdered` | §2.1 | 9B 定宽大端 key 的字节序 == 索引序；decode 往返 |
| `MetaKeyIsOutsideLogRange` | §2.1 | `0x02"meta"` 严格大于所有日志 key；`isLogKey` 拒绝它 |
| `RestartRestoresMetaAndLog` | §2.4.1 / `FileLogStore.RestartRestoresMetaAndLog` | 同 8 条断言 |
| `AppendIsDurableAndVisible` | §2.4.3 / L28-33 | 返回即 durable 且立刻可见 |
| `AppendNoSyncIsVisibleButSurvivesSync` | §2.4.4 / L31-33 | 可见但不 fsync；`sync()` 后重启仍在 |
| `IdempotentAppendSameIndexSameTerm` | §2.4.4 / L320-322 | 重复批不报错、不重复 |
| `ConflictTruncateThenAppend` | §2.4.4/E5 | 同 index 异 term ⇒ 覆盖；**重启后是新值**；`truncated_entries == 2` |
| `TruncateSuffixBoundaries` | §2.4.6 E1-E4 | `kNoIndex`⇒true；`>lastIndex+1`⇒false；`<=lastIncluded`⇒false；`==lastIndex+1`⇒true |
| `TruncateNoSyncThenSyncIsDurable` | §2.4.6 E7 | 同批 `sync()` 覆盖截断 |
| `TruncateNoSyncIntoKillDashNineKeepsDeletionInPageCache` | §2.4.6 崩溃窗口（**改口径**，见 M6.2-D2） | kill -9 后截断仍可见（页缓存存活） |
| `SliceClampsAndHonoursLimits` | §2.4.7 / L399-404 | clamp；`maxEntries`；`maxBytes` 至少 1 条；越界/0 条 |
| `CompactDropsPrefixAndSurvivesRestart` | §2.4.9 + §2.4.1 R2 | tombstone 让前缀在用户视图消失；重启 + `setBoundary` 后可读 |
| `PrefixGoneWithoutSnapshotRefusesLoad` | §2.4.1 R4 / L223-228 | 无快照 ⇒ 拒绝启动 |
| `LoadSkipsEntriesBelowBoundary` | §2.4.1 R3 / L219-222 | 边界之下残留被跳过，首条 == first |
| `EmptyLogWithBoundary` | §2.4.1 R5 | 空日志 + 边界 ⇒ `lastIndex==I`、`lastTerm==T`、`firstIndex==I+1` |
| `MetaPersistIsAtomicAndDurable` | §2.4.2 | 重启后 term/votedFor 一致（含 `-1` 的位模式） |
| `SecondProcessOnSameDirIsRejectedByLock` | D10 / §7 W7（**fork 双进程**，见 M6.0-D4） | 第二个**进程**的 Open 被 LOCK 拒绝 |
| `LoadRefusesGapInTheMiddle` | D5 | 直连 lsm 删中间 key 造空洞 ⇒ `load()` false |
| `TornWalTailIsTruncatedByOpenAndCounted` | §3.3 对齐点 1 / O8-D8 | 尾部残骸由 `DB::Open` 截断，`walTailTruncatedBytes() > 0`，前缀完好 |
| `ValueBytesMatchFileLogStoreOnDisk` | §2.2 / M6.0-D3 | `raft.log` 的 payload == `encodeEntry()` == lsm 的 value，**逐字节** |

### 偏差登记（M6.2）

| ID | 设计写的 | 落地 | 裁决 |
|---|---|---|---|
| **M6.2-D1** | §2.4.9 的 `setBoundary` 草案：先更新 `lastIncluded_`，再用 `firstIndex()+drop <= lastIncludedIndex` 丢弃 `terms_` 前缀 | 该写法会**漏掉正好等于新边界的条目**（`firstIndex()` 已是新值 ⇒ 循环首轮即不成立） | 按 `FileLogStore::setBoundary`（L437-441）的绝对索引语义实现：先取 `oldFirst = lastIncluded_+1`，再按 `oldFirst+drop <= newBoundary` 丢弃。`EmptyLogWithBoundary`/`LoadSkipsEntriesBelowBoundary` 钉住该行为 |
| **M6.2-D2** | §4-M6.2 的 `TruncateThenCrashResurrectSuffix`：只做 `truncateSuffixNoSync` 不 `sync()` ⇒「后缀复活」（文档化的反面） | 实测 **kill -9 后截断仍然可见**（页缓存存活）——与设计 §3.1 矩阵「appendNoSync 在本 VM 实测必存活（page cache）」是同一现象。「复活」只可能发生在掉电 | 用例改名 `TruncateNoSyncIntoKillDashNineKeepsDeletionInPageCache` 并断言**实测行为**（fork 子进程截断后 `_exit`，不跑析构）；「掉电语义未覆盖」写入未验证清单 |
| **M6.2-D3** | §2.4.4 的 `appendNoSync` 草案：边遍历边改内存（与 FileLogStore 同形） | 实现改为三阶段（规划/写/提交内存），且冲突的 Delete+Put 同批原子 | 更强：校验失败不产生半应用状态；且「截断后追加」是一次原子提交（设计 §2.4.4 自己也把这条列为「更强的保证」） |
| **M6.2-D4** | §2.4.4 只提「必须分批（≤ kMaxCount / 64MiB）」 | 落地上界为 4096 op / 8 MiB 每批 | 留出余量；分块跨越失败时毒化（fail-stop），不再假装成功 |
| **M6.2-D5** | 资源纪律：重活与 lsm 侧串行 | M6.2 期间 lsm 侧在同一 VM 上跑 `cmake --build`/`lsm_tests`（正在做 R1/SyncIsolation 修复），与本步的**增量构建**和 44s 的 gtest 全量有重叠 | 如实登记；**未**在重叠窗口跑任何计时敏感基准（基准在 M6.7，届时按纪律重探空闲）；结论不受影响（本步全是功能断言，不含计时） |

### 未做 / 未验证（M6.2 时点）

1. **未做**：`tests/raft_restart_test.cpp` 参数化（M6.3）、Disk fixture 参数化（M6.4）、e2e 脚本双引擎（M6.5）、故障注入（M6.6）、A/B（M6.7）—— 本步**不接 Raft**。
2. **未验证（覆盖缺口）**：`TruncateNoSync` 的**掉电**（页缓存丢失）语义 —— 测试装置无法构造（需要假文件系统/掉电模型）。设计 §4-M6.3 的 `TruncatesTornTail` 在 lsm 侧同样没有直接注入点；lsm 侧的等价证据目前是 `TornWalTailIsTruncatedByOpenAndCounted`（尾部残骸）+ M6.6 的真实 `kill -9`。
3. **未验证**：LsmLogStore 在并发（多线程 append/sync/slice 交错）下的正确性 —— 本步单测是单线程的；TSan 覆盖在 M6.8/H12（设计 §5.2）与 M6.5/M6.6 的端到端。
4. **未验证**：`slice()` 的解码失败路径（`slice_decode_errors`）与毒化路径 —— 未构造注入点（需要介质损坏）。
5. **未验证**：性能（每次 `slice()` 建迭代器的开销 R2、`truncateSuffix` 的写放大 R3）—— M6.7。

---

## M6.3 `raft_restart_test` 参数化（磁盘用例跑两个引擎）

### 交付

* `tests/raft_restart_test.cpp`：用例体抽成 `template <typename Store> RestartRestoresMetaAndLogBody()`；
  `FileLogStore.RestartRestoresMetaAndLog` **名字不变**、断言文本不变（现在是 `...Body<FileLogStore>()`）；
  新增孪生 `LsmLogStore.RestartRestoresMetaAndLog`、`LsmLogStore.TruncatesTornTail`。
* `tests/raft_lsm_log_test.cpp`：删除两条被取代的用例（见 M6.3-D2）。

### 判据与原始输出

```bash
$ cmake --build build -j8 && cmake --build build -j8 2>&1 | grep -c 'warning:'
[100%] Built target raftkv_raft_tests
0

$ ./build/bin/raftkv_raft_tests --gtest_filter='*RestartRestoresMetaAndLog:*TruncatesTornTail'
[==========] Running 4 tests from 2 test suites.
[----------] 2 tests from FileLogStore
[ RUN      ] FileLogStore.RestartRestoresMetaAndLog
[       OK ] FileLogStore.RestartRestoresMetaAndLog (47 ms)
[ RUN      ] FileLogStore.TruncatesTornTail
[       OK ] FileLogStore.TruncatesTornTail (17 ms)
[----------] 2 tests from LsmLogStore
[ RUN      ] LsmLogStore.RestartRestoresMetaAndLog
[       OK ] LsmLogStore.RestartRestoresMetaAndLog (67 ms)
[ RUN      ] LsmLogStore.TruncatesTornTail
[       OK ] LsmLogStore.TruncatesTornTail (53 ms)
[==========] 4 tests from 2 test suites ran. (190 ms total)
[  PASSED  ] 4 tests.

$ ./build/bin/raftkv_raft_tests            # 全量（与 M6.2 同量：114 条）
[==========] 114 tests from 16 test suites ran. (44690 ms total)
[  PASSED  ] 114 tests.

$ grep -c 'RestartRestoresMetaAndLogBody<FileLogStore>' tests/raft_restart_test.cpp   # 1
$ grep -c 'RestartRestoresMetaAndLogBody<LsmLogStore>'  tests/raft_restart_test.cpp   # 1
⇒ 两个引擎跑的是**同一个函数体** ⇒ 断言文本逐字相同（不是「人工保持一致」）
```

### 偏差登记（M6.3）

| ID | 设计写的 | 落地 | 裁决 |
|---|---|---|---|
| **M6.3-D1** | 设计 §4-M6.3 / §7 W9：「`TruncatesTornTail` 在 lsm 引擎下**没有直接的等价注入点**……若无法构造，如实标注『未覆盖』」 | **该判断被实测证伪**：`LsmLogStore::TruncatesTornTail` 用与 file 引擎**相同的注入手法**（在活动 WAL 尾部追加同一串垃圾）就能构造，且 `load()` 正确截断到 lastIndex=1，外加 `walTailTruncatedBytes() > 0` | 提供**真正的孪生**，不标注「未覆盖」。撕裂尾由 `DB::Open` 的恢复路径处理（`m2-design` §5.3）—— 设计 §3.3 的「对齐点 1」由此获得实测证据 |
| **M6.3-D2** | 设计 §4-M6.2 的用例清单含 `LsmLogStore.RestartRestoresMetaAndLog`（M6.2 已交付） | M6.3 把同场景搬到 `raft_restart_test.cpp` 与 file 引擎共享用例体 ⇒ 会产生**同名重复定义**（链接冲突） | 删除 M6.2 的那条拷贝（以及被取代的 `TornWalTailIsTruncatedByOpenAndCounted`）。这是**合并而非削减**：同一场景现在由「一份 body + 两个引擎」覆盖，断言更强（多断言 `walTailTruncatedBytes > 0`），全量用例数 114 未变。M6.2 报告中「20 条」在 M6.3 后变为 18 条（+ M6.3 的 2 条孪生） |
| **M6.3-D3** | 资源纪律 | 本步的增量构建与 44.7s 全量 gtest 期间，lsm 侧正在构建 `sync_isolation_probe`（R1 修复的工作） | 如实登记；本步全是功能断言、无计时，不受影响 |

### 未做 / 未验证（M6.3 时点）

1. **未做**：Disk fixture 参数化（M6.4）、e2e（M6.5）、故障注入（M6.6）、A/B（M6.7）。
2. **未验证**：`RaftSnapshotDisk.*` / `RaftMembershipDisk.*` 在 lsm 引擎下的行为（M6.4）。
3. **未验证**：`raft_restart_test` 之外，`LsmLogStore` 与 `RaftNode` 的**真实接线**（本步仍是 store 级用例；M6.4/M6.5 才接集群）。

---

## M6.4 `RaftSnapshotDisk.*` / `RaftMembershipDisk.*` fixture 参数化

### 交付

| 文件 | 改动 |
|---|---|
| `tests/raft_snapshot_test.cpp` | 新增 `enum class DiskEngine{kFile,kLsm}` + `makeDiskLog(eng,dir)` + `tornTailPath(eng,dir)`；`DiskNode::log` 改为 `std::unique_ptr<LogStore>`；4 个场景抽成共享 body（3 个 store 级 + 1 个 3 节点集群），各生成 file/lsm 两条用例 |
| `tests/raft_membership_test.cpp` | 同上（`DiskNode::log` → `LogStore`；`buildDiskCluster(..., DiskEngine eng = kFile)`）；B1/B2/B3 抽成共享 body + lsm 孪生 |

参数化机制：**运行时选引擎**（`makeDiskLog`）而不是 typed-test —— 既有用例名一字不改，两个引擎跑
**同一个函数体**（断言文本逐字一致，不是两份拷贝）。确定性内存集群（`makeCluster` /
`makeMembershipCluster`）**一行未动**（D15）。

### 判据与原始输出

```bash
$ touch tests/raft_snapshot_test.cpp tests/raft_membership_test.cpp && cmake --build build -j8
$ grep -c 'warning:' /tmp/m64-build3.log
0

$ ./build/bin/raftkv_raft_tests --gtest_filter='*Disk*' --gtest_list_tests
RaftSnapshotDisk.  RestartLoadsSnapshotThenReplaysTail / TornSnapshotDiscarded / TornTailAfterCompact /
                   SnapshotAndLogCombinedRecovery / FileStoreClusterKeepsCommittingAfterCompaction /
                   LsmStoreClusterKeepsCommittingAfterCompaction / ConcurrentSaveAndInstallKeepNewestSnapshot
RaftSnapshotDiskLsm. RestartLoadsSnapshotThenReplaysTail / TornTailAfterCompact / SnapshotAndLogCombinedRecovery
RaftMembershipDisk.  B1_ConfigPersistsAcrossRestart / B2_ConfigCompactedThenRecoveredFromSnapshot /
                     B3_CrashDuringConfigChangeKeepsConsistentTopology / B4_Rks1V1SnapshotStillLoads
RaftMembershipDiskLsm. B1_ConfigPersistsAcrossRestart / B2_ConfigCompactedThenRecoveredFromSnapshot /
                       B3_CrashDuringConfigChangeKeepsConsistentTopology
LsmLogStore. ValueBytesMatchFileLogStoreOnDisk      # 名字里带 Disk，被 *Disk* 命中（无害）

$ ./build/bin/raftkv_raft_tests --gtest_filter='*Disk*'
[==========] 18 tests from 5 test suites ran. (8237 ms total)
[  PASSED  ] 18 tests.

$ ./build/bin/raftkv_raft_tests          # 全量
[==========] 121 tests from 18 test suites ran. (48600 ms total)
[  PASSED  ] 121 tests.
```

**失败-修复留档**：M6.4 首轮构建出现 1 条 `-Wall` warning ——
`tests/raft_membership_test.cpp:293: warning: 'tornTailPath' defined but not used`（membership 侧不注入撕裂尾）。
删除该文件里多余的 helper（`/tmp/fix_m64_warning.py`），重建后 `grep -c 'warning:'` = 0。**没有**用
`(void)`/`[[maybe_unused]]` 之类把 warning 藏起来。

### 逐条判据状态（设计 §4-M6.4）

| 判据 | 状态 | 证据 |
|---|---|---|
| Disk 系列参数化后两个引擎都绿 | ✅ 通过 | `*Disk*` 18/18 PASSED |
| 既有 `RaftSnapshotDisk.*` / `RaftMembershipDisk.*` 用例名保留、断言不变 | ✅ 通过 | file 侧 11 条名字与断言文本原样（body 抽出来但文本未改） |
| 全量不退化（M2-M5 门禁） | ✅ 通过 | 121/121（114 + 7 条 lsm 孪生） |
| 0 warning | ✅ 通过（修过一条，见上） | `grep -c 'warning:'` = 0 |
| 设计预期的「8 用例 × 2 = 16 条」 | ⚠️ 口径偏差 | 见 M6.4-D1 |
| `TornTailAfterCompact` 在 lsm 侧「可能不适用」 | ⚠️ 被证伪 | 见 M6.4-D2 |

### 偏差登记（M6.4）

| ID | 设计写的 | 落地 | 裁决 |
|---|---|---|---|
| **M6.4-D1** | §4-M6.4：「8 个 Disk 用例 × 2 引擎 = 16 条全绿」 | 实测 Disk 系列共 10 个用例名，其中**只有 7 个真的用 LogStore**（`TornSnapshotDiscarded`、`ConcurrentSaveAndInstallKeepNewestSnapshot`、`B4_Rks1V1SnapshotStillLoads` 只用 `FileSnapshotStore`，与「日志引擎」无关）⇒ 7×2 = **14 条引擎参数化**用例；`*Disk*` 过滤命中 18 条（含名字里带 Disk 的 `LsmLogStore.ValueBytesMatchFileLogStoreOnDisk`） | 以「真的涉及 LogStore」为准参数化；快照存储/codec 类用例不强行参数化（参数化它们是假覆盖）。**登记为口径偏差** |
| **M6.4-D2** | §4-M6.4：`RaftSnapshotDisk.TornTailAfterCompact`（L551)「前者可能**不适用**，如实标注」 | **不适用这一判断被实测证伪**：同一 body + 引擎相关的注入路径（`tornTailPath(eng, dir)`）在 lsm 侧直接通过（compact 后写 tombstone，尾部垃圾由 `DB::Open` 截断） | 提供真正的孪生，不标注「不适用」 |
| **M6.4-D3** | §4-M6.4：`FileStoreClusterKeepsCommittingAfterCompaction`（L866）「名字里就带 FileStore，语义在 lsm 侧需要重新表述」 | 保留原用例名（file），新增同 suite 的 `LsmStoreClusterKeepsCommittingAfterCompaction`（lsm），两者共用同一 body | 不改既有名字（避免破坏历史过滤/引用），新增孪生名更明确 |
| **M6.4-D4** | 设计未规定参数化形式 | 采用**运行时** `DiskEngine` + `makeDiskLog()`，而不是 typed-test fixture | 既有用例名保持不变、diff 最小；两个引擎仍跑同一份断言文本 |
| **M6.4-D5** | 资源纪律 | 本步构建（两个测试 TU + link）与 48.6s 全量期间，lsm 侧在跑 `sync_isolation_probe` 相关构建 | 如实登记；无计时断言 |

### 未做 / 未验证（M6.4 时点）

1. **未做**：e2e 脚本双引擎（M6.5）、故障注入 + `LOG_BOUND` 口径（M6.6）、A/B（M6.7）。
2. **未验证**：真实多进程集群（`main_raft_node` + 脚本）在 lsm 引擎下能否跑通 —— 本步仍是同进程的 MemoryTransport 集群。
3. **未验证**：`RaftMembershipDiskLsm.B2` 的块级细节（配置随快照持久化在 lsm 侧同样成立，已由断言覆盖；但**压缩后物理空间回收**未观测 → M6.7 的 D3/R4）。
4. **未验证**：并发（多线程 save/install 与 lsm 引擎的组合）—— `ConcurrentSaveAndInstallKeepNewestSnapshot` 只用快照存储，未参数化。

---

## M6.5 端到端脚本支持双引擎

### 交付

`scripts/raft_e2e.sh` / `raft_snapshot_e2e.sh` / `raft_membership_e2e.sh`：

1. 新增 `--log-engine file|lsm`（`--log-engine=lsm` 或 `--log-engine lsm`），默认 `file`；未知值 ⇒ stderr + exit 2。
2. `start_node()` 把引擎**同时**用 CLI 参数（`--log-engine "$LOG_ENGINE"`）与 `RAFTKV_LOG_ENGINE="$LOG_ENGINE"` 环境变量交给 node（C7 的 env 透传 + 一条可端到端验证的通道）。
3. 脚本开头打印 `engine=<file|lsm>`（进证据）。

### 判据与原始输出

```bash
$ for eng in file lsm; do for s in raft_e2e raft_snapshot_e2e raft_membership_e2e; do
    bash scripts/$s.sh --log-engine "$eng"; done; done
=== raft_e2e engine=file exit=0 ===                     engine=file   raft_e2e: PASS
=== raft_snapshot_e2e engine=file exit=0 ===            engine=file   raft_snapshot_e2e: PASS
=== raft_membership_e2e engine=file exit=0 ===          engine=file   raft_membership_e2e: PASS
=== raft_e2e engine=lsm exit=0 ===                      engine=lsm    raft_e2e: PASS
=== raft_snapshot_e2e engine=lsm exit=0 ===             engine=lsm    raft_snapshot_e2e: PASS
=== raft_membership_e2e engine=lsm exit=0 ===           engine=lsm    raft_membership_e2e: PASS
```

「真的用了 lsm 引擎」的运行时证据（脚本运行期间采样 node 进程与数据目录）：

```bash
$ bash /tmp/m65_proof.sh          # run `raft_e2e.sh --log-engine lsm` 并在 t=2s 采样
--- 运行中的 node 进程 ---
.../build/bin/raftkv_raft_node --id 2 --port 19435 --peers ... --data-dir /tmp/tmp.WuPfGmrn7O/node2 --log-engine lsm
--- 该 node 的数据目录内容（lsm 引擎应为 raft-lsm/）---
raft
raft-lsm
--- 它的 stderr 日志里的引擎行 ---
[raftkv-node] log-engine=lsm
[raftkv-node] log-engine=lsm
[raftkv-node] log-engine=lsm
--- 脚本结果 ---
engine=lsm
raft_e2e: PASS
```
⇒ `--log-engine lsm` 真的到了 node 进程（cmdline + stderr 双证据），并真的创建了 `raft-lsm/`。

### 逐条判据状态（设计 §4-M6.5）

| 判据 | 状态 | 证据 |
|---|---|---|
| 三个脚本在 `file` 与 `lsm` 下都打印各自 PASS 行 | ✅ 通过 | 6 条 PASS（上面） |
| node 真的用了对应引擎 | ✅ 通过 | 运行时采样：cmdline `--log-engine lsm` + `raft-lsm/` + stderr |
| `verify` 的 `missing 0`（H8） | ✅ 通过（断言在脚本内部） | `raft_membership_e2e.sh` 的 `fill_and_verify` 用 `cli ... verify | grep -q 'missing 0'`，失败即 `exit 1`；**原始 verify 行**的逐格打印落在 M6.7 的 `bench_m6_ab.sh`（见 M6.5-D2） |
| 无多数派时写不得返回 OK（H10） | ✅ 通过（既有断言未改） | `raft_e2e.sh` 末尾的多数派检查在 lsm 臂同样通过（脚本 PASS） |
| 默认（不传参）行为不变 | ✅ 通过 | 默认 `file`；未传参时 `engine=file`（A/B 的 `file` 臂） |

### 偏差登记（M6.5）

| ID | 设计写的 | 落地 | 裁决 |
|---|---|---|---|
| **M6.5-D1** | C7：「把 `RAFTKV_LOG_ENGINE` 透传给 node 进程」（脚本靠环境变量选引擎） | 本 VM 的自动化通道上发现：当一条远程命令含**多条语句**时，`VAR=value cmd` 的前缀赋值与 `export` 都不会到达子进程（同一命令改为单条语句 / 嵌套 `bash -c "..."` 时正常）。实测：<br>`ssh vm 'RAFTKV_LOG_ENGINE=lsm bash /tmp/p.sh'` → `plain=[lsm]`（正常）<br>`ssh vm 'echo x; RAFTK_LOG_ENGINE=lsm bash /tmp/p.sh'` → `plain=[]`（**丢**）<br>`ssh vm 'echo x; export RAFTK_LOG_ENGINE=lsm; bash /tmp/p.sh'` → `plain=[]`（**丢**）<br>根因未定位（怀疑是本机→VM 的自动化通道对多语句命令做了语句拆分/前缀赋值剥离，不属于 raft-kv-lsm 代码） | 工程上最保守：脚本改用**显式 CLI 参数** `--log-engine` 作为权威通道（可端到端验证），env 通道仍按 C7 透传并在 node 侧保留（`--data-dir` 单条命令探针确认过 node 能读到 env：`log-engine=lsm`）。**A/B 一律用 `--log-engine`** |
| **M6.5-D2** | §5.2 H8：「每次 fill 之后的 verify 输出必须含 `missing 0`」 | 三个 e2e 脚本内部是 `grep -q 'missing 0'`（不打印原文）；只有 `bench_m5_ab.sh` 类的基准脚本会把 verify 原文写进行格式 | H8 的**原始行**证据落在 M6.7 的 `bench_m6_ab.sh`（每格打印 `verify=[...]`）；本步只断言「脚本未因 verify 失败而退出」 |
| **M6.5-D3** | 设计 §4-M6.5 的文件清单只列 3 个 e2e 脚本 | 故障脚本（`raft_fault.sh`/`raft_snapshot_fault.sh`/`raft_membership_fault.sh`）留到 M6.6 一起改（它们还要顺带修 `LOG_BOUND` 口径） | 按设计顺序执行 |

### 未做 / 未验证（M6.5 时点）

1. **未做**：故障注入脚本双引擎 + `LOG_BOUND` 按引擎取口径（M6.6）、A/B（M6.7）。
2. **未验证**：`RAFTKV_LOG_ENGINE` 环境变量在**多语句自动化命令**下的传递（本机→VM 通道限制，见 M6.5-D1）；脚本内 `RAFTKV_LOG_ENGINE=... node` 的单进程路径已由 M6.1 的探针与本次 node 侧 stderr 证明可用。
3. **未验证**：`raft_snapshot_fault.sh` 在 lsm 引擎下的行为（M6.6；它当前直接 `stat` 了 `raft/raft.log`，在 lsm 下必然失败）。
4. **未验证**：e2e 的 ASan/TSan 变体（H11/H12 在 M6.8 收口）。

---

## M6.6 故障注入 + `LOG_BOUND` 按引擎取口径（C6/D9）

### 交付

| 文件 | 改动 |
|---|---|
| `scripts/raft_fault.sh` | `--log-engine file\|lsm`（解析循环同时保留 `--repeat`）；`start_node` 同时用 CLI 参数 + `RAFTKV_LOG_ENGINE` 把引擎交给 node；开头打印 `engine=` |
| `scripts/raft_membership_fault.sh` | 同上（6 节点、`--snapshot-threshold 1000`） |
| `scripts/raft_snapshot_fault.sh` | 同上 + **`LOG_BOUND` 按引擎取口径**：file = `<data-dir>/raft/raft.log` 字节 ≤ 1 MiB（既有判据不变）；lsm = `<data-dir>/raft-lsm` **目录总字节** ≤ `LSM_LOG_BOUND`（**实测标定**值 4 MiB，见下）；每个节点打印两个目录的字节数（`raft/`、`raft-lsm/`）供复标定 |

### LOG_BOUND 的实测标定（不是文件大小启发式）

```bash
$ bash scripts/raft_snapshot_fault.sh --log-engine lsm --repeat 1     # 先用 32 MiB 的临时上界观测
  node1 raft-lsm=2561634 bytes (bound=33554432; raft/=1489627 raft-lsm/=2561634)
  node2 raft-lsm=2579679 bytes (bound=33554432; raft/=1489627 raft-lsm/=2579679)
  node3 raft-lsm=2223606 bytes (bound=33554432; raft/=1486957 raft-lsm/=2223606)
A) 100000 entries applied, log bounded (engine=lsm, bound=33554432, bytes=2223606)

$ bash scripts/raft_snapshot_fault.sh --log-engine file --repeat 1    # 对照臂
  node3 raft/raft.log=49770 bytes (bound=1048576; raft/=1531422 raft-lsm/=0)
A) 100000 entries applied, log bounded (engine=file, bound=1048576, bytes=49770)
```
⇒ 标定：`LSM_LOG_BOUND = 4194304`（4 MiB）= 实测最大值 `2579679 B` 的 ~1.6×。
**这是真实的空间负结果（R4 确认）**：同一负载（100k 条 + 每 2000 条一次快照）下，
lsm 的日志目录稳态是 **2.22–2.58 MB**，而 file 引擎的 `raft.log` 只有 **49 770 B**（≈ **45–52×**）；
且 lsm 侧 `compact()` 只写 tombstone，物理空间要等后台 compaction 才回收。
**file 引擎的 1 MiB 判据保持原样**（D9：改了它会让 M2-M5 的历史判据口径漂移）。

### 判据与原始输出

```bash
$ bash scripts/raft_fault.sh --log-engine file --repeat 50
engine=file
raft_fault: PASS (50 iterations)                       # H5（file 臂不退化）

$ bash scripts/raft_fault.sh --log-engine lsm --repeat 50
engine=lsm
raft_fault: PASS (50 iterations)                       # H5（lsm 臂）

$ bash scripts/raft_membership_fault.sh --log-engine lsm --repeat 50
engine=lsm
raft_membership_fault: PASS (50 iterations)            # H7（lsm 臂）

$ bash scripts/raft_snapshot_fault.sh --log-engine lsm --repeat 1
engine=lsm
  node1 raft-lsm=2561634 bytes ... / node2 ... / node3 ...
A) 100000 entries applied, log bounded (engine=lsm, bound=4194304, bytes=2223606)
B) empty node caught up via InstallSnapshot (last_applied=125742)
C) fault injection PASS (1 iterations)
raft_snapshot_fault: PASS                              # H6（lsm 臂；--repeat 50 见下方说明）

$ bash scripts/raft_snapshot_fault.sh --log-engine file --repeat 1
engine=file
A) 100000 entries applied, log bounded (engine=file, bound=1048576, bytes=49770)
B) empty node caught up via InstallSnapshot (last_applied=102142)
C) fault injection PASS (1 iterations)
raft_snapshot_fault: PASS                              # H6（file 臂不退化）
```

**耗时观察（不是基准，机器同时被别的重活占用，只作定性记录）**：lsm 臂的 A 段
（`fill 100000 --pipeline 64`）实测 ~13 分钟（≈108 写/秒）；同机 file 臂约 5 分钟（≈300 写/秒）。
与设计 §6 R1 的预测方向一致，但**本数字不可作为 A/B 结论**（并发重活污染，见 M6.6-D3）。

### 逐条判据状态（设计 §4-M6.6）

| 判据 | 状态 | 证据 |
|---|---|---|
| lsm 下 `raft_fault.sh --repeat 50` PASS | ✅ 通过 | `raft_fault: PASS (50 iterations)` |
| lsm 下 `raft_membership_fault.sh --repeat 50` PASS | ✅ 通过 | `raft_membership_fault: PASS (50 iterations)` |
| lsm 下 `raft_snapshot_fault.sh` PASS | ✅ 通过（`--repeat 1`；`--repeat 50` 见未验证清单） | `raft_snapshot_fault: PASS` |
| `LOG_BOUND` 在 lsm 下改为目录总字节，阈值实测标定 | ✅ 通过 | 上面的标定输出 + `LSM_LOG_BOUND=4194304` |
| file 引擎不退化（对照） | ✅ 通过 | `raft_fault --repeat 50`（file）+ `raft_snapshot_fault --repeat 1`（file）均 PASS |
| 默认 `--repeat 50` 在 lsm 下的 `raft_snapshot_fault` | ⏳ 未完成 | 见未验证清单（100k 段在 lsm 下太慢，超时风险；已排队后台跑，结果未取到） |

### 偏差登记（M6.6）

| ID | 设计写的 | 落地 | 裁决 |
|---|---|---|---|
| **M6.6-D1** | §4-M6.6 的证据命令用 `RAFTKV_LOG_ENGINE=lsm bash scripts/...` | 改用 `--log-engine lsm`（M6.5-D1：本机→VM 通道上前缀赋值不可靠） | 同一判据、更可验证的通道 |
| **M6.6-D2** | C6「`LOG_BOUND` 判据按引擎取数据目录字节」 vs D9「file 引擎的 1 MiB 判据保持原样」 | 两条同时满足：**每个引擎取自己日志存储的字节面** —— file = `raft/raft.log`（既有 1 MiB），lsm = `raft-lsm/` 目录总字节（标定 4 MiB）；脚本对每个节点**同时打印** `raft/`（含 snapshot.dat）与 `raft-lsm/` 的字节数 | file 侧口径不变（避免历史门禁漂移）；lsm 侧用目录字节（它的持久化面是多文件） |
| **M6.6-D3** | 资源纪律：重活串行 | 标定与门禁运行的窗口里，lsm 侧同时在跑 ASan/TSan 构建与 `lsm_tests`（loadavg 峰值 15.9） | 如实登记。**功能门禁不受影响**；耗时数字只作定性参考，**未**用于任何比值结论（M6.7 会在空闲窗口重测） |

### 未做 / 未验证（M6.6 时点）

1. **未完成**：`raft_snapshot_fault.sh --log-engine lsm --repeat 50` —— 100k 段的 lsm 侧耗时约 13 分钟（且与别的重活并发），在本轮时间盒内未跑完；已放在后台队列，若最终未取到结果则本项判据**按未验证处理**（不得写成通过）。
2. **未验证**：`raft_snapshot_fault.sh` 在 lsm 下的 `LOG_BOUND` 是否在 **--repeat 50** 的长期运行中仍成立（重复的 kill -9/快照可能让 WAL/SST 累积更多）。
3. **未验证**：`--strace` 的 fsync 次数（D5/R1 的判定数字）—— 在 M6.7 采集。
4. **未验证**：file 引擎侧 `raft/` 目录字节（含 snapshot.dat ≈1.5 MB）的长期上界（既有脚本历史上只约束 raft.log）。

---

## M6.7 A/B 基准（§5）

**数字与结论全部落在 `docs/m6-bench.md`**（27 格原始行 + 汇总表 + H1-H12 + O/D 采集状态 + R1-R7 负结果 + 未验证清单）。
本节只登记口径、偏差与裁决。

### 交付

* `scripts/bench_m6_ab.sh`（新）：三臂（`base`=`~/raft-kv/build/bin`、`file`、`lsm`）同一脚本内交替
  `base → file → lsm`，每档 200 写预热丢弃、重复 `--repeats` 次取中位数、每次 `fill` 后必 `verify`；
  行格式在 `bench_m5_ab.sh` 的前缀列之后**追加** `logdir_bytes` / `open_ms_max` / `fsync_calls` /
  `fsync_us_total`；任一格 `verify` 缺 `missing 0` ⇒ 退出码 1；**不设比值硬门禁**（D13）。
* `docs/m6-bench.md`（新）：按设计 §6.4 的固定章节写。

### 关键数字（详见 m6-bench.md §3）

| pipeline | n | base ms/w | file ms/w | lsm ms/w | lsm/file 延迟 | lsm/file 吞吐 |
|---|---|---|---|---|---|---|
| 1 | 1000 | 34.365 | 33.847 | 34.178 | 1.01x | 0.97x |
| 8 | 4000 | 6.078 | 5.712 | 5.994 | 1.05x | 0.95x |
| 64 | 4000 | 2.979 | 2.902 | 3.264 | **1.12x** | **0.89x** |

⇒ **base vs file 无回归（±2%）**；**lsm 略慢且随并发加深，但未出现 R1 预测的 0.47× 塌陷**；
空间放大在小负载 ≈1.5×、在 100k+频繁快照下 ≈50×（R4 确认）。

### 偏差登记（M6.7）

| ID | 设计写的 | 落地 | 裁决 |
|---|---|---|---|
| **M6.7-D1** | §5.5 的正式口径 `n = 5000/20000/20000`（照搬 `bench_m5_ab.sh`） | 本轮用 `n = 1000/4000/4000`（每行都打印 `n=`） | 时间盒 + lsm 臂写吞吐限制；偏差与影响写进 `m6-bench.md` §7 第 9 条 |
| **M6.7-D2** | §5.2 H9：「`base` 臂在同一套脚本（H2-H7）下必须全部通过」 | 只跑了 A/B 的 base 臂（27 格全 `missing 0`）；**没有**用 `~/raft-kv/scripts/*.sh` 跑 base 的 e2e/fault | 未做项如实登记（`m6-bench.md` §4/§7）；A/B 的 base 列仍然证明「基线 vs 工作区」的可比性 |
| **M6.7-D3** | §5.2 O4/D5：用 `strace -f -e trace=fsync,fdatasync -c` 采 leader 的 fsync 次数 | 实测该通道**无效**：strace 让系统慢 3-4×，连 `base` 臂都出现 `missing 269`（H8 失败），且 `-c` 汇总在读文件时未落盘（`fsync_calls=0`） | D5 **按未采集**入档（不编数字）；该轮原始行保留在 `m6-bench.md` §2 末并明确标注**作废** |
| **M6.7-D4** | §5.2 O1/D2 要 P99 / max；D12 授权「为本里程碑引入 P99 采样」 | node 侧直方图**未实现**（本轮只做存储层适配，没动 `status`/`Metrics`） | P99/max **未采集**，如实入档（`m6-bench.md` §5/§7） |
| **M6.7-D5** | 计时敏感基准不得与另一侧重活并发 | 正式 27 格期间 VM loadavg 4.7–5.4（另一 agent 的构建/测试窗口），三臂**同轮交替**把相对比较的偏置压到最小 | 如实登记为已知限制；绝对 qps 请以安静机器重测为准 |

### 未做 / 未验证（M6.7 时点）

见 `docs/m6-bench.md` §7 的 11 条（base 臂脚本门禁未跑、`snapshot_fault --repeat 50` 未跑完、
P99 未采集、D5 通道无效、lsm 内部统计未接线、D4 口径未采集、正式 n 未达、R2/R3 无定向性能数字、
机器安静度限制、TSan 未跑全量、base 臂未跑 sanitizer）。

**H11/H12 已在 M6.8 收口**：

```bash
$ cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ASAN=ON -DRAFTKV_LSM_DIR=/tmp/lsm-pin-b1bd050
$ cmake --build build-asan -j8            # 0 warning
$ ./build-asan/bin/raftkv_raft_tests
[==========] 121 tests from 18 test suites ran. (50991 ms total)
[  PASSED  ] 121 tests.                    # H11：无 ASan 报告

$ cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TSAN=ON -DRAFTKV_LSM_DIR=/tmp/lsm-pin-b1bd050
$ cmake --build build-tsan -j8            # 0 warning
$ setarch $(uname -m) -R ./build-tsan/bin/raftkv_raft_tests --gtest_filter='LsmLogStore.*'
[==========] 20 tests from 1 test suite ran. (2008 ms total)
[  PASSED  ] 20 tests.                     # H12：无 TSan 报告
```

（两者都用 pin 的 lsm 基座 `b1bd050` 一起编译，因此 lsm 侧代码也在 sanitizer 覆盖内。）

---

## M6 收尾状态（供父代理接手的两条硬事实）

1. **A/B 所用的 lsm 基座是 `b1bd050`（pin 在 `/tmp/lsm-pin-b1bd050`）**。父代理裁决 #1 要求
   「R1 落地后要在**最终 lsm 提交**上重跑 A/B」—— 收尾时点复查：`~/lsm-kv` 的 HEAD **仍是 `b1bd050`**，
   R1 修复仍在**未提交的工作区**里（`git status --short` 显示 `M src/db_impl.cpp/h`、`M src/wal.cpp/h`、
   `?? tests/sync_isolation_test.cpp`、`?? tests/env_slow_sync.h`）。
   ⇒ **M6.7 的 A/B 必须在 R1 提交落地后重跑**（`scripts/bench_m6_ab.sh` 已就绪：换 pin 目录、
   `-DRAFTKV_LSM_DIR=<新 pin>` 重建即可），并把新数字替换 `docs/m6-bench.md` §2/§3 的对应行。
   本轮**没有**触碰 `~/lsm-kv`（只读）。
2. **最终 HEAD 的单测门禁**：`./build/bin/raftkv_raft_tests` = **121/121 PASSED**（47713 ms），
   `git status --short` 空，全部提交已 `push origin main`（未打 tag —— tag 由父代理打）。
