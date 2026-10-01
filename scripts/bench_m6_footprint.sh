#!/usr/bin/env bash
# M6.r3：lsm 引擎日志**有界性**探针（同一份负载、同一探针、两个 lsm 基座对照）。
#
# 形状贴近 scripts/raft_snapshot_fault.sh：3 节点 + --snapshot-threshold 2000（周期性快照/compact），
# 写入按阶梯累加到 N ∈ {25000,50000,100000,200000}，每档在**稳定点**采样：
#   每 5s 采样一次 du -sb / 逐文件字节 / lsm 自报计数，直到「连续 3 次完全相同」或 60s 上限。
# 最后再 kill -9 全部节点 → 重启 → 再采一次（「带重启」的档）。
#
# 用法: scripts/bench_m6_footprint.sh --bin DIR --label NAME [--pipeline 64] [--threshold 2000]
#                                     [--steps "25000 25000 50000 100000"] [--engine lsm|file]
set -uo pipefail

BIN=""; LABEL="unknown"; PIPELINE=64; THRESHOLD=2000; STEPS="25000 25000 50000 100000"; ENGINE=lsm
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin) BIN="$2"; shift 2;;
    --label) LABEL="$2"; shift 2;;
    --pipeline) PIPELINE="$2"; shift 2;;
    --threshold) THRESHOLD="$2"; shift 2;;
    --steps) STEPS="$2"; shift 2;;
    --engine) ENGINE="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[[ -x "$BIN/raftkv_raft_node" ]] || { echo "missing node binary in $BIN" >&2; exit 1; }

WORK="$(mktemp -d)"
BASE=$((24000 + RANDOM % 2000))
PEERS="1=127.0.0.1:$BASE,2=127.0.0.1:$((BASE+1)),3=127.0.0.1:$((BASE+2))"
declare -a PIDS
cleanup() {
  for id in 1 2 3; do [[ -n "${PIDS[$id]:-}" ]] && kill -9 "${PIDS[$id]}" 2>/dev/null || true; done
  rm -rf "$WORK"
}
trap cleanup EXIT

port_of() { echo $((BASE + $1 - 1)); }
cli() { "$BIN/raftkv_raft_cli" --peers "$PEERS" "$@"; }
field() { { cli --host 127.0.0.1 --port "$(port_of "$1")" status 2>/dev/null | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; } || true; }

start_node() { # <id>
  local id=$1
  "$BIN/raftkv_raft_node" --id "$id" --port "$(port_of "$id")" --peers "$PEERS" \
    --data-dir "$WORK/node$id" --snapshot-threshold "$THRESHOLD" --log-engine "$ENGINE" \
    >"$WORK/node$id.log" 2>&1 &
  PIDS[$id]=$!
}

wait_all_up() {
  for id in 1 2 3; do
    for _ in $(seq 1 300); do
      [[ -n "$(field "$id" role)" ]] && break
      sleep 0.1
    done
  done
}

find_leader_port() {
  for id in 1 2 3; do
    if cli --host 127.0.0.1 --port "$(port_of "$id")" status 2>/dev/null | grep -q 'role=leader'; then
      echo "$(port_of "$id")"; return 0
    fi
  done
  return 1
}
require_leader_port() {
  for _ in $(seq 1 300); do
    local p; if p="$(find_leader_port)"; then echo "$p"; return 0; fi
    sleep 0.1
  done
  return 1
}
wait_applied() { # <target>
  for _ in $(seq 1 6000); do
    local ok=1
    for id in 1 2 3; do
      local v; v="$(field "$id" last_applied)"
      [[ "$v" =~ ^[0-9]+$ ]] && (( v >= $1 )) || ok=0
    done
    (( ok == 1 )) && return 0
    sleep 0.05
  done
  return 1
}

leaf_of() { # <id>
  if [[ "$ENGINE" == "lsm" ]]; then echo "$WORK/node$1/raft-lsm"; else echo "$WORK/node$1/raft"; fi
}

sample_once() { # <id> -> "<bytes> <files>"
  local leaf; leaf="$(leaf_of "$1")"
  local bytes files
  bytes=$(du -sb "$leaf" 2>/dev/null | cut -f1); bytes=${bytes:-0}
  files=$(find "$leaf" -maxdepth 1 -type f -printf '%f:%s,' 2>/dev/null | sed 's/,$//')
  echo "$bytes $files"
}

stable_sample() { # <id> <n> <tag>: 每 5s 采一次，3 次连续不变或 60s 上限
  local id=$1 n=$2 tag=$3
  local prev="" same=0 k=0 bytes files
  while (( k < 12 )); do
    read -r bytes files < <(sample_once "$id")
    k=$((k+1))
    if [[ "$bytes" == "$prev" ]]; then same=$((same+1)); else same=0; fi
    prev="$bytes"
    (( same >= 2 )) && break
    sleep 5
  done
  local stable=no; (( same >= 2 )) && stable=yes
  local wal sst flush comp
  wal="$(field "$id" lsm_wal_bytes)"; sst="$(field "$id" lsm_sst_bytes)"
  flush="$(field "$id" lsm_flush_done)"; comp="$(field "$id" lsm_compaction_rounds)"
  printf 'STEP label=%s n=%s tag=%s node=%s stable=%s samples=%s leaf_bytes=%s files=[%s] lsm_wal_bytes=%s lsm_sst_bytes=%s lsm_flush_done=%s lsm_compaction_rounds=%s\n' \
    "$LABEL" "$n" "$tag" "$id" "$stable" "$k" "$bytes" "$files" "${wal:-NA}" "${sst:-NA}" "${flush:-NA}" "${comp:-NA}" >&2
  echo "$bytes"
}

echo "# footprint probe: label=$LABEL bin=$BIN engine=$ENGINE pipeline=$PIPELINE threshold=$THRESHOLD steps=[$STEPS]"
echo "# $(date -Is) uname=$(uname -sr) nproc=$(nproc) load=$(cut -d' ' -f1 /proc/loadavg)"
for id in 1 2 3; do start_node "$id"; done
wait_all_up
LP="$(require_leader_port)" || { echo "FAIL: no leader"; exit 1; }
sleep 1

CUM=0
for step in $STEPS; do
  CUM=$(( CUM + step ))
  cli --host 127.0.0.1 --port "$LP" fill "$step" --pipeline "$PIPELINE" >/dev/null || { echo "FAIL: fill $step"; exit 1; }
  wait_applied "$CUM" || { echo "FAIL: not applied to $CUM"; exit 1; }
  max_bytes=0
  for id in 1 2 3; do
    b="$(stable_sample "$id" "$CUM" pre-restart | tail -1)"
    (( b > max_bytes )) && max_bytes=$b
  done
  echo "SUMMARY label=$LABEL n=$CUM max_leaf_bytes=$max_bytes"
done

# ---- 带重启的档：kill -9 全部 → 重启 → 稳定点再采 ----
echo "# restart phase (kill -9 all, restart, re-sample)"
for id in 1 2 3; do kill -9 "${PIDS[$id]}" 2>/dev/null || true; PIDS[$id]=""; done
sleep 1
for id in 1 2 3; do start_node "$id"; done
wait_all_up
CUM_AFTER="$(field 1 last_applied)"
LP="$(require_leader_port || true)"
for id in 1 2 3; do
  stable_sample "$id" "${CUM_AFTER:-0}" post-restart >/dev/null
done
rmax=0
for id in 1 2 3; do
  b="$(sample_once "$id" | cut -d' ' -f1)"; (( b > rmax )) && rmax=$b
done
echo "SUMMARY label=$LABEL n=${CUM_AFTER:-0} max_leaf_bytes_post_restart=$rmax"
echo "# done $(date -Is)"
