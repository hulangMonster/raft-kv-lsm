#!/usr/bin/env bash
# M6.10.5 (2)：两套 compact 交互驱动 —— raft 前缀压缩（周期快照 + log_.compact()）
# 与 lsm 后台 compaction **同时**真跑。
#
# 为什么需要它：n<=4000 的常规门禁里 lsm_compaction_rounds 恒为 0，这条交互从没被压到。
# 这里把 lsm 的 memtable 调小（RAFTKV_LSM_WRITE_BUFFER_BYTES，默认 64 KiB），
# 于是每次 flush 产出的小 L0 文件远快于后台 compaction 消化，默认 L0 触发阈值（4）就会真的触发。
#
# 正向标记（缺一即失败，避免空绿）：
#   * lsm_compaction_rounds > 0  —— 引擎自报的后台 compaction 轮次
#   * snapshot_index > 0         —— raft 侧前缀压缩真的发生（log_.compact 的前置）
# 注意：这两个引擎计数是**每会话**的，重启后的新进程自然从 0 开始，所以正向标记取
#       **重启前各轮的最大值**（MAX_CR/MAX_SI），重启后只看 verify 与 snapshot_index。
# 判据：
#   * 每轮 verify 必须 `missing 0`
#   * kill -9 全部节点 -> 重启 -> verify 仍 `missing 0`（被压缩掉的区间不得复活）
#
# 用法：scripts/raft_two_compactions.sh [--rounds N] [--n N] [--pipeline P] [--threshold T]
#                                      [--write-buffer-bytes B] [--bin DIR]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/bin"
ROUNDS=6
N=20000
PIPELINE=64
THRESHOLD=1000
export RAFTKV_LSM_WRITE_BUFFER_BYTES="${RAFTKV_LSM_WRITE_BUFFER_BYTES:-65536}"
WB_LABEL="$RAFTKV_LSM_WRITE_BUFFER_BYTES"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --n) N="$2"; shift 2;;
    --pipeline) PIPELINE="$2"; shift 2;;
    --threshold) THRESHOLD="$2"; shift 2;;
    --write-buffer-bytes) RAFTKV_LSM_WRITE_BUFFER_BYTES="$2"; export RAFTKV_LSM_WRITE_BUFFER_BYTES; WB_LABEL="$2"; shift 2;;
    --bin) BIN="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[[ -x "$BIN/raftkv_raft_node" ]] || { echo "missing node binary in $BIN" >&2; exit 1; }

WORK="$(mktemp -d)"
BASE=$((28000 + RANDOM % 1000))
PEERS="1=127.0.0.1:$BASE,2=127.0.0.1:$((BASE+1)),3=127.0.0.1:$((BASE+2))"
declare -a PIDS
cleanup() {
  for id in 1 2 3; do [[ -n "${PIDS[$id]:-}" ]] && kill -9 "${PIDS[$id]}" 2>/dev/null; done
  rm -rf "$WORK"
}
trap cleanup EXIT

port_of() { echo $((BASE + $1 - 1)); }
cli() { "$BIN/raftkv_raft_cli" --peers "$PEERS" "$@"; }
field() { { cli --host 127.0.0.1 --port "$(port_of "$1")" status 2>/dev/null | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; } || true; }
start_node() {
  local id=$1
  RAFTKV_LSM_WRITE_BUFFER_BYTES="$RAFTKV_LSM_WRITE_BUFFER_BYTES" \
  "$BIN/raftkv_raft_node" --id "$id" --port "$(port_of "$id")" --peers "$PEERS" \
    --data-dir "$WORK/node$id" --snapshot-threshold "$THRESHOLD" \
    --log-engine lsm --state-engine lsm >"$WORK/node$id.log" 2>&1 &
  PIDS[$id]=$!
}
leader_port() {
  for id in 1 2 3; do
    cli --host 127.0.0.1 --port "$(port_of "$id")" status 2>/dev/null | grep -q 'role=leader' && { echo "$(port_of "$id")"; return 0; }
  done
  return 1
}
require_leader_port() {
  local p
  for _ in $(seq 1 300); do p="$(leader_port)" && { echo "$p"; return 0; }; sleep 0.1; done
  return 1
}
leader_id() {
  for id in 1 2 3; do
    cli --host 127.0.0.1 --port "$(port_of "$id")" status 2>/dev/null | grep -q 'role=leader' && { echo "$id"; return 0; }
  done
  return 1
}

echo "# two-compactions probe bin=$BIN rounds=$ROUNDS n=$N pipeline=$PIPELINE threshold=$THRESHOLD write_buffer_bytes=$RAFTKV_LSM_WRITE_BUFFER_BYTES"
echo "# $(date -Is) uname=$(uname -sr) nproc=$(nproc) load=$(cut -d' ' -f1 /proc/loadavg)"

for id in 1 2 3; do start_node "$id"; done
LP="$(require_leader_port)" || { echo "FAIL: no leader"; exit 1; }
LID="$(leader_id)"
echo "# leader node=$LID"

fail=0
MAX_CR=0
MAX_SI=0
cli --host 127.0.0.1 --port "$LP" fill 200 --pipeline "$PIPELINE" >/dev/null 2>&1

for round in $(seq 1 "$ROUNDS"); do
  cli --host 127.0.0.1 --port "$LP" fill "$N" --pipeline "$PIPELINE" >/dev/null 2>&1 || { echo "TCOMP round=$round fill=FAIL"; fail=1; break; }
  v="$(cli --host 127.0.0.1 --port "$LP" verify "$N" 2>&1 | grep -o 'missing [0-9]*' | head -1)"
  [[ "$v" == "missing 0" ]] || fail=1
  cli --host 127.0.0.1 --port "$LP" snapshot >/dev/null 2>&1 || true
  si_now="$(field "$LID" snapshot_index)"
  cr_now="$(field "$LID" lsm_compaction_rounds)"
  printf 'TCOMP round=%s verify=[%s] snapshot_index=%s lsm_compaction_rounds=%s lsm_flush_done=%s lsm_sst_files=%s last_applied=%s\n' \
    "$round" "${v:-none}" "${si_now:-none}" "${cr_now:-none}" \
    "$(field "$LID" lsm_flush_done)" "$(field "$LID" lsm_sst_files)" "$(field "$LID" last_applied)"
  if [[ "${cr_now:-}" =~ ^[0-9]+$ ]] && (( cr_now > MAX_CR )); then MAX_CR=$cr_now; fi
  if [[ "${si_now:-}" =~ ^[0-9]+$ ]] && (( si_now > MAX_SI )); then MAX_SI=$si_now; fi
done

# ---- 带重启的档：kill -9 全部 -> 重启 -> verify（被压缩区间不得复活）----
echo "# restart phase (kill -9 all, restart, verify)"
for id in 1 2 3; do kill -9 "${PIDS[$id]}" 2>/dev/null || true; PIDS[$id]=""; done
sleep 1
for id in 1 2 3; do start_node "$id"; done
LP2="$(require_leader_port)" || { echo "FAIL: no leader after restart"; exit 1; }
LID2="$(leader_id)"
v2="$(cli --host 127.0.0.1 --port "$LP2" verify "$N" 2>&1 | grep -o 'missing [0-9]*' | head -1)"
[[ "$v2" == "missing 0" ]] || fail=1
printf 'TCOMP post_restart verify=[%s] snapshot_index=%s lsm_compaction_rounds=%s lsm_flush_done=%s last_applied=%s\n' \
  "${v2:-none}" "$(field "$LID2" snapshot_index)" "$(field "$LID2" lsm_compaction_rounds)" \
  "$(field "$LID2" lsm_flush_done)" "$(field "$LID2" last_applied)"

# 正向标记（重启前各轮最大值）：没有它们说明这条交互根本没被压到。
if (( MAX_CR == 0 )); then echo "POSITIVE_MARKER_MISSING lsm_compaction_rounds_max=0"; fail=1; fi
if (( MAX_SI == 0 )); then echo "POSITIVE_MARKER_MISSING snapshot_index_max=0"; fail=1; fi
echo "TCOMP_POSITIVE_MARKERS lsm_compaction_rounds_max=$MAX_CR snapshot_index_max=$MAX_SI write_buffer_bytes=$WB_LABEL"
if (( fail == 0 )); then echo "[TWO_COMPACTIONS_OK]"; else echo "[TWO_COMPACTIONS_FAIL]"; fi
echo "# done $(date -Is)"
exit $fail
