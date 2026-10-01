#!/usr/bin/env bash
# M6 A/B 基准：三个臂同脚本交替测量（设计 docs/m6-design.md §5）
#   base = ~/raft-kv/build/bin（远端基线 1463620，一个字节都没改）
#   file = 本仓 build/bin，默认 FileLogStore
#   lsm  = 本仓 build/bin --log-engine lsm
# 纪律（照搬 bench_m5_ab.sh §2-4）：
#   * 同一脚本内交替 base → file → lsm → base …；每档先 200 写预热（丢弃）
#   * 每档重复 REPS 次取中位数；每次 fill 之后必须 verify，缺失 missing 0 即整轮作废
#   * **不设比值硬门禁**（M6 是适配不是提速；D13）
# 行格式（前缀列冻结，只允许行尾追加）：
#   eng=.. p=.. rep=.. n=.. ms=.. ms_per_write=.. qps=.. verify=[..] logdir_bytes=.. open_ms_max=.. fsync_calls=.. fsync_us_total=..
#   M6.r2 追加列：lat_p50_us=.. lat_p99_us=.. node_fsync_calls=.. node_fsync_ms=.. lsm_stats=[..]
#     * lat_p50_us/lat_p99_us/node_fsync_calls/node_fsync_ms 来自 **node 侧** metrics（同一轮同一次 fill 之后采样）；
#     * lsm_stats 只在 lsm 臂出现（file/base 臂为 NA —— 不是 0）；
#     * 旧的 fsync_calls/fsync_us_total 仍是 --strace 通道（实测污染系统，默认 NA）。
# 用法: scripts/bench_m6_ab.sh [--quick] [--repeats N] [--pipelines "1 8 64"] [--strace] [--only base,file,lsm]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_BIN="${BASE_BIN:-$HOME/raft-kv/build/bin}"
REPS=3
QUICK=0
STRACE=0
ONLY="base,file,lsm"
PIPELINES="1 8 64"
THRESHOLD=5000

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) QUICK=1; shift;;
    --repeats) REPS="$2"; shift 2;;
    --pipelines) PIPELINES="$2"; shift 2;;
    --strace) STRACE=1; shift;;
    --only) ONLY="$2"; shift 2;;
    --base-bin) BASE_BIN="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[[ $QUICK == 1 ]] && REPS=1

for eng in base file lsm; do
  if [[ ",$ONLY," == *",$eng,"* ]]; then
    case $eng in
      base) BIN="$BASE_BIN";;
      *)    BIN="$ROOT/build/bin";;
    esac
    [[ -x "$BIN/raftkv_raft_node" ]] || { echo "missing binary for arm $eng: $BIN/raftkv_raft_node" >&2; exit 1; }
  fi
done

WORK="$(mktemp -d)"
LOGDIR="/tmp/m6bench-logs"
mkdir -p "$LOGDIR"
RAW="$LOGDIR/raw-$(date +%s).txt"
{
  echo "# m6 A/B raw log  $(date -Is)"
  echo "# uname=$(uname -sr) nproc=$(nproc) load=$(cut -d' ' -f1 /proc/loadavg) fs=$(df -T "$WORK" | tail -1 | awk '{print $2}') tmpfs=$([[ "$WORK" == /tmp/* ]] && echo maybe || echo no)"
  echo "# base_bin=$BASE_BIN reps=$REPS quick=$QUICK pipelines=$PIPELINES strace=$STRACE threshold=$THRESHOLD"
} > "$RAW"

n_for() { # <pipeline>
  if [[ $QUICK == 1 ]]; then
    case "$1" in 1) echo 200;; *) echo 500;; esac
  else
    case "$1" in 1) echo 1000;; *) echo 4000;; esac
  fi
}

arg_for() { # <eng>
  case "$1" in
    lsm) echo "--log-engine lsm";;
    *)   echo "";;
  esac
}

probe() { # <eng> <pipeline> <rep>
  local eng="$1" p="$2" rep="$3"
  local binvar="BIN_FOR_$eng"
  local bin="${!binvar}"
  local n; n="$(n_for "$p")"
  local base=$(( 23000 + RANDOM % 3000 ))
  local peers="1=127.0.0.1:$base,2=127.0.0.1:$((base+1)),3=127.0.0.1:$((base+2))"
  local dir="$WORK/$eng-p$p-r$rep"; mkdir -p "$dir"
  local pids=()
  local openmax=0
  local extra; extra="$(arg_for "$eng")"
  for id in 1 2 3; do
    local t0 t1
    t0=$(date +%s.%N)
    if [[ $STRACE == 1 ]]; then
      # shellcheck disable=SC2086
      strace -f -c -e trace=fsync,fdatasync -o "$dir/n$id.strace" \
        "$bin/raftkv_raft_node" --id "$id" --port $((base+id-1)) --peers "$peers" \
        --data-dir "$dir/n$id" --snapshot-threshold "$THRESHOLD" $extra \
        >"$dir/n$id.log" 2>&1 &
    else
      # shellcheck disable=SC2086
      "$bin/raftkv_raft_node" --id "$id" --port $((base+id-1)) --peers "$peers" \
        --data-dir "$dir/n$id" --snapshot-threshold "$THRESHOLD" $extra \
        >"$dir/n$id.log" 2>&1 &
    fi
    pids+=($!)
    local cli="$bin/raftkv_raft_cli --peers $peers --host 127.0.0.1"
    local up=1
    for _ in $(seq 1 200); do
      if $cli --port $((base+id-1)) status >/dev/null 2>&1; then up=0; break; fi
      sleep 0.02
    done
    t1=$(date +%s.%N)
    local oms; oms=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%d", (b-a)*1000}')
    (( oms > openmax )) && openmax=$oms
    if (( up != 0 )); then
      echo "$eng p=$p rep=$rep NO_START node$id" | tee -a "$RAW"
      for pid in "${pids[@]}"; do kill -9 "$pid" 2>/dev/null || true; done
      return 1
    fi
  done
  local cli="$bin/raftkv_raft_cli --peers $peers --host 127.0.0.1"
  local leaderport=0
  for _ in $(seq 1 300); do
    for id in 1 2 3; do
      if $cli --port $((base+id-1)) status 2>/dev/null | grep -q 'role=leader'; then
        leaderport=$((base+id-1)); break
      fi
    done
    [[ $leaderport -ne 0 ]] && break
    sleep 0.1
  done
  if [[ $leaderport -eq 0 ]]; then
    echo "$eng p=$p rep=$rep NO_LEADER" | tee -a "$RAW"
    for pid in "${pids[@]}"; do kill -9 "$pid" 2>/dev/null || true; done
    return 1
  fi
  sleep 1

  local t0 t1 ms lat qps verify
  $cli --port "$leaderport" fill 200 --pipeline 1 >/dev/null 2>&1 || true   # 预热（丢弃）
  t0=$(date +%s.%N)
  $cli --port "$leaderport" fill "$n" --pipeline "$p" >/dev/null
  t1=$(date +%s.%N)
  verify=$($cli --port "$leaderport" verify "$n" | tail -1)
  ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", (b-a)*1000}')
  lat=$(awk -v n="$n" -v ms="$ms" 'BEGIN{printf "%.3f", ms/n}')
  qps=$(awk -v n="$n" -v ms="$ms" 'BEGIN{ if (ms>0) printf "%.0f", n*1000/ms; else print 0 }')

  # O2/D3：按引擎取日志目录字节（file/base = raft/raft.log；lsm = raft-lsm/ 目录）。
  local ldb=0
  for id in 1 2 3; do
    local leaf="$dir/n$id/raft/raft.log" v=0
    if [[ "$eng" == "lsm" ]]; then leaf="$dir/n$id/raft-lsm"; fi
    if [[ -d "$leaf" ]]; then v=$(du -sb "$leaf" 2>/dev/null | cut -f1); else v=$(stat -c%s "$leaf" 2>/dev/null || echo 0); fi
    ldb=$(( ldb + ${v:-0} ))
  done

  # D5 / D2 / O5-O7：leader 的 node 侧指标（M5.1 的 statusFragment 已有 p50/p99/fsync 计数；
  # lsm 臂还会带 lsm_* 引擎统计）。**同一轮**采集，与上面的 ms/qps 是同一次 fill 的结果。
  local st p50 p99 nfc nfms lsmstats
  st=$($cli --port "$leaderport" status | tail -1)
  tok() { printf '%s\n' "$st" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
  p50=$(tok lat_p50_us); p99=$(tok lat_p99_us)
  nfc=$(tok fsync_calls); nfms=$(tok fsync_ms)
  lsmstats=$(printf '%s\n' "$st" | tr ' ' '\n' | sed -n 's/^\(lsm_[a-z0-9_]*=.*\)$/\1/p' | tr '\n' ' ')

  # D5（备选通道）：仅 --strace 时的进程级 fsync 聚合（实测该通道会污染系统，默认关闭）。
  local fc="NA" fu="NA"
  if [[ $STRACE == 1 ]]; then
    local lid=$(( leaderport - base + 1 ))
    if [[ -f "$dir/n$lid.strace" ]]; then
      read -r fc fu < <(awk '$NF=="fsync"||$NF=="fdatasync"{ n=NF; if (n>=6) {c+=$(n-2); s+=$(n-4)} else {c+=$(n-1); s+=$(n-3)} } END{printf "%d %.0f", c, s*1000000}' "$dir/n$lid.strace")
      fc=${fc:-NA}; fu=${fu:-NA}
    fi
  fi

  echo "$eng p=$p rep=$rep n=$n ms=$ms ms_per_write=$lat qps=$qps verify=[$verify] logdir_bytes=$ldb open_ms_max=$openmax fsync_calls=$fc fsync_us_total=$fu lat_p50_us=${p50:-NA} lat_p99_us=${p99:-NA} node_fsync_calls=${nfc:-NA} node_fsync_ms=${nfms:-NA} lsm_stats=[${lsmstats:-NA}]" | tee -a "$RAW"
  for pid in "${pids[@]}"; do kill -9 "$pid" 2>/dev/null || true; done
  sleep 0.5
  [[ "$verify" == *"missing 0"* ]] || { echo "VERIFY_FAILED $eng p=$p" >&2; return 1; }
  return 0
}

BIN_FOR_base="$BASE_BIN"
BIN_FOR_file="$ROOT/build/bin"
BIN_FOR_lsm="$ROOT/build/bin"

echo
echo "== M6 A/B（每格 = $REPS 次的中位数；base=$BASE_BIN）=="
for p in $PIPELINES; do
  for rep in $(seq 1 "$REPS"); do
    for eng in base file lsm; do
      [[ ",$ONLY," == *",$eng,"* ]] || continue
      probe "$eng" "$p" "$rep" || true
    done
  done
done

medians() { # <engine> <pipeline> <field>
  awk -v e="$1" -v want="$2" -v f="$3" '
    $1==e {
      hit=0; v=""
      for (i=1;i<=NF;i++) { n=split($i,kv,"="); if (n<2) continue
        if (kv[1]=="p" && kv[2]==want) hit=1
        if (kv[1]==f) v=kv[2] }
      if (hit && v!="") print v
    }' "$RAW" | sort -g | awk '{a[NR]=$1} END{ if (NR==0) {print "n/a"; exit} if (NR%2) printf "%.3f", a[(NR+1)/2]; else printf "%.3f", (a[NR/2]+a[NR/2+1])/2 }'
}

echo
echo "| pipeline | n | base ms/w | file ms/w | lsm ms/w | base qps | file qps | lsm qps | lsm/file 延迟 | lsm/file 吞吐 | verify | 判定 |"
echo "|---|---|---|---|---|---|---|---|---|---|---|---|"
FAILED=0
for p in $PIPELINES; do
  n="$(n_for "$p")"
  bl=$(medians base "$p" ms_per_write); fl=$(medians file "$p" ms_per_write); ll=$(medians lsm "$p" ms_per_write)
  bq=$(medians base "$p" qps);         fq=$(medians file "$p" qps);         lq=$(medians lsm "$p" qps)
  vr="missing 0"; grep -q "p=$p .*verify=\[.*missing 0.*\]" "$RAW" || vr="见原始行"
  grep -q "missing 0" "$RAW" || FAILED=1
  lr="n/a"; qr="n/a"
  if [[ "$ll" != "n/a" && "$fl" != "n/a" ]]; then lr=$(awk -v a="$fl" -v b="$ll" 'BEGIN{ if(a>0) printf "%.2fx", b/a; else print "n/a" }'); fi
  if [[ "$lq" != "n/a" && "$fq" != "n/a" ]]; then qr=$(awk -v a="$fq" -v b="$lq" 'BEGIN{ if(a>0) printf "%.2fx", b/a; else print "n/a" }'); fi
  echo "| $p | $n | $bl | $fl | $ll | $bq | $fq | $lq | $lr | $qr | $vr | **不设比值硬门禁（D13）** |"
done
echo
echo "raw log: $RAW"
echo "（硬门禁仅：每格 verify 必须含 missing 0；任一格缺失 ⇒ 退出码 1）"
rm -rf "$WORK"
exit "$FAILED"
