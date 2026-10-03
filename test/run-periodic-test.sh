#!/bin/bash
# 回归测试：周期汇总不得杀死宿主。
#
# 判据：
#   1. 宿主退出码 0 —— 旧实现（alarm+SIGALRM，每个被注入进程各设一个）下这里会是
#      142（= 128 + SIGALRM），也就是"启动器被闹钟打死、QQ 起不来"那个事故。
#   2. 宿主与 fork 出来的子进程都跑到自然结束
#   3. 期间确实产出了周期摘要（≥2 行）—— 说明是"惰性打印"在干活，而不是"什么都没做"
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

fail=0
chk() { if [ "$1" = 0 ]; then echo "  ✓ $2"; else echo "  ✗ $2"; fail=1; fi; }

echo "=== 编译"
gcc -O2 -o periodichost periodichost.c -ldl || exit 1
[ -f ../libhwprobe.so ] || (cd .. && ./build.sh) || exit 1

LOG=$(mktemp /tmp/hwprobe-periodic-XXXXXX.log)
# 说明：探针只对 cmdline 含 qq/ppapi 的进程做周期汇总（其它进程静默，只记一行），
# 测试宿主不是 QQ，所以用 HWPROBE_ALL=1 走全量路径 —— 这与真实场景等价：
# QQ 的 ppapi 进程本来就属于"相关进程"，不需要这个变量。
echo "=== 跑 9 秒（HWPROBE_PERIOD=2，宿主每秒 dlopen 一次，另有一个 fork 出来的子进程）"
OUT=$(HWPROBE_LOG="$LOG" HWPROBE_PERIOD=2 HWPROBE_ALL=1 LD_PRELOAD=../libhwprobe.so ./periodichost 2>&1)
rc=$?

[ "$rc" -eq 0 ]; chk $? "宿主退出码 0（旧实现下会是 142/SIGALRM；实际 $rc）"
echo "$OUT" | grep -q "宿主完成";   chk $? "宿主跑到自然结束"
echo "$OUT" | grep -q "子进程完成"; chk $? "fork 出来的子进程也活着"
n=$(grep -c "周期汇总" "$LOG" 2>/dev/null); n=${n:-0}
[ "${n:-0}" -ge 2 ]; chk $? "周期摘要 ≥2 行（实际 ${n:-0}）—— 惰性打印确实在工作"
grep -q "SIGALRM\|被信号" "$LOG" 2>/dev/null; [ $? -ne 0 ]; chk $? "日志里没有信号相关异常"

echo
echo "--- 日志里的周期摘要（前 3 行）"
grep "周期汇总" "$LOG" 2>/dev/null | head -3
echo
if [ $fail -eq 0 ]; then echo "全部通过"; else echo "有失败项"; fi
rm -f "$LOG"
exit $fail
