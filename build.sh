#!/bin/bash
# 编译探针。依赖只有 gcc。
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)

gcc -O2 -shared -fPIC -Wall -Wextra -o "$here/libhwprobe.so" \
	"$here/src/hwprobe.c" "$here/src/vthook.c" -ldl

echo "编好了: $here/libhwprobe.so"
echo "--- 包装的符号（应能看到 dlopen/dlsym/dlmopen 以及 NVENC 入口）"
nm -D --defined-only "$here/libhwprobe.so" | grep -oE '(dlopen|dlmopen|dlsym)$' | sed 's/^/    /' || true
echo "    （DllGetClassObject / NvEncodeAPI* 是 static 包装，经 dlsym 转发，不出现在动态符号表里 —— 正常）"
echo "--- 下一步：$here/linuxqq-hwcodec-probe"
