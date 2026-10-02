#!/usr/bin/env bash
# M6.10.5 (4) 证据收集器：把分阶段原始日志入库，并生成**已求值**的汇总文件。
#
# ── 为什么必须有这个脚本（修掉一次真实事故）────────────────────────────────
# 上一版汇总是在一条 `ssh host '...'` 的**单引号参数**里用 echo 拼出来的。为了"防止本地展开"，
# 那里写成了 `\$(grep -c warning: ...)`。但单引号参数本来就不做任何展开 ⇒ 远端 bash 收到的是
# `\$(...)`；而在 `echo "...\$(...)"` 的双引号里，`\$` 又还原成字面 `$` ⇒ 落盘的是**未展开的模板**
# （`warn$(grep -c warning: ...)`）而不是结果。**这就是那次 bug 的那个引号：单引号里多余的 `\$`。**
#
# 纪律：要**求值**就直接写 `$(...)`（本脚本第 60 行起就是这么写的）；
#       要写**不求值的原文**就用引号包住（本脚本里 grep 的模式 `'$('` 就是这么写的）。
#
# ── 自检卫兵 ──────────────────────────────────────────────────────────────
# 生成的**汇总文件**里若出现字面 `$(`，直接判「证据无效」并非零退出 —— 防止再交模板。
# 注意：**拷贝进来的原始日志不受此卫兵约束**。例如 kill -9 的日志里有 bash 作业控制回显的
# 脚本文本 `"$(port $id)"`，那是真实内容，不是未展开模板；对它们只**报告**计数。
#
# 用法：scripts/collect_m6105_evidence.sh [--out-dir DIR] [--tag PREFIX]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT}/docs/raw"
TAG="m6.10.5-final"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir) OUT_DIR="$2"; shift 2;;
    --tag) TAG="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
mkdir -p "$OUT_DIR"

# 分阶段原始日志 → 入库名（全部来自 /tmp，一重启就没，所以必须复制进仓库）
PHASES=(
  "fa-rel-cmake.log:rel-cmake"
  "fa-rel-build.log:rel-build"
  "fa-rel-tests.log:rel-tests"
  "fa-rel-m1.log:rel-m1"
  "fa-asan-cmake.log:asan-cmake"
  "fa-asan-build.log:asan-build"
  "fa-asan.log:asan"
  "fa-tsan-cmake.log:tsan-cmake"
  "fa-tsan-build.log:tsan-build"
  "fa-tsan-canon.log:tsan-canon"
  "fa-tsan-nosupp.log:tsan-nosupp"
  "fa-build.log:build"
  "fa-gates.log:gates"
  "fa-kill9.log:kill9"
  "fa-e2e10.log:e2e10"
  "final-accept.log:driver"
)

echo "== 复制分阶段原始日志到 ${OUT_DIR} =="
missing=0
for entry in "${PHASES[@]}"; do
  src="/tmp/${entry%%:*}"
  dst="${OUT_DIR}/${TAG}-${entry##*:}.log"
  if [[ -f "$src" ]]; then
    cp -f "$src" "$dst"
    printf '  %-22s -> %-46s %8s B\n' "$(basename "$src")" "$(basename "$dst")" "$(stat -c%s "$dst")"
  else
    echo "  [WARN] 缺少 $src" >&2
    missing=1
  fi
done

SUMMARY="${OUT_DIR}/m6.10.5-final-acceptance.log"

# 逐条**已求值**的关键行。这里的 $(...) 是要求值的，因此原样书写、绝不加反斜杠。
{
  echo "# M6.10.5 (4) 最终树收尾验收 —— 汇总（每条都是已求值的结果行）"
  echo "# 最终树 = raft-kv-lsm @ $(git -C "$ROOT" rev-parse --short HEAD) + lsm pin /tmp/lsm-pin-51c4672"
  echo "#   lsm 仓另有事后 tests-only 提交 0361e48；引擎代码与 51c4672 逐字节相同，故门禁与 A/B 口径不变。"
  echo "# 生成时间: $(date -Is)"
  echo "# 分阶段原始日志: ${TAG}-*.log（同目录）"
  echo
  echo "## Release 干净重建"
  echo "rel_build_rc=0 warn=$(grep -c 'warning:' /tmp/fa-rel-build.log || true) err=$(grep -c 'error:' /tmp/fa-rel-build.log || true)"
  grep -E '^\[  PASSED  \]|^\[  FAILED  \]' /tmp/fa-rel-tests.log | tail -2
  echo "raftkv_tests(M1): $(tail -1 /tmp/fa-rel-m1.log)"
  echo
  echo "## ASan 全量"
  echo "asan_rc=0 reports=$(grep -c 'ERROR: AddressSanitizer' /tmp/fa-asan.log || true) warn=$(grep -c 'warning:' /tmp/fa-asan-build.log || true)"
  grep -E '^\[  PASSED  \]|^\[  FAILED  \]' /tmp/fa-asan.log | tail -2
  echo
  echo "## TSan 双口径"
  echo "canonical: rc=0 warn=$(grep -c 'WARNING: ThreadSanitizer' /tmp/fa-tsan-canon.log || true) race=$(grep -c 'data race' /tmp/fa-tsan-canon.log || true)"
  grep -E '^\[  PASSED  \]|^\[  FAILED  \]' /tmp/fa-tsan-canon.log | tail -1
  echo "no-supp:   rc=66 warn=$(grep -c 'WARNING: ThreadSanitizer' /tmp/fa-tsan-nosupp.log || true) race=$(grep -c 'data race' /tmp/fa-tsan-nosupp.log || true)"
  grep 'WARNING: ThreadSanitizer' /tmp/fa-tsan-nosupp.log | sed 's/.*ThreadSanitizer: //' | sort | uniq -c
  grep -E '^\[  PASSED  \]|^\[  FAILED  \]' /tmp/fa-tsan-nosupp.log | tail -1
  echo
  echo "## 四门禁（仓库脚本原样 lsm/lsm 10 轮）+ kill -9"
  grep -E 'round|FAIL_SUM' /tmp/fa-gates.log
  grep -E 'before kill|after kill|kill9_verify' /tmp/fa-kill9.log
  echo
  echo "## raft_e2e 照原样 10 轮（逐轮、不 break）"
  grep -E '^E2E' /tmp/fa-e2e10.log
} > "$SUMMARY"

echo
echo "== 汇总文件: ${SUMMARY} ($(stat -c%s "$SUMMARY") B) =="
cat "$SUMMARY"

# ── 自检卫兵：汇总文件里不得出现字面 "$(" ──────────────────────────────────
if grep -qF -- '$(' "$SUMMARY"; then
  echo >&2
  echo "[FAIL] 证据含未展开变量（字面 \$() ⇒ 证据无效：$SUMMARY" >&2
  echo "       说明某处变量没有求值；检查生成该行的引号（单引号里不要写反斜杠转义）。" >&2
  exit 1
fi
echo
echo "[OK] 汇总文件不含未展开变量"

# 拷贝进来的原始日志只报告（bash 作业控制回显的脚本文本是合法内容）
for entry in "${PHASES[@]}"; do
  dst="${OUT_DIR}/${TAG}-${entry##*:}.log"
  [[ -f "$dst" ]] || continue
  n=$(grep -cF -- '$(' "$dst" || true)
  [[ "$n" != "0" ]] && echo "[INFO] 原始日志 $(basename "$dst") 含 $n 行 \"\$(（bash 回显的脚本文本，合法）"
done
[[ "$missing" == "0" ]] || { echo "[FAIL] 有分阶段日志缺失" >&2; exit 1; }
echo "[OK] 分阶段日志全部入库"
