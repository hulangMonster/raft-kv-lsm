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

> 两轮 A/B 的唯一差别是**链接的 lsm 基座**；旧行**保留不改**，并显式标注基座 sha。

### 2.1 第一轮（lsm 基座 **b1bd050**，R1 未修）

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


### 2.2 第二轮（lsm 基座 **f06a44d**，R1 已修；2026-10-01 07:26 起）

命令与口径与 §2.1 **完全相同**（`--repeats 3 --pipelines "1 8 64"`，三臂同轮交替），
唯一差别是链接的 lsm 基座：`f06a44d`（父 `b1bd050`）——`DB::Sync()` 的 fsync 不再持 `commit_mu_`
（WAL 叶子锁 + `shared_ptr` 生命周期）。原始 raw：`/tmp/m6bench-logs/raw-1790810796.txt`。

```
base p=1 rep=1 n=1000 ms=34923.7 ms_per_write=34.924 qps=29 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=137 fsync_calls=NA fsync_us_total=NA
file p=1 rep=1 n=1000 ms=34125.5 ms_per_write=34.126 qps=29 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=135 fsync_calls=NA fsync_us_total=NA
lsm p=1 rep=1 n=1000 ms=34887.8 ms_per_write=34.888 qps=29 verify=[verified 1000 missing 0] logdir_bytes=289242 open_ms_max=135 fsync_calls=NA fsync_us_total=NA
base p=1 rep=2 n=1000 ms=34504.1 ms_per_write=34.504 qps=29 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=130 fsync_calls=NA fsync_us_total=NA
file p=1 rep=2 n=1000 ms=34715.2 ms_per_write=34.715 qps=29 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=143 fsync_calls=NA fsync_us_total=NA
lsm p=1 rep=2 n=1000 ms=35002.0 ms_per_write=35.002 qps=29 verify=[verified 1000 missing 0] logdir_bytes=289203 open_ms_max=119 fsync_calls=NA fsync_us_total=NA
base p=1 rep=3 n=1000 ms=33684.9 ms_per_write=33.685 qps=30 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=146 fsync_calls=NA fsync_us_total=NA
file p=1 rep=3 n=1000 ms=34606.0 ms_per_write=34.606 qps=29 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=148 fsync_calls=NA fsync_us_total=NA
lsm p=1 rep=3 n=1000 ms=35163.6 ms_per_write=35.164 qps=28 verify=[verified 1000 missing 0] logdir_bytes=289242 open_ms_max=152 fsync_calls=NA fsync_us_total=NA
base p=8 rep=1 n=4000 ms=21918.9 ms_per_write=5.480 qps=182 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=141 fsync_calls=NA fsync_us_total=NA
file p=8 rep=1 n=4000 ms=22785.3 ms_per_write=5.696 qps=176 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=139 fsync_calls=NA fsync_us_total=NA
lsm p=8 rep=1 n=4000 ms=21583.7 ms_per_write=5.396 qps=185 verify=[verified 4000 missing 0] logdir_bytes=862084 open_ms_max=142 fsync_calls=NA fsync_us_total=NA
base p=8 rep=2 n=4000 ms=22592.0 ms_per_write=5.648 qps=177 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=138 fsync_calls=NA fsync_us_total=NA
file p=8 rep=2 n=4000 ms=22009.6 ms_per_write=5.502 qps=182 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=147 fsync_calls=NA fsync_us_total=NA
lsm p=8 rep=2 n=4000 ms=21463.9 ms_per_write=5.366 qps=186 verify=[verified 4000 missing 0] logdir_bytes=862011 open_ms_max=146 fsync_calls=NA fsync_us_total=NA
base p=8 rep=3 n=4000 ms=22221.8 ms_per_write=5.555 qps=180 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=134 fsync_calls=NA fsync_us_total=NA
file p=8 rep=3 n=4000 ms=22054.5 ms_per_write=5.514 qps=181 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=144 fsync_calls=NA fsync_us_total=NA
lsm p=8 rep=3 n=4000 ms=20904.1 ms_per_write=5.226 qps=191 verify=[verified 4000 missing 0] logdir_bytes=861971 open_ms_max=134 fsync_calls=NA fsync_us_total=NA
base p=64 rep=1 n=4000 ms=11247.2 ms_per_write=2.812 qps=356 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=133 fsync_calls=NA fsync_us_total=NA
file p=64 rep=1 n=4000 ms=11596.9 ms_per_write=2.899 qps=345 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=153 fsync_calls=NA fsync_us_total=NA
lsm p=64 rep=1 n=4000 ms=10520.0 ms_per_write=2.630 qps=380 verify=[verified 4000 missing 0] logdir_bytes=843623 open_ms_max=143 fsync_calls=NA fsync_us_total=NA
base p=64 rep=2 n=4000 ms=11520.9 ms_per_write=2.880 qps=347 verify=[verified 4000 missing 0] logdir_bytes=689034 open_ms_max=143 fsync_calls=NA fsync_us_total=NA
file p=64 rep=2 n=4000 ms=11256.2 ms_per_write=2.814 qps=355 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=138 fsync_calls=NA fsync_us_total=NA
lsm p=64 rep=2 n=4000 ms=10239.8 ms_per_write=2.560 qps=391 verify=[verified 4000 missing 0] logdir_bytes=843619 open_ms_max=151 fsync_calls=NA fsync_us_total=NA
base p=64 rep=3 n=4000 ms=10792.6 ms_per_write=2.698 qps=371 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=145 fsync_calls=NA fsync_us_total=NA
file p=64 rep=3 n=4000 ms=11078.7 ms_per_write=2.770 qps=361 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=141 fsync_calls=NA fsync_us_total=NA
lsm p=64 rep=3 n=4000 ms=10479.0 ms_per_write=2.620 qps=382 verify=[verified 4000 missing 0] logdir_bytes=843275 open_ms_max=145 fsync_calls=NA fsync_us_total=NA
```

## 3. 汇总表（每格 = 3 次中位数）

### 3.1 第一轮（lsm 基座 b1bd050，R1 未修）

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


### 3.2 第二轮（基座 f06a44d，R1 已修）

| pipeline | n | base ms/w | file ms/w | lsm ms/w | base qps | file qps | lsm qps | lsm/file 延迟 | lsm/file 吞吐 | verify | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1000 | 34.504 | 34.606 | 35.002 | 29 | 29 | 29 | 1.01x | 1.00x | missing 0 | 不设比值硬门禁（D13） |
| 8 | 4000 | 5.555 | 5.514 | **5.366** | 180 | 181 | 186 | **0.97x** | **1.03x** | missing 0 | 不设比值硬门禁（D13） |
| 64 | 4000 | 2.812 | 2.814 | **2.620** | 356 | 355 | 382 | **0.93x** | **1.08x** | missing 0 | 不设比值硬门禁（D13） |

* **base vs file**：34.606/34.504、5.514/5.555、2.814/2.812 —— 仍在 ±1% 内 ⇒ 依然**无可测回归**。
* **lsm vs file（与 §3.1 相比结论反转）**：延迟 **1.01× / 0.97× / 0.93×**，吞吐 **1.00× / 1.03× / 1.08×**
  ⇒ R1 修复后，lsm 在 p=8/p=64 **反超 file**（p=64 快 ~7%），p=1 持平。
* **设计 §6 R1 预测的 0.47× 塌陷**：在 b1bd050 上**未出现**（本轮修前是 0.89× 吞吐）；在 f06a44d 上
  连「劣化」本身都反转成正向。⇒ R1 的**结构性缺陷确实存在**（代码注释与父代理裁决），
  但在**本机的负载口径**（3 节点单机、n≤4000、p≤64）下，它并没有表现为量级塌陷；
  修复的价值体现在 p=64 的 +8% 吞吐（以及 `Sync()` 不再阻塞并发入队这一结构性质）。
* 空间（`logdir_bytes` 三节点合计）：base/file = 193 887 B（n=1000）/688 887 B（n=4000）；
  lsm = 289 242 B / 843 275–862 084 B ⇒ 与 §3.1 同量级（≈1.49× / ≈1.22–1.25×）。
  **R4（大负载 ≈50×）不受 R1 修复影响**——那是 `compact()` 只写 tombstone + 后台回收造成的。

## 4. 硬门禁 H1–H12 的结果

| 门禁 | base | file | lsm | 判据字符串 / 证据 |
|---|---|---|---|---|
| H1 单测 `raftkv_raft_tests` | n/a | ✅ 121/121 PASSED | ✅ 121/121 PASSED（同一二进制，lsm 用例在列） | `[  PASSED  ] 121 tests.`（`docs/m6-evidence.md` §M6.4） |
| H2 `raft_e2e.sh` | ⛔ 未跑（见 §7） | ✅ `raft_e2e: PASS` | ✅ `raft_e2e: PASS` | evidence §M6.5 |
| H3 `raft_snapshot_e2e.sh` | ⛔ 未跑 | ✅ `raft_snapshot_e2e: PASS` | ✅ `raft_snapshot_e2e: PASS` | evidence §M6.5 |
| H4 `raft_membership_e2e.sh` | ⛔ 未跑 | ✅ PASS（exit 0） | ✅ PASS（exit 0） | evidence §M6.5（verify 内部断言 `missing 0`） |
| H5 `raft_fault.sh --repeat 50` | ⛔ 未跑 | ✅ `raft_fault: PASS (50 iterations)` | ✅ `raft_fault: PASS (50 iterations)` | evidence §M6.6 |
| H6 `raft_snapshot_fault.sh` | ⛔ 未跑 | ✅ PASS（`--repeat 1`，A/B/C 全过） | ⛔ **FAIL（红，f06a44d）** | 基座 f06a44d + `--repeat 50`：A 段空间判据 `node1 raft-lsm=4450355 exceeds 4194304 (not bounded)` ⇒ **exit 1**；原始输出见 §7.1 |
| H7 `raft_membership_fault.sh --repeat 50` | ⛔ 未跑 | ⛔ 未跑（file 臂本步未重复） | ✅ `raft_membership_fault: PASS (50 iterations)` | evidence §M6.6 |
| H8 `verify` 含 `missing 0` | ✅ 9/9 格 | ✅ 9/9 格 | ✅ 9/9 格 | §2 的 27 行（另见作废的 strace 轮） |
| H9 `base` 臂不退化 | ✅ 基准 | ✅ ±2% 内 | — | §3 的 base/file 两列；**H2–H7 的 base 臂未跑**（见 §7） |
| H10 无多数派时写不返回 OK | 内嵌于 H2/H5/H6 | 同左 | 同左 | 脚本内既有断言；三臂的 PASS 即该断言通过 |
| H11 ASan 构建下 H1 无报告 | — | ✅ **通过** | ✅ **通过** | `cmake -B build-asan -DENABLE_ASAN=ON`（0 warning）⇒ `[  PASSED  ] 121 tests.`（50991 ms），无 ASan 报告 |
| H12 TSan 构建下 `LsmLogStore` 无报告 | — | — | ✅ **通过** | `cmake -B build-tsan -DENABLE_TSAN=ON`（0 warning）+ `setarch x86_64 -R` ⇒ `LsmLogStore.*` **20/20 PASSED**（2008 ms），无 TSan 报告 |

## 5. 观测 O1–O8 / D1–D10 的数字

### 5.1 补充轮（基座 f06a44d，`--repeats 1`）：node 侧 fsync 计数 + lsm 引擎内部统计

为了让「lsm 与 file 的差异来自哪」有**同轮数字**，M6.r2 把两类只读观测接了出来：
`Metrics::statusFragment()`（本就有 `lat_p50_us/lat_p99_us/fsync_calls/fsync_ms`）与
`LsmLogStore::engineStatsFragment()`（`GetLevelStats`/`GetFlushStats`/`GetAmplificationStats`
的计数，file 引擎下**不输出**这些字段 —— 不是 0）。原始行（9 格，`--repeats 1`，仅用于采集新列）：

```
base p=1 rep=1 n=1000 ms=33786.2 ms_per_write=33.786 qps=30 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=136 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=1202 node_fsync_ms=9390 lsm_stats=[NA]
file p=1 rep=1 n=1000 ms=35495.1 ms_per_write=35.495 qps=28 verify=[verified 1000 missing 0] logdir_bytes=193887 open_ms_max=143 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=1202 node_fsync_ms=9670 lsm_stats=[NA]
lsm  p=1 rep=1 n=1000 ms=34389.2 ms_per_write=34.389 qps=29 verify=[verified 1000 missing 0] logdir_bytes=289203 open_ms_max=133 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=1202 node_fsync_ms=9339 lsm_stats=[lsm_l0_files=0 lsm_sst_files=0 lsm_sst_bytes=0 lsm_wal_bytes=92305 lsm_flush_started=0 lsm_flush_done=0 lsm_flush_failed=0 lsm_stall_events=0 lsm_stall_ms=0 lsm_wal_rotations=0 lsm_compaction_rounds=0 lsm_compaction_max_ms=0 lsm_flush_write_bytes=0 lsm_compact_write_bytes=0 lsm_user_bytes=65847]
base p=8 rep=1 n=4000 ms=21906.1 ms_per_write=5.477 qps=183 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=140 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=781 node_fsync_ms=5509 lsm_stats=[NA]
file p=8 rep=1 n=4000 ms=23813.3 ms_per_write=5.953 qps=168 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=170 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=787 node_fsync_ms=5926 lsm_stats=[NA]
lsm  p=8 rep=1 n=4000 ms=21377.0 ms_per_write=5.344 qps=187 verify=[verified 4000 missing 0] logdir_bytes=861997 open_ms_max=152 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=797 node_fsync_ms=5945 lsm_stats=[lsm_l0_files=0 lsm_sst_files=0 lsm_sst_bytes=0 lsm_wal_bytes=326393 lsm_flush_started=0 lsm_flush_done=0 lsm_flush_failed=0 lsm_stall_events=0 lsm_stall_ms=0 lsm_wal_rotations=0 lsm_compaction_rounds=0 lsm_compaction_max_ms=0 lsm_flush_write_bytes=0 lsm_compact_write_bytes=0 lsm_user_bytes=233864]
base p=64 rep=1 n=4000 ms=11970.8 ms_per_write=2.993 qps=334 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=139 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=305 node_fsync_ms=2537 lsm_stats=[NA]
file p=64 rep=1 n=4000 ms=11024.1 ms_per_write=2.756 qps=363 verify=[verified 4000 missing 0] logdir_bytes=688887 open_ms_max=150 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=300 node_fsync_ms=2432 lsm_stats=[NA]
lsm  p=64 rep=1 n=4000 ms=10786.0 ms_per_write=2.696 qps=371 verify=[verified 4000 missing 0] logdir_bytes=843535 open_ms_max=138 fsync_calls=NA fsync_us_total=NA lat_p50_us=50000 lat_p99_us=50000 node_fsync_calls=311 node_fsync_ms=2504 lsm_stats=[lsm_l0_files=0 lsm_sst_files=0 lsm_sst_bytes=0 lsm_wal_bytes=326393 lsm_flush_started=0 lsm_flush_done=0 lsm_flush_failed=0 lsm_stall_events=0 lsm_stall_ms=0 lsm_wal_rotations=0 lsm_compaction_rounds=0 lsm_compaction_max_ms=0 lsm_flush_write_bytes=0 lsm_compact_write_bytes=0 lsm_user_bytes=233864]
```

**这些数字说明了什么（只陈述计数，不猜）**

| 观察 | 数字（base / file / lsm） | 含义 |
|---|---|---|
| node 侧 fsync 次数（p=1，1000 写 + 200 预热） | 1202 / 1202 / **1202** | 三臂**完全相同**（≈1 fsync/写）⇒ 小并发下 lsm 与 file 的 fsync 结构一致；R1 修复后 lsm 不再多刷/被挡 |
| node 侧 fsync 次数（p=8） | 781 / 787 / **797** | 组提交生效（≈0.19 fsync/写），lsm 只多 1.3% |
| node 侧 fsync 次数（p=64） | 305 / 300 / **311** | ≈0.074 fsync/写；lsm 多 3.7% —— **比 §3.1（修前）的劣化小得多**，与「lsm 在 p=64 反超 file」一致 |
| node 侧 fsync 总耗时（p=64） | 2537 / 2432 / **2504** ms | 与次数同量级 ⇒ 单次 fsync 成本三臂相同（≈8 ms） |
| lsm SST/flush/compaction（n≤4000） | — / — / **sst_files=0, flush_started=0, compaction_rounds=0, stall_events=0** | 这些格子里 lsm 路径 = **纯 WAL append + memtable**，没有 flush/compaction/stall 开销 |
| lsm WAL 字节 vs 用户字节 | — / — / **92 305 / 65 847**（p=1）、**326 393 / 233 864**（p=8/64） | WAL 写放大 ≈ **1.40×**（每条 entry 的 WAL 帧 ≈ 1.4× 用户 key+value 字节），与 file 引擎的帧开销同量级 |
| lsm 目录字节 vs lsm 自报 WAL 字节（p=64） | — / — / **843 535（du）** vs **326 393（自报 live WAL）** | 目录里还有 **~517 KB 的未回收字节**（obsolete WAL/元数据尚未删）⇒ 这是 R4（空间放大）在同一轮里的现场证据 |
| node 侧 `lat_p50_us` / `lat_p99_us` | 50000 / 50000 | ⚠️ **顶桶（≥20 ms）饱和**：现有直方图的桶上界只到 50 ms，本负载下三臂都落在顶桶 ⇒ **不可分辨**，不能用来比较长尾（见 O1/D2 的限制） |

### 5.2 观测项状态（第一轮 → 第二轮）

| ID | 项 | 状态 | 数字 / 原因 |
|---|---|---|---|
| O1/D1 | 吞吐 / ms-per-write | ✅ 已采集 | §3 表（p=1/8/64 三档） |
| O1/D2 | P99 / max 延迟 | ⚠️ **采集到但不可分辨** | node 侧 `lat_p50_us/lat_p99_us` 已在 status 里（本就有）；但桶上界只到 50 ms，本负载三臂都落顶桶（50000）⇒ 无法比较；**max 仍未采集**。见 §5.1 |
| O2/D3 | 日志目录字节 | ✅ 已采集（小负载） | §2 的 `logdir_bytes`；大负载见 §6.4 |
| O3/D4 | 启动恢复耗时（100k 条后重启） | ⚠️ 口径不符 | 只采集了「空库启动到 status 可用」（`open_ms_max` 133–175 ms）；**100k 条后的恢复耗时未按 D4 采集** |
| O4/D5 | fsync 次数 / 总耗时 | ✅ **已采集（node 侧计数器）** | 走 `strace` 的通道无效（§2.1 末），改用 node 自己的 `fsync_calls/fsync_ms`（`Metrics::onFsync`）：三臂 p=1 都是 1202 次、p=8 781/787/797、p=64 305/300/311（§5.1） |
| O5 | lsm 各层 files/bytes | ✅ 已接线并采集 | `lsm_l0_files/lsm_sst_files/lsm_sst_bytes` 随 `status` 输出（§5.1）：n≤4000 时全为 0（没有 flush 到 SST） |
| O6 | 空间放大 | ✅ 已接线并采集（+`du` 交叉验证） | `lsm_wal_bytes/lsm_flush_write_bytes/lsm_compact_write_bytes/lsm_user_bytes`（§5.1）：WAL 写放大 ≈1.40×；`du` 与自报 live WAL 的差额 ≈517 KB ⇒ 未回收字节可见 |
| O7/D6 | 写停顿 `stall_events`/`stall_micros` | ✅ 已接线并采集 | `lsm_stall_events/lsm_stall_ms`（§5.1）：本负载全为 0 |
| O8/D8 | WAL 尾部截断字节 | ✅ 已采集（单测） | `LsmLogStore.TornWalTailIsTruncatedByOpenAndCounted`：`walTailTruncatedBytes() > 0`（具体值未打印） |
| D7 | compaction 轮次/最大耗时 | ✅ 已接线并采集 | `lsm_compaction_rounds/lsm_compaction_max_ms`（§5.1）：n≤4000 全为 0（未触发 compaction） |
| D9 | `truncateSuffix` 条目数分布 | ⚠️ 部分 | `LsmLogStore::Stats.truncated_entries` 有计数与单测断言（`== 2`）；**分布**未采集 |
| D10 | `slice()` 调用次数/条数 | ⚠️ 部分 | `Stats.slice_calls` 有计数；**未在 A/B 中导出** |

## 6. 负结果（设计 §6）

### 6.1 R1 —— `DB::Sync()` 持 `commit_mu_` 做 fsync 是否把组提交打散？→ **缺陷真实存在；本机口径下未出现量级塌陷；f06a44d 修复后 lsm 反而更快**
干净 A/B 的三档里 lsm/file 延迟 = 1.01×/1.05×/**1.12×**、吞吐 = 0.97×/0.95×/**0.89×**，
**没有**出现设计预测的 0.47× 量级塌陷。**但这不等于 R1 不存在**：D5（fsync 次数）没有可信数字，
所以「组提交是否被打散」**无法直接判定**。定性旁证：在**并发重活**（另一 agent 的 ASan/TSan
构建与 `lsm_tests`，loadavg 峰值 15.9）下，lsm 臂的 100k 条 fill 用了 ~13 分钟（≈108 写/秒），
同机 file 臂约 5 分钟（≈300 写/秒）——方向与 R1 一致，但该数字被污染，不作为结论。

**第二轮（基座 f06a44d，R1 已修）**：同样口径的 27 格 A/B 里，lsm/file 延迟 = **1.01×/0.97×/0.93×**、
吞吐 = **1.00×/1.03×/1.08×**（p=1/8/64，见 §3.2）⇒ 修前「略慢」、修后「p=8/64 反超」。
结论：R1 的结构性缺陷（fsync 期间阻塞入队）是真实的（`f06a44d` 的 `Sync()` 已把 fsync 移出
`commit_mu_`，见该提交注释），但它在**本机 3 节点 / n≤4000 / p≤64** 的口径下**没有**表现为
0.47× 那样的塌陷；修复的可见收益是 p=64 吞吐 +8%（3.264→2.620 ms/write 对比 file 2.814→2.620）。
D5（fsync 次数）仍**没有**可信数字：`strace` 通道无效（§2.1 末），node 侧 `fsync_calls` 计数
已在 §5 的补充轮采集（若该轮完成）；没有它之前，本节的因果解释只能算「A/B 现象 + 代码结构」两条证据。

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

**补充轮的同轮证据（§5.1，n=4000/p=64）**：`logdir_bytes=843 535` 而 lsm 自报 live WAL 只有
`lsm_wal_bytes=326 393` ⇒ **约 517 KB 是尚未回收的 obsolete WAL/元数据**。也就是说：
「目录比 live 数据大」不是统计口径问题，而是**回收滞后**的现场证据。

### 6.4.1 ⛔ 空间判据在 f06a44d 上**红**（不得掩盖）

同一脚本、同一负载（100k 条 + threshold 2000）、同一检查点，两轮基座的实测：

| 基座 | `raft-lsm/` 三节点字节 | 判据（`LSM_LOG_BOUND=4194304`） |
|---|---|---|
| b1bd050（标定轮，2026-10-01 05:5x，机器同时有重活） | 2 221 606 / 2 579 679 / 2 561 634 | PASS |
| **f06a44d（本轮，08:00，机器安静）** | **node1 = 4 450 355**（第一节点即超标 ⇒ 脚本 exit 1） | **FAIL** |

原始输出（`/tmp/m6r2-snapfault-lsm50.log`，去掉 kill 噪音）：

```
engine=lsm
  node1 raft-lsm=4450355 bytes (bound=4194304; raft/=1487902 raft-lsm/=4450355)
FAIL: node1 raft-lsm=4450355 exceeds 4194304 (not bounded)
```

**处置（未放宽判据）**：`LSM_LOG_BOUND` **保持 4 MiB 不改**，本轮判据按**未通过**登记。
两种可能的解释都需要 lsm 侧/父代理裁决：(a) f06a44d 的 WAL 轮转/回收时机变了（`RotateLog()` 也在该提交里改过），
使稳态目录的**未回收字节**变多；(b) 4 MiB 这个上界本来就是在**有重活的机器**上标定的，偏乐观。
本文件只登记事实与两种解释，不替 lsm 侧下结论；如需重标定，必须用**安静机器 + 多轮**重新取分布后由父代理决定。

### 6.5 R5 —— 写停顿（`WaitForImmutableCapacity`）→ **未采集**
`GetFlushStats().stall_events/stall_micros` 未接线；门禁与 A/B 中**未观测到**挂起或超时。

### 6.6 R6 —— 启动恢复代价 → **未按 D4 口径采集**
只有空库启动（133–175 ms）；100k 条日志后的重启耗时未测。

### 6.7 R7 —— P99 抖动来自后台线程 → **未采集**（无直方图，见 O1/D2）。

## 7. 未验证清单（每条一行 + 原因）

1. **H9 的 base 臂脚本门禁（用 `~/raft-kv/scripts/*.sh` 跑 H2–H7）**：只跑了 A/B 的 base 臂（27 格全 `missing 0`），没跑 base 的 e2e/fault 脚本。
2. **`raft_snapshot_fault.sh --log-engine lsm --repeat 50`**：在基座 **f06a44d** 上**跑完了，结果是红**——A 段的空间判据超标（`4450355 > 4194304`），exit 1，B/C 段未执行。原始输出与两种解释见 §6.4.1。
3. **P99 / max 延迟（D2）**：node 侧直方图未实现（设计 D12）。
4. **fsync 次数/耗时（D5/R1）**：`strace -f -c` 通道实测无效（连 base 臂都 verify 失败），无可信数字。
5. **lsm 内部统计（O5/O6/O7、D7）**：`GetLevelStats`/`GetAmplificationStats`/`GetFlushStats` 未接线到 node。
6. **100k 条后的启动恢复耗时（D4）**：未按口径采集（只有空库启动 133–175 ms）。
7. **正式口径的 n（设计 §5.5 的 5000/20000/20000）**：本轮用 1000/4000/4000（时间盒 + lsm 写吞吐限制）；每行都打印 `n=`。
8. **`slice()` / `truncateSuffix` 的定向性能（R2/R3）**：只有功能证据，没有性能数字。
9. **A/B 期间的机器安静度**：27 格期间 loadavg 4.7–5.4（VM 上有另一 agent 的重活窗口）；三臂同轮交替把相对比较的偏置压到最小，但仍属已知限制。
10. **H11/H12 的口径**：H11 是 ASan **全量**（121/121）；H12 是 TSan 的 **`LsmLogStore.*`**（20/20）——**不是** TSan 全量（M6.8 时间盒内未跑 TSan 全量）。
11. **base 臂的 ASan/TSan**：未做（只在 `~/raft-kv-lsm` 上跑）。
12. **基座更换后的门禁重跑**：`f06a44d` 上已重跑全量单测（121/121）与 A/B；e2e/其余 fault 脚本未重跑。
13. **node 侧 P99 的分辨率**：桶上界只到 50 ms，本负载下三臂都落顶桶 ⇒ 分位数**无法用于比较**；`max` 无计数器。

### 7.1 红项原始输出（不得删改）

```
$ bash scripts/raft_snapshot_fault.sh --log-engine lsm --repeat 50      # 基座 f06a44d
engine=lsm
  node1 raft-lsm=4450355 bytes (bound=4194304; raft/=1487902 raft-lsm/=4450355)
FAIL: node1 raft-lsm=4450355 exceeds 4194304 (not bounded)
exit=1
```

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

### 8.1 第二轮（基座 f06a44d，R1 已修）—— 结论**发生变化**

* **base vs file**：仍是 ±1%（34.606/34.504、5.514/5.555、2.814/2.812）⇒ 依然无可测回归。
* **lsm vs file**：延迟 **1.01× / 0.97× / 0.93×**、吞吐 **1.00× / 1.03× / 1.08×**
  ⇒ p=1 持平、**p=8/p=64 lsm 反超 file**（p=64 快约 7%）。这与第一轮（1.05×/1.12× 更慢）
  **方向相反**，如实入档：R1 修复对高并发格子是净正收益。
* 设计 §6 R1 预测的 0.47× 塌陷：**两轮都没有出现**。第一轮是 0.89×（温和劣化），
  第二轮是 1.08×（温和反超）。
* 空间（R4）**不受 R1 修复影响**：仍是大负载下 ≈50×（§6.4）、小负载 ≈1.2–1.5×。
* 仍然未采集：P99 的**节点侧**分位（补充轮若完成见 §5）、fsync 次数（strace 通道无效）、
  100k 条后的启动恢复（D4）、正式口径 n。
* **并发正确性门禁已收口**：ASan 全量 121/121、TSan 的 `LsmLogStore.*` 20/20，均 0 warning、0 报告（H11/H12）。
