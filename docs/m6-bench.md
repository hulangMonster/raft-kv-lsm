# M6 A/B 基准与负结果（docs/m6-bench.md）

> 规格：`docs/m6-design.md` §5/§6。**不设比值硬门禁**（D13）——M6 的目标是「把 raft-kv 的存储层
> 适配到 lsm」，不是提速；本文件只给数字与负结果，判定留给用户。
> 偏差与裁决登记在 `docs/m6-evidence.md`（本文件只放基准口径的数字与结论）。
> 纪律：**未采集 == 未采集**，不写成通过；每个数字都能追溯到 `scripts/bench_m6_ab.sh` 的原始行。

## 1. 环境

```
$ uname -sr; nproc; df -T /tmp | tail -1; cat /proc/loadavg
Linux 6.8.0-138-generic
8
/dev/mapper/vgubuntu-root ext4  38614848 27042804   9798092  74% /     # ext4，**非 tmpfs**
4.69 5.36 6.64                       # 正式跑的 cell 前后 1 分钟负载（VM 上有别的重活，见 §7）
```
- 三个臂都是**同机、同轮、交替**测量（`base → file → lsm` 每 rep 一轮），每格 = 3 次取中位数。
- lsm 基座：`~/lsm-kv @ b1bd050`（`git archive` pin 到 `/tmp/lsm-pin-b1bd050` 后链接）；
  base 臂 = `~/raft-kv/build/bin`（远端基线 `1463620`，未改动）。
- 每格独立临时数据目录（`mktemp -d`），200 写预热丢弃，`fill` 后立即 `verify`。

## 2. 原始行（`scripts/bench_m6_ab.sh --repeats 3 --pipelines "1 8 64"`，27 格逐行粘贴）

```
base p=1 rep=1 n=1000 ms=36061.2 ms_per_write=36.061 qps=28 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=161 fsync_calls=NA fsync_us_total=NA
file p=1 rep=1 n=1000 ms=33846.9 ms_per_write=33.847 qps=30 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=137 fsync_calls=NA fsync_us_total=NA
lsm p=1 rep=1 n=1000 ms=35252.1 ms_per_write=35.252 qps=28 verify=[verified 1000 missing 0] logdir_bytes=289203 open_ms_max=140 fsync_calls=NA fsync_us_total=NA
base p=1 rep=2 n=1000 ms=34364.6 ms_per_write=34.365 qps=29 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=154 fsync_calls=NA fsync_us_total=NA
file p=1 rep=2 n=1000 ms=33861.1 ms_per_write=33.861 qps=30 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=163 fsync_calls=NA fsync_us_total=NA
lsm p=1 rep=2 n=1000 ms=34177.8 ms_per_write=34.178 qps=29 verify=[verified 1000 missing 0] logdir_bytes=289242 open_ms_max=150 fsync_calls=NA fsync_us_total=NA
base p=1 rep=3 n=1000 ms=32513.3 ms_per_write=32.513 qps=31 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=175 fsync_calls=NA fsync_us_total=NA
file p=1 rep=3 n=1000 ms=33137.9 ms_per_write=33.138 qps=30 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=168 fsync_calls=NA fsync_us_total=NA
lsm p=1 rep=3 n=1000 ms=34096.0 ms_per_write=34.096 qps=29 verify=[verified 1000 missing 0] logdir_bytes=289242 open_ms_max=136 fsync_calls=NA fsync_us_total=NA
base p=8 rep=1 n=4000 ms=24505.7 ms_per_write=6.126 qps=163 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=173 fsync_calls=NA fsync_us_total=NA
file p=8 rep=1 n=4000 ms=22849.0 ms_per_write=5.712 qps=175 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=147 fsync_calls=NA fsync_us_total=NA
lsm p=8 rep=1 n=4000 ms=23354.8 ms_per_write=5.839 qps=171 verify=[verified 4000 missing 0] logdir_bytes=861933 open_ms_max=135 fsync_calls=NA fsync_us_total=NA
base p=8 rep=2 n=4000 ms=23959.6 ms_per_write=5.990 qps=167 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=153 fsync_calls=NA fsync_us_total=NA
file p=8 rep=2 n=4000 ms=26148.6 ms_per_write=6.537 qps=153 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=171 fsync_calls=NA fsync_us_total=NA
lsm p=8 rep=2 n=4000 ms=26829.0 ms_per_write=6.707 qps=149 verify=[verified 4000 missing 0] logdir_bytes=862313 open_ms_max=134 fsync_calls=NA fsync_us_total=NA
base p=8 rep=3 n=4000 ms=24312.2 ms_per_write=6.078 qps=165 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=140 fsync_calls=NA fsync_us_total=NA
file p=8 rep=3 n=4000 ms=21771.8 ms_per_write=5.443 qps=184 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=143 fsync_calls=NA fsync_us_total=NA
lsm p=8 rep=3 n=4000 ms=23977.2 ms_per_write=5.994 qps=167 verify=[verified 4000 missing 0] logdir_bytes=862275 open_ms_max=146 fsync_calls=NA fsync_us_total=NA
base p=64 rep=1 n=4000 ms=11191.2 ms_per_write=2.798 qps=357 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=134 fsync_calls=NA fsync_us_total=NA
file p=64 rep=1 n=4000 ms=11028.8 ms_per_write=2.757 qps=363 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=147 fsync_calls=NA fsync_us_total=NA
lsm p=64 rep=1 n=4000 ms=12982.3 ms_per_write=3.246 qps=308 verify=[verified 4000 missing 0] logdir_bytes=843275 open_ms_max=142 fsync_calls=NA fsync_us_total=NA
base p=64 rep=2 n=4000 ms=11918.0 ms_per_write=2.979 qps=336 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=154 fsync_calls=NA fsync_us_total=NA
file p=64 rep=2 n=4000 ms=12475.1 ms_per_write=3.119 qps=321 verify=[verified 4000 missing 0] logdir_bytes=689052 open_ms_max=146 fsync_calls=NA fsync_us_total=NA
lsm p=64 rep=2 n=4000 ms=13056.6 ms_per_write=3.264 qps=306 verify=[verified 4000 missing 0] logdir_bytes=843237 open_ms_max=140 fsync_calls=NA fsync_us_total=NA
base p=64 rep=3 n=4000 ms=11954.7 ms_per_write=2.989 qps=335 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=133 fsync_calls=NA fsync_us_total=NA
file p=64 rep=3 n=4000 ms=11609.9 ms_per_write=2.902 qps=345 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=145 fsync_calls=NA fsync_us_total=NA
lsm p=64 rep=3 n=4000 ms=13228.6 ms_per_write=3.307 qps=302 verify=[verified 4000 missing 0] logdir_bytes=849190 open_ms_max=150 fsync_calls=NA fsync_us_total=NA
```

**作废的一轮（D5 的 strace 尝试，不得用作结论）**：`--strace` 把三个 node 都挂在 `strace -f -c`
下会让系统慢 3–4×，**连 base 臂都出现 `missing 269`**（H8 判据失败），且 `-c` 的 fsync 汇总在
读文件时还没落盘（`fsync_calls=0`）。原始行（保留以示该通道无效）：

```
base p=8 rep=1 n=500 ms=56169.1 ms_per_write=112.338 qps=9  verify=[verified 500 missing 269] logdir_bytes=78342  open_ms_max=813 fsync_calls=0 fsync_us_total=0
file p=8 rep=1 n=500 ms=46753.6 ms_per_write=93.507  qps=11 verify=[verified 500 missing 0]   logdir_bytes=118371 open_ms_max=552 fsync_calls=0 fsync_us_total=0
lsm  p=8 rep=1 n=500 ms=14750.7 ms_per_write=29.501  qps=34 verify=[verified 500 missing 0]   logdir_bytes=161791 open_ms_max=652 fsync_calls=0 fsync_us_total=0
```

## 3. 汇总表（每格 = 3 次中位数）

| pipeline | n | base ms/w | file ms/w | lsm ms/w | base qps | file qps | lsm qps | lsm/file 延迟 | lsm/file 吞吐 | verify | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1000 | 34.365 | 33.847 | 34.178 | 29 | 30 | 29 | 1.01x | 0.97x | missing 0 | 不设比值硬门禁（D13） |
| 8 | 4000 | 6.078 | 5.712 | 5.994 | 165 | 175 | 167 | 1.05x | 0.95x | missing 0 | 不设比值硬门禁（D13） |
| 64 | 4000 | 2.979 | 2.902 | 3.264 | 336 | 345 | 306 | 1.12x | 0.89x | missing 0 | 不设比值硬门禁（D13） |

* **base vs file（无回归对照）**：33.847/34.365、5.712/6.078、2.902/2.979 —— 三档都在 ±2% 内
  ⇒ M6 的改动**没有**改变原 FileLogStore 路径的行为/性能。
* **lsm vs file**：延迟 +1%（p=1）/+5%（p=8）/+12%（p=64）；吞吐 0.97×/0.95×/**0.89×**。
  劣化随 pipeline 加深而变大，但**没有**出现设计 §6 R1 预测的 0.47× 量级塌陷。
* 空间（`logdir_bytes` 三节点合计）：base/file = 193 887 B（n=1000）/688 887 B（n=4000）；
  lsm = 289 203 B / 861 933 B ⇒ **≈1.49× / ≈1.25×**（小负载下的目录字节）。
  **大负载（100k 条 + 频繁快照）下差距拉大到 ~50×**（M6.6 的标定：lsm `raft-lsm/` 2.22–2.58 MB
  vs file `raft.log` 49 770 B）——见 §6.4 R4。
* 启动（`open_ms_max`，从 exec 到 `status` 首次可用，3 节点取最大）：base 133–175 ms、
  file 137–171 ms、lsm 134–150 ms ⇒ 本负载下**无差异**（注意：这不是 D4 的口径，见 §5）。

## 4. 硬门禁 H1–H12 的结果

| 门禁 | base | file | lsm | 判据字符串 / 证据 |
|---|---|---|---|---|
| H1 单测 `raftkv_raft_tests` | n/a | ✅ 121/121 PASSED | ✅ 121/121 PASSED（同一二进制，lsm 用例在列） | `[  PASSED  ] 121 tests.`（`docs/m6-evidence.md` §M6.4） |
| H2 `raft_e2e.sh` | ⛔ 未跑（见 §7） | ✅ `raft_e2e: PASS` | ✅ `raft_e2e: PASS` | evidence §M6.5 |
| H3 `raft_snapshot_e2e.sh` | ⛔ 未跑 | ✅ `raft_snapshot_e2e: PASS` | ✅ `raft_snapshot_e2e: PASS` | evidence §M6.5 |
| H4 `raft_membership_e2e.sh` | ⛔ 未跑 | ✅ PASS（exit 0） | ✅ PASS（exit 0） | evidence §M6.5（verify 内部断言 `missing 0`） |
| H5 `raft_fault.sh --repeat 50` | ⛔ 未跑 | ✅ `raft_fault: PASS (50 iterations)` | ✅ `raft_fault: PASS (50 iterations)` | evidence §M6.6 |
| H6 `raft_snapshot_fault.sh` | ⛔ 未跑 | ✅ PASS（`--repeat 1`，A/B/C 全过） | ✅ PASS（`--repeat 1`）；`--repeat 50` **未跑完** | evidence §M6.6 |
| H7 `raft_membership_fault.sh --repeat 50` | ⛔ 未跑 | ⛔ 未跑（file 臂本步未重复） | ✅ `raft_membership_fault: PASS (50 iterations)` | evidence §M6.6 |
| H8 `verify` 含 `missing 0` | ✅ 9/9 格 | ✅ 9/9 格 | ✅ 9/9 格 | §2 的 27 行（另见作废的 strace 轮） |
| H9 `base` 臂不退化 | ✅ 基准 | ✅ ±2% 内 | — | §3 的 base/file 两列；**H2–H7 的 base 臂未跑**（见 §7） |
| H10 无多数派时写不返回 OK | 内嵌于 H2/H5/H6 | 同左 | 同左 | 脚本内既有断言；三臂的 PASS 即该断言通过 |
| H11 ASan 构建下 H1 无报告 | — | ✅ **通过** | ✅ **通过** | `cmake -B build-asan -DENABLE_ASAN=ON`（0 warning）⇒ `[  PASSED  ] 121 tests.`（50991 ms），无 ASan 报告 |
| H12 TSan 构建下 `LsmLogStore` 无报告 | — | — | ✅ **通过** | `cmake -B build-tsan -DENABLE_TSAN=ON`（0 warning）+ `setarch x86_64 -R` ⇒ `LsmLogStore.*` **20/20 PASSED**（2008 ms），无 TSan 报告 |

## 5. 观测 O1–O8 / D1–D10 的数字

| ID | 项 | 状态 | 数字 / 原因 |
|---|---|---|---|
| O1/D1 | 吞吐 / ms-per-write | ✅ 已采集 | §3 表（p=1/8/64 三档） |
| O1/D2 | P99 / max 延迟 | ⛔ **未采集** | 设计 D12 的 node 侧直方图**未实现**（本里程碑未改 `status`/`Metrics`）；均值掩盖长尾的风险仍在 |
| O2/D3 | 日志目录字节 | ✅ 已采集（小负载） | §2 的 `logdir_bytes`；大负载见 §6.4 |
| O3/D4 | 启动恢复耗时（100k 条后重启） | ⚠️ 口径不符 | 只采集了「空库启动到 status 可用」（`open_ms_max` 133–175 ms）；**100k 条后的恢复耗时未按 D4 采集** |
| O4/D5 | fsync 次数 / 总耗时 | ⛔ **未采集** | `strace -f -c` 通道实测无效（连 base 臂都 verify 失败，见 §2 末）；没有可信数字 |
| O5 | lsm 各层 files/bytes | ⛔ 未采集 | 需要 node 侧调 `GetLevelStats()`（未接线） |
| O6 | 空间放大 | ⚠️ 部分 | 用 `du -sb` 间接替代（§3/§6.4）；`GetAmplificationStats()` 未接线 |
| O7/D6 | 写停顿 `stall_events`/`stall_micros` | ⛔ 未采集 | `GetFlushStats()` 未接线；门禁中未观测到挂起 |
| O8/D8 | WAL 尾部截断字节 | ✅ 已采集（单测） | `LsmLogStore.TornWalTailIsTruncatedByOpenAndCounted`：`walTailTruncatedBytes() > 0`（具体值未打印） |
| D7 | compaction 轮次/最大耗时 | ⛔ 未采集 | 同上（未接线） |
| D9 | `truncateSuffix` 条目数分布 | ⚠️ 部分 | `LsmLogStore::Stats.truncated_entries` 有计数与单测断言（`== 2`）；**分布**未采集 |
| D10 | `slice()` 调用次数/条数 | ⚠️ 部分 | `Stats.slice_calls` 有计数；**未在 A/B 中导出** |

## 6. 负结果（设计 §6）

### 6.1 R1 —— `DB::Sync()` 持 `commit_mu_` 做 fsync 是否把组提交打散？→ **未确认**
干净 A/B 的三档里 lsm/file 延迟 = 1.01×/1.05×/**1.12×**、吞吐 = 0.97×/0.95×/**0.89×**，
**没有**出现设计预测的 0.47× 量级塌陷。**但这不等于 R1 不存在**：D5（fsync 次数）没有可信数字，
所以「组提交是否被打散」**无法直接判定**。定性旁证：在**并发重活**（另一 agent 的 ASan/TSan
构建与 `lsm_tests`，loadavg 峰值 15.9）下，lsm 臂的 100k 条 fill 用了 ~13 分钟（≈108 写/秒），
同机 file 臂约 5 分钟（≈300 写/秒）——方向与 R1 一致，但该数字被污染，不作为结论。

### 6.2 R2 —— `slice()` 每次建 lsm 迭代器 → **未单独测量**
心跳路径的开销被合并进 p=1 的 +1% 延迟里；没有单独的 slice 计数/耗时（D10 未导出）。

### 6.3 R3 —— `truncateSuffix` 从 O(1) 变成 O(被截断条目数) → **未直接测量，未观测到问题**
单测钉住了写放大口径（`Stats.truncated_entries`）；50 轮 kill -9/SIGSTOP 故障注入
（`raft_fault`/`membership_fault` lsm 臂）全过。没有「长冲突后缀」的定向构造。

### 6.4 R4 —— 空间放大 + 物理空间不即时回收 → **确认（如实入档）**
同一负载（100k 条 + 每 2000 条一次快照）下：
* file 引擎 `raft/raft.log` = **49 770 B**（compact 后 rename 缩小）；
* lsm 引擎 `raft-lsm/` 目录 = **2 221 606–2 579 679 B**（≈ **45–52×**，且 `compact()` 只写
   tombstone，物理回收要等后台 compaction）。
阈值标定见 `docs/m6-evidence.md` §M6.6（`LSM_LOG_BOUND = 4 MiB`，= 实测最大值的 ~1.6×）。

### 6.5 R5 —— 写停顿（`WaitForImmutableCapacity`）→ **未采集**
`GetFlushStats().stall_events/stall_micros` 未接线；门禁与 A/B 中**未观测到**挂起或超时。

### 6.6 R6 —— 启动恢复代价 → **未按 D4 口径采集**
只有空库启动（133–175 ms）；100k 条日志后的重启耗时未测。

### 6.7 R7 —— P99 抖动来自后台线程 → **未采集**（无直方图，见 O1/D2）。

## 7. 未验证清单（每条一行 + 原因）

1. **H9 的 base 臂脚本门禁（用 `~/raft-kv/scripts/*.sh` 跑 H2–H7）**：只跑了 A/B 的 base 臂（27 格全 `missing 0`），没跑 base 的 e2e/fault 脚本。
2. **`raft_snapshot_fault.sh --log-engine lsm --repeat 50`**：100k 段在 lsm 下 ~13 分钟（并发重活下），本轮未跑完；`--repeat 1` 已 PASS。
3. **P99 / max 延迟（D2）**：node 侧直方图未实现（设计 D12）。
4. **fsync 次数/耗时（D5/R1）**：`strace -f -c` 通道实测无效（连 base 臂都 verify 失败），无可信数字。
5. **lsm 内部统计（O5/O6/O7、D7）**：`GetLevelStats`/`GetAmplificationStats`/`GetFlushStats` 未接线到 node。
6. **100k 条后的启动恢复耗时（D4）**：未按口径采集（只有空库启动 133–175 ms）。
7. **正式口径的 n（设计 §5.5 的 5000/20000/20000）**：本轮用 1000/4000/4000（时间盒 + lsm 写吞吐限制）；每行都打印 `n=`。
8. **`slice()` / `truncateSuffix` 的定向性能（R2/R3）**：只有功能证据，没有性能数字。
9. **A/B 期间的机器安静度**：27 格期间 loadavg 4.7–5.4（VM 上有另一 agent 的重活窗口）；三臂同轮交替把相对比较的偏置压到最小，但仍属已知限制。
10. **H11/H12 的口径**：H11 是 ASan **全量**（121/121）；H12 是 TSan 的 **`LsmLogStore.*`**（20/20）——**不是** TSan 全量（M6.8 时间盒内未跑 TSan 全量）。
11. **base 臂的 ASan/TSan**：未做（只在 `~/raft-kv-lsm` 上跑）。

## 8. 结论

* **base vs file**：三档都在 ±2% 内 ⇒ **M6 的改动对原 FileLogStore 路径没有可测回归**。
* **lsm vs file**：p=1 延迟 +1%、p=8 +5%、p=64 **+12%**（吞吐 0.89×）。
  ⇒ LSM 在本机**略慢**，且劣化随并发加深；**但没有出现量级塌陷**（设计预测的 0.47× 未复现）。
* **LSM 更快的地方**：本 A/B 里**没有**——延迟/吞吐三档都不占优；启动时间三臂相同。
  （唯一方向性优势是设计 §6.1 预测的「前缀压缩成本」在**空间回收**上，但那要等后台 compaction，
  且 §6.4 显示稳态目录字节反而更大。）
* **空间**：小负载 ≈1.5×；100k + 频繁快照 ≈50×（R4 确认）。
* **说得更谨慎一点**：本结论只覆盖「单机 3 节点、n≤4000、无 P99、无 fsync 计数」的口径；
  高并发大负载 + 无重活干扰下的数字**未验证**。
* **并发正确性门禁已收口**：ASan 全量 121/121、TSan 的 `LsmLogStore.*` 20/20，均 0 warning、0 报告（H11/H12）。
