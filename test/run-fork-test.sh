#!/bin/bash
# 验证：fork 出来的子进程即使被关掉所有继承的 fd，探针仍能把日志写回 HWPROBE_LOG。
# 背景见 forktest.c 顶部注释（2026-10-03 实测：ppapi 进程里有探针却一行没写）。
set -uo pipefail
cd "$(dirname "$0")"

[ -f ../libhwprobe.so ] || { echo "先跑 ../build.sh" >&2; exit 1; }
[ -x ./forktest ] || gcc -O2 -o forktest forktest.c || exit 1

log=$(mktemp /tmp/hwprobe-fork-XXXX.log)
chk() { if [ "$1" -eq 0 ]; then echo "  ✓ $2"; else echo "  ✗ $2"; fail=1; fi; }
fail=0

# HWPROBE_ALL=1：这个测试程序的 cmdline 里没有 qq/ppapi，不设的话探针按设计保持安静
out=$(LD_PRELOAD=../libhwprobe.so HWPROBE_ALL=1 HWPROBE_LOG="$log" ./forktest 2>&1)
child=$(printf '%s\n' "$out" | sed -n 's/^子进程 pid=\([0-9]*\) 已退出$/\1/p')

echo "=== 子进程输出"
printf '%s\n' "$out" | sed 's/^/  /'
echo "=== 日志（$(wc -l < "$log") 行）"
grep -nE '已注入|被继承后关闭|libopenh264|汇总|命令行' "$log" | cut -c1-150 | sed 's/^/  /'

echo "=== 断言"
grep -q '日志 fd 被继承后关闭，已重新打开' "$log"; chk $? "子进程发现 fd 没了并重新打开日志"
grep -q 'libopenh264' "$log";                      chk $? "子进程的 dlopen 被记录下来了"
[ -n "$child" ] && grep -q "汇总" "$log";          chk $? "子进程退出时（继承来的）汇总也写进去了"

rm -f "$log" ./forktest
if [ "$fail" -eq 0 ]; then echo "结论：通过 ✓"; else echo "结论：失败 ✗"; exit 1; fi
