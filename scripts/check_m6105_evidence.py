#!/usr/bin/env python3
# M6.10.5 证据体检器：两关合一的只读检查，防止再交"未求值的模板"当证据。
#
# 第 1 关（未展开变量）：证据文件里若出现字面 `$(`，说明某处变量没有求值。
#   白名单（**合法**，不算失败）：
#     * `*.sh` —— 那是**脚本文本本身**（e.g. m6.10.5-e2e-before.sh 是修前的仓库脚本快照），
#       `$(...)` 是它的源码，不是输出；
#     * 显式声明的**原始日志白名单** —— bash 作业控制会把脚本源码行回显进日志
#       （e.g. kill -9 日志里的 `"$(port $id)"`），那是真实内容。
# 第 2 关（已求值）：每条"结论数字"必须能在对应文件里找到**已求值**的行；
#   计数类检查支持限定小节（见 CHECKS 的 scope），避免同一行被数两遍。
#
# ⚠ 为什么用 python 而不是 grep：本轮已经踩过两次同类坑 —— 在 `ssh host '...'` 的**单引号**参数里
#   写 `grep -cF "$\("`，远端 bash 会把它当成固定串 `$\(`（双引号内 `(` 前的反斜杠被保留），
#   于是"含未展开变量"被误报成 0。本脚本的模式全在 python 字符串里构造，不经任何 shell 引号。
#
# 用法：scripts/check_m6105_evidence.py [--raw-dir DIR]
import argparse
import glob
import os
import sys

NEEDLE = "$("

# 原始日志白名单：这些文件的 `$(` 是 bash 回显的脚本文本，属合法内容。
RAW_LOG_ALLOWLIST = {
    "m6.10.5-final-kill9.log": "bash 作业控制回显的脚本文本（\"$(port $id)\" 等）",
}

# 证据文件 -> (必须出现的已求值子串, [(前缀, 期望条数, 限定小节 or None)])
CHECKS = {
    "m6.10.5-two-compactions.log": (
        ["cross_engine_mismatches=0", "reopen_dump_equal=1",
         "lsm_compaction_rounds=140", "snapshot_index=119834",
         "TWO_COMPACTIONS_OK", "TCOMP round=6 verify=[missing 0]"],
        [("TCOMP round=", 6, None), ("TCOMP post_restart verify=[missing 0]", 1, None)],
    ),
    "m6.10.5-e2e-probe.log": (
        ["PROBE_SUMMARY mode=raw rounds=80 pass=75 fail=5",
         "PROBE_SUMMARY mode=retry rounds=200 pass=200 fail=0",
         "attempts=2", "out=[NOT_LEADER]",
         "SANITY_SUMMARY rounds=12 pass=12 fail=0"],
        [],
    ),
    "m6.10.5-bench-ab-p99max.log": (
        ["lat_max_us=82950", "lat_max_us=NA", "lat_p99_us="],
        [],
    ),
    "m6.10.5-lsm-memenv-powerloss.log": (
        ["[  PASSED  ] 210 tests.", "gate_rc=0",
         "CrashSim.UnsyncedWalTailIsActuallyDiscarded",
         "CrashSim.CrashInjectionEffectiveAcrossSeeds"],
        [("PASS  ", 24, "## C) lsm_gate 逐腿"),
         ("FAIL  ", 0, "## C) lsm_gate 逐腿"),
         ("SKIP  ", 0, "## C) lsm_gate 逐腿")],
    ),
    "m6.10.5-final-acceptance.log": (
        ["warn=0 err=0", "reports=0 warn=0",
         "canonical: rc=0 warn=0 race=0", "no-supp:   rc=66 warn=16 race=0",
         "FAIL_SUM=0", "kill9_verify: PASS",
         "E2E_SUMMARY total=10 pass=10 fail=0",
         "raftkv_tests(M1): all tests passed (13)"],
        [("e2e round ", 10, None)],
    ),
}


def scan_unexpanded(raw_dir):
    files = [p for p in sorted(glob.glob(os.path.join(raw_dir, "m6.10.5-*"))) if not p.endswith(".sh")]
    bad = 0
    print("== 第 1 关：未展开变量 ==")
    for path in files:
        hits = [i for i, l in enumerate(open(path, encoding="utf-8", errors="replace").read().splitlines(), 1)
                if NEEDLE in l]
        if not hits:
            continue
        name = os.path.basename(path)
        if name in RAW_LOG_ALLOWLIST:
            print("[OK-WHITELIST] %-44s %d 行（%s）" % (name, len(hits), RAW_LOG_ALLOWLIST[name]))
            continue
        bad += 1
        print("[BAD] %-44s %d 行含字面 %r" % (name, len(hits), NEEDLE))
        for i in hits[:4]:
            print("        L%d" % i)
    print("检查了 %d 个文件（.sh 脚本文本已排除，原始日志白名单 %d 个）" % (len(files), len(RAW_LOG_ALLOWLIST)))
    return bad


def scan_evaluated(raw_dir):
    bad = 0
    print()
    print("== 第 2 关：结论数字必须是已求值的行 ==")
    for name, (tokens, counts) in CHECKS.items():
        path = os.path.join(raw_dir, name)
        if not os.path.exists(path):
            print("[MISSING] %s" % name)
            bad += 1
            continue
        all_lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        text = "\n".join(all_lines)
        problems = [("missing token %r" % t) for t in tokens if t not in text]
        for prefix, want, scope in counts:
            lines = all_lines
            if scope is not None:
                idx = next((i for i, l in enumerate(all_lines) if l.startswith(scope)), None)
                if idx is None:
                    problems.append("scope marker %r not found" % scope)
                    continue
                lines = all_lines[idx:]
            got = sum(1 for l in lines if l.startswith(prefix))
            if got != want:
                problems.append("count(%r)=%d want %d" % (prefix, got, want))
        print("[%s] %-46s tokens=%d counts=%d problems=%d" %
              ("OK " if not problems else "BAD", name, len(tokens), len(counts), len(problems)))
        for p in problems:
            print("        %s" % p)
        if problems:
            bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw-dir", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "raw"))
    args = ap.parse_args()
    bad = scan_unexpanded(args.raw_dir) + scan_evaluated(args.raw_dir)
    print()
    print("RESULT: %s" % ("OK（未展开变量 0 / 结论数字全部已求值）" if bad == 0 else "FAIL count=%d" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
