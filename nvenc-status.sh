#!/bin/bash
# 一眼看清：QQ 的 NVENC 到底有没有在跑（只读，不碰任何进程）
# 用法：./nvenc-status.sh
set -uo pipefail
LOG="${QQ_WAYLAND_FIX_LOG:-/run/user/1000/linuxqq-wayland-fix.log}"
ok()   { printf '  \033[32m%s\033[0m  %s\n' "正常" "$1"; }
warn() { printf '  \033[33m%s\033[0m  %s\n' "注意" "$1"; }
bad()  { printf '  \033[31m%s\033[0m  %s\n' "故障" "$1"; }
info() { printf '  %s  %s\n' "提示" "$1"; }

ppapi=""
for p in $(pgrep -x qq 2>/dev/null); do
	c=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null) || continue
	case "$c" in *--type=ppapi*) ppapi="$p" ;; esac
done

printf '=== NVENC 状态 ===\n'
if [ -z "$ppapi" ]; then
	info "没找到收帧进程 ppapi（QQ 没运行，或还没开过共享）"
	info "收帧进程是共享开始后才 fork 出来的，先开一次共享再跑本脚本"
	exit 0
fi
printf '  收帧进程 ppapi = %s\n' "$ppapi"

# 1) 环境变量（只在进程启动时读一次，所以改了必须重启 QQ）
master=$(tr '\0' '\n' < "/proc/$ppapi/environ" 2>/dev/null | sed -n 's/^QQ_NVENC=//p' | head -1)
active=$(tr '\0' '\n' < "/proc/$ppapi/environ" 2>/dev/null | sed -n 's/^QQ_NVENC_ACTIVE=//p' | head -1)
printf '  QQ_NVENC=%s   QQ_NVENC_ACTIVE=%s\n' "${master:-（未设）}" "${active:-（未设）}"

# 2) 库映射
n_lib=$(grep -c 'libqq-nvenc' "/proc/$ppapi/maps" 2>/dev/null)
n_enc=$(grep -c 'libnvidia-encode' "/proc/$ppapi/maps" 2>/dev/null)
n_cuda=$(grep -c 'libcuda' "/proc/$ppapi/maps" 2>/dev/null)
printf '  映射: libqq-nvenc=%s 段  libnvidia-encode=%s 段  libcuda=%s 段\n' "$n_lib" "$n_enc" "$n_cuda"

# 3) 日志
printf '  日志(%s)里的 NVENC 行:\n' "$LOG"
if [ -r "$LOG" ]; then
	grep -aE 'qq-nvenc|NVENC' "$LOG" 2>/dev/null | tail -5 | sed 's/^/      /' || true
else
	info "  读不到日志文件"
fi

# 4) 结论
printf -- '--- 结论\n'
if [ "$n_lib" = "0" ]; then
	bad "libqq-nvenc 没被加载 —— LD_PRELOAD 没生效（QQ 是不是没用带 LD_PRELOAD 的命令启动？）"
elif [ -z "$master" ]; then
	bad "设了 ACTIVE 但没设总开关 QQ_NVENC=1 —— 库已被加载但构造函数直接 return，不挂钩、不打日志、不加载 NVENC"
	printf '      正确启动：LD_PRELOAD=…/libqq-nvenc.so QQ_NVENC=1 QQ_NVENC_ACTIVE=1 linuxqq-wayland-fix\n'
elif [ "$n_enc" = "0" ]; then
	warn "总开关已设，但还没加载 libnvidia-encode —— 多半是此刻没有在共享（编码器是共享时才创建的）"
	info "开一次共享、动着屏幕，再跑本脚本"
else
	ok "NVENC 在跑：libnvidia-encode + libcuda 已加载（$n_enc / $n_cuda 段）"
	info "NVENC 会一直开着；看内存增速用 wayland-cast-doctor 的【五】"
fi
