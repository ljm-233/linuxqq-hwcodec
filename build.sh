#!/bin/bash
# 编译探针。依赖只有 gcc。
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)

gcc -O2 -shared -fPIC -Wall -Wextra -o "$here/libhwprobe.so" \
	"$here/src/hwprobe.c" "$here/src/vthook.c" -ldl

echo "编好了: $here/libhwprobe.so"
echo "--- 动态符号：包装 dl* 入口（这四个都必须导出，缺一个就是整条调用路径漏掉）"
wrappers=$(nm -D --defined-only "$here/libhwprobe.so" | awk '$2 == "T" { print $3 }' || true)
missing=""
for w in dlopen dlmopen dlsym dlvsym; do
	if printf '%s\n' "$wrappers" | grep -qx "$w"; then
		echo "  ✓ $w"
	else
		echo "  ✗ $w 没有导出"
		missing="$missing $w"
	fi
done
if [ -n "$missing" ]; then
	echo "    ↑ 缺少包装符号：$missing（QQ 的调用方走 dlvsym 取版本化符号，缺它整条路看不到）"
	exit 1
fi
echo "--- 动态符号：被介入的目标（必须是导出的同名符号；否则直接链接的调用方绕过我们）"
targets=$(nm -D --defined-only "$here/libhwprobe.so" | awk '$2 == "T" { print $3 }' |
	grep -E '^(DllGetClassObject|NvEncodeAPICreateInstance|NvEncodeAPIGetMaxSupportedVersion)$' || true)
if [ -n "$targets" ]; then
	printf '%s\n' "$targets" | sed 's/^/  ✓ /'
else
	echo "    ✗ 三个目标符号没有导出 —— LD_PRELOAD 介入不会生效（检查 static / visibility）"
	exit 1
fi
echo "--- 下一步：$here/linuxqq-hwcodec-probe"
