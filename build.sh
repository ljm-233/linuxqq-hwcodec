#!/bin/bash
# 编译探针。依赖只有 gcc。
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)

gcc -O2 -shared -fPIC -Wall -Wextra -o "$here/libhwprobe.so" \
	"$here/src/hwprobe.c" "$here/src/vthook.c" -ldl

echo "编好了: $here/libhwprobe.so"
echo "--- 动态符号：dlopen/dlmopen/dlsym（dlsym 那条路要用）"
nm -D --defined-only "$here/libhwprobe.so" | awk '$2 == "T" { print $3 }' |
	grep -xE 'dlopen|dlmopen|dlsym' | sed 's/^/    /' || true
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
