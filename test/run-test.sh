#!/bin/bash
# 冒烟测试：用假 COM 模块验证转发桩「不挪栈、不改返回值、不崩」
#
# 判据：
#   A. 开启模式（HWPROBE_VTABLE=1）
#      A1 driver 的 6 个期望值全对 —— 尤其 m8（7 个整数参数，第 7 个走栈）
#      A2 日志里有"已代理 IClassFactory"与"vtable 已挂钩"
#      A3 日志里能按槽位记录调用与返回值，且 vtable[3] 返回 0x0
#      A4 槽位统计里能看到热点
#   B. 默认模式（不设 HWPROBE_VTABLE）：照样跑通，且不代理工厂（默认不动宿主）
#   C. 日志上限（HWPROBE_VTABLE_MAXLOG=3）生效
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

fail=0
chk() { if [ "$1" = 0 ]; then echo "  ✓ $2"; else echo "  ✗ $2"; fail=1; fi; }

echo "=== 编译"
gcc -O2 -shared -fPIC -o libfakecom.so fakecom.c || exit 1
gcc -O2 -o driver driver.c -ldl || exit 1
[ -f ../libhwprobe.so ] || (cd .. && ./build.sh) || exit 1

expect_output() {
    local out="$1" rc="$2"
    [ "$rc" -eq 0 ]; chk $? "退出码 0（没崩）"
    echo "$out" | grep -q "^m0(5)   = 6 "      ; chk $? "m0(5) = 6"
    echo "$out" | grep -q "^m1(5)   = 10 "     ; chk $? "m1(5) = 10"
    echo "$out" | grep -q "^m2()    = 42 "     ; chk $? "m2() = 42"
    echo "$out" | grep -q "^m3()    = 0 "      ; chk $? "m3() = 0"
    echo "$out" | grep -q "^m5(9,4) = 5 "      ; chk $? "m5(9,4) = 5"
    echo "$out" | grep -q "^m8(1..7)= 1234567 "; chk $? "m8(1..7) = 1234567（栈参数没错位）"
}

echo
echo "=== A. 开启模式（HWPROBE_VTABLE=1）"
logA=$(mktemp /tmp/hwprobe-A-XXXX.log)
outA=$(LD_PRELOAD=../libhwprobe.so HWPROBE_ALL=1 HWPROBE_VTABLE=1 HWPROBE_LOG="$logA" ./driver 2>&1)
rcA=$?
expect_output "$outA" "$rcA"
grep -q "已代理 IClassFactory" "$logA"                  ; chk $? "代理了 IClassFactory"
grep -q "vtable 已挂钩" "$logA"                         ; chk $? "挂钩了接口虚表"
grep -qE "vtable\[3\] ret .*0x0000000000000000" "$logA"  ; chk $? "vtable[3] 返回值记为 0x0"
grep -qE "vtable\[[0-9]+\]  调用 [0-9]+ 次" "$logA"      ; chk $? "汇总里有槽位热点统计"

echo
echo "=== B. 默认模式（不设 HWPROBE_VTABLE，应当完全不动宿主）"
logB=$(mktemp /tmp/hwprobe-B-XXXX.log)
outB=$(LD_PRELOAD=../libhwprobe.so HWPROBE_ALL=1 HWPROBE_LOG="$logB" ./driver 2>&1)
rcB=$?
expect_output "$outB" "$rcB"
if grep -q "已代理 IClassFactory" "$logB"; then echo "  ✗ 默认模式不该代理工厂"; fail=1; else echo "  ✓ 默认模式不代理工厂"; fi
grep -q "COM 虚表观测：未启用" "$logB"                   ; chk $? "日志里说明未启用"

echo
echo "=== C. 日志上限（HWPROBE_VTABLE_MAXLOG=3）"
logC=$(mktemp /tmp/hwprobe-C-XXXX.log)
outC=$(LD_PRELOAD=../libhwprobe.so HWPROBE_ALL=1 HWPROBE_VTABLE=1 HWPROBE_VTABLE_MAXLOG=3 HWPROBE_LOG="$logC" ./driver 2>&1)
rcC=$?
expect_output "$outC" "$rcC"
n=$(grep -cE "vtable\[[0-9]+\] (call|ret )" "$logC")
if [ "$n" -le 6 ]; then echo "  ✓ 日志被限流（vtable 行 $n ≤ 6）"; else echo "  ✗ 限流没生效（$n 行）"; fail=1; fi

echo
echo "=== D. 直接链接路径（导出符号介入 —— 真实调用方走的就是这条）"
gcc -O2 -o direct direct.c -L. -lfakecom -Wl,-rpath,'$ORIGIN' || exit 1
logD=$(mktemp /tmp/hwprobe-D-XXXX.log)
outD=$(LD_PRELOAD=../libhwprobe.so HWPROBE_ALL=1 HWPROBE_VTABLE=1 HWPROBE_LOG="$logD" ./direct 2>&1)
rcD=$?
expect_output "$outD" "$rcD"
grep -q "导出符号介入生效" "$logD"          ; chk $? "日志确认走的是导出符号路径（全程没有 dlsym）"
grep -q "DllGetClassObject(clsid=" "$logD"  ; chk $? "我们的 DllGetClassObject 确实被介入了"
grep -q "已代理 IClassFactory" "$logD"      ; chk $? "这条路上工厂照样被代理"
if grep -q "用 dlsym 取走的" "$logD"; then
    echo "  ✗ 直接链接却走了 dlsym 路径（说明导出没生效）"; fail=1
else
    echo "  ✓ 调用方没有用 dlsym 取符号"
fi

echo
echo "=== 日志样本（A 模式）"
grep -E "vtable\[[0-9]+\]" "$logA" | head -6 | sed 's/^/  /'
echo
[ "$fail" -eq 0 ] && echo "结论：全部通过 ✓" || echo "结论：有失败项 ✗"
exit "$fail"
