#!/bin/bash
# try-gpu-config.sh —— 逐个尝试「让 QQ 跑在 N 卡（NVIDIA）上」，每一步都能自证。
#
# 为什么要做这件事：QQ 如果跑在核显（i915）上，它持有的缓冲记账在**系统内存**
# （/proc/meminfo 的 Shmem），共享时一路涨到把机器冻死；跑在独显上同样的堆积会落在
# 显存（8 GB），机器不会冻。
#
# 用法：
#   ./try-gpu-config.sh list              # 看配置矩阵
#   ./try-gpu-config.sh verdict           # 只读：当前 QQ 到底在哪个 GPU 上（随时可跑）
#   ./try-gpu-config.sh dry-run <配置名>  # 只打印将要执行的环境与命令，不启动
#   ./try-gpu-config.sh <配置名>          # 用该配置启动 QQ（前台；要求 QQ 已完全退出）
#
# 本脚本**不会**杀任何进程：如果 QQ 还在跑，它会拒绝启动并让你自己先退出
# （QQ 是单实例，不退出新实例不会接管）。

set -uo pipefail

ICD_NVIDIA=/usr/share/vulkan/icd.d/nvidia_icd.json
LAUNCHER=linuxqq-wayland-fix

die() { printf '%s\n' "$*" >&2; exit 1; }

qq_pids() { pgrep -x qq 2>/dev/null; }

# 收帧/编码的那个进程（内存堆积就记在它的 GPU 上）；找不到就输出空
ppapi_pid() {
	local p c
	for p in $(qq_pids); do
		c=$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null) || continue
		case "$c" in *--type=ppapi*) printf '%s' "$p"; return ;; esac
	done
}

# 单个进程打开的 DRM 设备（去重）
drm_devs() {
	local pid="$1" t
	for f in /proc/$pid/fd/*; do
		t=$(readlink "$f" 2>/dev/null) || continue
		case "$t" in /dev/dri/*) printf '%s\n' "$t" ;; esac
	done | sort -u
}

# 该进程通过 fdinfo 报出来的 drm 驱动/PCI 设备（去重）
drm_info() {
	local pid="$1"
	for f in /proc/$pid/fdinfo/*; do
		grep -hE '^drm-(driver|pdev)' "$f" 2>/dev/null || true
	done | sort -u | paste -sd' ' -
}

# 该进程里有多少个线程在跑 llvmpipe（软件渲染）；0 = 没在用软件渲染
llvmpipe_threads() {
	local pid="$1" n=0 c
	for t in /proc/$pid/task/*/comm; do
		c=$(cat "$t" 2>/dev/null) || continue
		case "$c" in llvmpipe*) n=$((n + 1)) ;; esac
	done
	printf '%d' "$n"
}

# 只读体检：把"在哪个 GPU 上"的证据全打出来，并给一句结论
verdict() {
	local pids pid n_nv=0 n_i915=0 n_other=0
	pids=$(qq_pids)

	echo "=== QQ 在哪个 GPU 上 ==="
	if [ -z "$pids" ]; then
		echo "QQ 没在运行（跑一次配置或用「QQ（独显版）」启动后再看）"
	else
		echo "QQ 进程数: $(printf '%s\n' "$pids" | wc -l)"
		for pid in $pids; do
			local devs info lp
			devs=$(drm_devs "$pid" | paste -sd' ' -)
			[ -n "$devs" ] || continue
			info=$(drm_info "$pid")
			lp=$(llvmpipe_threads "$pid")
			printf '  pid=%-8s 设备: %-28s %s  llvmpipe线程=%s\n' "$pid" "$devs" "$info" "$lp"
			case "$devs" in *renderD129*) n_nv=$((n_nv + 1)) ;; esac
			case "$devs" in *renderD128*) n_i915=$((n_i915 + 1)) ;; esac
			case "$devs" in *renderD129*) ;; *) case "$devs" in *renderD128*) ;; *) n_other=$((n_other + 1)) ;; esac ;; esac
		done
		printf 'libnvidia-* 映射: %s\n' "$(for pid in $pids; do grep -ohE 'libnvidia-[a-z]+' /proc/$pid/maps 2>/dev/null; done | sort -u | paste -sd' ' - || true)"
	fi

	echo "--- 独显侧"
	if command -v nvidia-smi >/dev/null; then
		nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader 2>/dev/null | sed 's/^/  VRAM: /'
		local in_smi
		in_smi=$(nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader 2>/dev/null | grep -c qq || true)
		printf '  nvidia-smi 进程列表里的 qq: %s 个\n' "${in_smi:-0}"
	else
		echo "  没装 nvidia-smi"
	fi

	local pp devs
	pp=$(ppapi_pid)
	if [ -n "$pp" ]; then
		devs=$(drm_devs "$pp" | paste -sd' ' -)
		printf '收帧/编码进程 ppapi: pid=%s 设备=%s %s\n' "$pp" "${devs:-（无）}" "$(drm_info "$pp")"
	fi

	echo "--- 结论"
	if [ -z "$pids" ]; then
		echo "  无法判断（QQ 没在运行）"
	elif [ -z "$pp" ]; then
		echo "  ⚠️ 没找到收帧进程（还没开过共享？）—— 开一次共享后再跑本命令"
	elif [ "${devs:-}" = "" ]; then
		echo "  ⚠️ 收帧进程没有打开任何 DRM 设备（还没开共享？）"
	elif printf '%s' "$devs" | grep -q renderD129; then
		echo "  ✅ 收帧进程在独显（NVIDIA）上 —— 堆积会落在显存（8 GB），机器不容易冻"
	else
		echo "  ❌ 收帧进程在核显（i915）上 —— 共享占用记进系统内存（Shmem），这就是会冻机的那条路"
		if [ "${n_nv:-0}" -gt 0 ]; then
			echo "     （其它 qq 子进程碰过 renderD129，但堆积不记在它们身上，不算数）"
		fi
	fi
}

# ---- 配置矩阵 -------------------------------------------------------------
# 每个配置：环境变量 + 传给启动器的额外参数
config_env() {
	case "$1" in
	baseline)
		# 用户当前在用的组合：核显 + Mesa llvmpipe + 关掉 ANGLE/Vulkan（画面正常）
		cat <<-'EOF'
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			QQ_WAYLAND_FIX_ANGLE=off
		EOF
		;;
	vulkan-nvidia)
		# 让 ANGLE 走 Vulkan，并把 Vulkan ICD 锁到 NVIDIA —— 本机最可能成的一条
		cat <<-EOF
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			VK_ICD_FILENAMES=$ICD_NVIDIA
			VK_DRIVER_FILES=$ICD_NVIDIA
			QQ_WAYLAND_FIX_ANGLE=vulkan
		EOF
		;;
	angle-gl-nvidia)
		# 关掉启动器自带的 --use-angle=vulkan，改用 ANGLE 的桌面 GL 后端（经 GLX/NVIDIA）
		cat <<-'EOF'
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			QQ_WAYLAND_FIX_ANGLE=off
		EOF
		;;
	prime-1)
		# Mesa 的设备选择变量（先前只试过 DRI_PRIME=2/0，1 没试过）
		cat <<-'EOF'
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			DRI_PRIME=1
			QQ_WAYLAND_FIX_ANGLE=off
		EOF
		;;
	*) return 1 ;;
	esac
}

config_args() {
	case "$1" in
	angle-gl-nvidia) printf '%s\n' '--use-gl=angle' '--use-angle=gl' ;;
	*) : ;;
	esac
}

config_note() {
	case "$1" in
	baseline) echo "你当前在用的组合（核显 + llvmpipe + ANGLE off）。作为对照，画面正常。" ;;
	vulkan-nvidia) echo "锁 NVIDIA Vulkan ICD + ANGLE vulkan。最可能上独显的一条；风险：--use-angle=vulkan 正是「视频画面变小区」的元凶，画面可能变小。" ;;
	angle-gl-nvidia) echo "ANGLE 桌面 GL 后端（经 GLX/NVIDIA）。绕开 EGL 那个失败点；风险：GLX 在 Wayland 下不一定可用。" ;;
	prime-1) echo "Mesa DRI_PRIME=1。改动最小；先前只试过 2/0，1 未试。" ;;
	esac
}

list_configs() {
	echo "配置矩阵（先 ./try-gpu-config.sh dry-run <名字> 看将要执行什么）："
	local c
	for c in baseline vulkan-nvidia angle-gl-nvidia prime-1; do
		printf '\n  %-18s %s\n' "$c" "$(config_note "$c")"
	done
	echo
	echo "判据：verdict 里出现 renderD129（且没有 renderD128）才算真上独显。"
}

show_plan() {
	local name="$1" envs args
	envs=$(config_env "$name") || die "不认识的配置：$name（用 list 看全部）"
	args=$(config_args "$name")
	printf '=== 配置 %s\n' "$name"
	printf '%s\n' "$(config_note "$name")"
	echo "--- 将设置的环境变量"
	printf '%s\n' "$envs" | sed 's/^/  /'
	if [ -n "$args" ]; then
		echo "--- 将追加给启动器的参数"
		printf '%s\n' "$args" | sed 's/^/  /'
	else
		echo "--- 不追加参数"
	fi
	echo "--- 实际命令行"
	if [ -n "$args" ]; then
		printf '  env %s %s %s\n' "$(printf '%s ' $envs)" "$LAUNCHER" "$(printf '%s ' $args)"
	else
		printf '  env %s %s\n' "$(printf '%s ' $envs)" "$LAUNCHER"
	fi
	echo "--- 启动后怎么验"
	echo "  开一次共享，然后（新终端）跑： ./try-gpu-config.sh verdict"
	echo "  回到现在这个正常状态： ./try-gpu-config.sh baseline"
}

launch() {
	local name="$1" envs args
	envs=$(config_env "$name") || die "不认识的配置：$name（用 list 看全部）"
	args=$(config_args "$name")

	if [ -n "$(qq_pids)" ]; then
		die "QQ 还在运行（$(qq_pids | wc -l) 个进程）—— 请先从托盘完全退出 QQ，再跑本命令。
本脚本不会替你杀进程：QQ 是单实例，不退出新实例不会接管。"
	fi
	command -v "$LAUNCHER" >/dev/null || die "找不到 $LAUNCHER"

	echo "=== 用配置 $name 启动 QQ"
	printf '%s\n' "$(config_note "$name")" | sed 's/^/    /'
	echo "    启动后开一次共享，再跑： ./try-gpu-config.sh verdict"

	# 环境变量逐条 export；参数追加在启动器后面（启动器会 exec "$QQ" … "$@"）
	while IFS= read -r kv; do
		[ -n "$kv" ] || continue
		export "${kv?}"
	done <<< "$envs"

	if [ -n "$args" ]; then
		# shellcheck disable=SC2086
		exec "$LAUNCHER" $args
	else
		exec "$LAUNCHER"
	fi
}

# ---- 入口 -----------------------------------------------------------------
case "${1:-verdict}" in
	list) list_configs ;;
	verdict | status) verdict ;;
	dry-run) [ -n "${2:-}" ] || die "用法：$0 dry-run <配置名>"; show_plan "$2" ;;
	baseline | vulkan-nvidia | angle-gl-nvidia | prime-1) launch "$1" ;;
	-h | --help)
		sed -n '2,20p' "$0"
		;;
	*) die "不认识的参数：$1（用 list / verdict / dry-run <配置名> / <配置名>）" ;;
esac
