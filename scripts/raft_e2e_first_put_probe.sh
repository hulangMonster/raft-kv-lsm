#!/usr/bin/env bash
# M6.10.5 (8)：`raft_e2e.sh` 既有 flake 的**聚焦**复现/验证探针。
#
# 只跑整脚本里真正出问题的那条路径：起 3 节点 -> wait_leader -> **首次 put**，
# 不做后续步骤。这样「修前会失败 / 修后不失败」能在同一个 harness 里对照，
# 而不必反复跑整脚本（每次 ~5s，整脚本要 4 个步骤 + kill -9）。
#
# 用法：scripts/raft_e2e_first_put_probe.sh [--rounds N] [--mode raw|retry]
#   raw   ：原样逻辑 —— `out=$(cli ... put hello world)`，`set -euo pipefail` 下
#           客户端返回 NOT_LEADER（非 0）会直接中止，与仓库脚本修前完全一致。
#   retry ：M6.10.5(8) 补丁后的逻辑 —— 只在明确的领导权/路由错误（NOT_LEADER）上做
#           **有界重试**；其它错误立即上抛。判据没有放宽：最终仍必须是 OK。
#
# 输出：每轮一行 `PROBE mode=.. round=.. rc=.. out=[..] attempts=..`，最后一行计数。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/bin"
ROUNDS=200
MODE=retry
LOG_ENGINE="${RAFTKV_LOG_ENGINE:-lsm}"
STATE_ENGINE="${RAFTKV_STATE_ENGINE:-lsm}"

# 与 scripts/raft_e2e.sh 一致的有界重试常量（改这里也要同步改那里）。
PUT_RETRY_MAX=8
PUT_RETRY_SLEEP=0.25

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2;;
    --mode) MODE="$2"; shift 2;;
    --bin) BIN="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[[ "$MODE" == "raw" || "$MODE" == "retry" ]] || { echo "bad --mode $MODE" >&2; exit 2; }
[[ -x "$BIN/raftkv_raft_node" ]] || { echo "missing node binary in $BIN" >&2; exit 1; }

echo "# first-put probe bin=$BIN rounds=$ROUNDS mode=$MODE log=$LOG_ENGINE state=$STATE_ENGINE"
echo "# $(date -Is) uname=$(uname -sr) nproc=$(nproc) load=$(cut -d' ' -f1 /proc/loadavg)"

pass=0
fail=0
for round in $(seq 1 "$ROUNDS"); do
  WORK="$(mktemp -d)"
  BASE=$((30000 + RANDOM % 2000))
  P1=$BASE; P2=$((BASE + 1)); P3=$((BASE + 2))
  PEERS="1=127.0.0.1:$P1,2=127.0.0.1:$P2,3=127.0.0.1:$P3"
  declare -a PIDS=()
  for id in 1 2 3; do
    port=$((BASE + id - 1))
    "$BIN/raftkv_raft_node" --id "$id" --port "$port" --peers "$PEERS" \
      --data-dir "$WORK/node$id" --log-engine "$LOG_ENGINE" --state-engine "$STATE_ENGINE" \
      >"$WORK/node$id.log" 2>&1 &
    PIDS[$id]=$!
  done

  # wait_leader（与仓库脚本同形：200 × 0.1s 轮询 find_leader）
  leader_ok=0
  for _ in $(seq 1 200); do
    for id in 1 2 3; do
      if "$BIN/raftkv_raft_cli" --peers "$PEERS" --host 127.0.0.1 --port "$((BASE + id - 1))" status 2>/dev/null | grep -q 'role=leader'; then
        leader_ok=1; break
      fi
    done
    (( leader_ok == 1 )) && break
    sleep 0.1
  done

  out=""
  rc=0
  attempts=0
  if (( leader_ok == 0 )); then
    rc=99
    out="NO_LEADER_WITHIN_20S"
  elif [[ "$MODE" == "raw" ]]; then
    attempts=1
    # 原样：这一行在 set -e 下失败会直接中止脚本（探针里用 if 包住以便计数）
    if out="$("$BIN/raftkv_raft_cli" --peers "$PEERS" --host 127.0.0.1 --port "$P1" put hello world 2>&1)"; then rc=0; else rc=$?; fi
  else
    for ((i = 1; i <= PUT_RETRY_MAX; ++i)); do
      attempts=$i
      if out="$("$BIN/raftkv_raft_cli" --peers "$PEERS" --host 127.0.0.1 --port "$P1" put hello world 2>&1)"; then
        rc=0; break
      fi
      rc=$?
      case "$out" in
        *NOT_LEADER*) sleep "$PUT_RETRY_SLEEP";;
        *) break;;
      esac
    done
  fi

  if (( rc == 0 )) && [[ "$out" == "OK" ]]; then
    pass=$((pass + 1)); st=PASS
  else
    fail=$((fail + 1)); st=FAIL
  fi
  printf 'PROBE mode=%s round=%s rc=%s out=[%s] attempts=%s %s\n' "$MODE" "$round" "$rc" "$out" "$attempts" "$st"

  for id in 1 2 3; do kill -9 "${PIDS[$id]}" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$WORK"
done

echo "PROBE_SUMMARY mode=$MODE rounds=$ROUNDS pass=$pass fail=$fail"
if (( fail == 0 )); then echo "[FIRST_PUT_PROBE_OK] mode=$MODE"; else echo "[FIRST_PUT_PROBE_FAIL] mode=$MODE"; fi
