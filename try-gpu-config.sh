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
#   ./try-gpu-config.sh mark              # 记下当前显存占用，作为「共享前后涨了多少」的基线
#   ./try-gpu-config.sh dry-run <配置名>  # 只打印将要执行的环境与命令，不启动
#   ./try-gpu-config.sh <配置名>          # 用该配置启动 QQ（前台；要求 QQ 已完全退出）
#
# 本脚本**不会**杀任何进程：如果 QQ 还在跑，它会拒绝启动并让你自己先退出
# （QQ 是单实例，不退出新实例不会接管）。
#
# 判据（三条硬标准，缺一不可）：
#   1. 收帧进程 ppapi 打开了 renderD129
#   2. 该进程里 llvmpipe 线程数 = 0（只要还有 llvmpipe，就是在软件渲染，不是真上独显）
#   3. 共享进行中显存明显上涨（先 mark，再开共享，再看 verdict 的差值）
# 「只打开了设备节点」不算数 —— 实测 prime-1 就是这样：节点开了、VRAM 还是 32 MiB。

set -uo pipefail

ICD_NVIDIA=/usr/share/vulkan/icd.d/nvidia_icd.json
LAUNCHER=linuxqq-wayland-fix
STATE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/linuxqq-hwcodec"
VRAM_BASE_FILE="$STATE_DIR/vram-baseline"

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

vram_used_mib() {
	command -v nvidia-smi >/dev/null || { printf ''; return; }
	nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' '
}

# 当前有没有活动的采集流（有才谈得上"显存上涨"）
video_streams() {
	command -v pw-dump >/dev/null || { printf '0'; return; }
	local n
	n=$(pw-dump 2>/dev/null | grep -c 'Stream/Output/Video' || true)
	printf '%s' "${n:-0}"
}

mark() {
	mkdir -p "$STATE_DIR" || die "建不了 $STATE_DIR"
	local v
	v=$(vram_used_mib)
	[ -n "$v" ] || die "读不到显存占用（没装 nvidia-smi？）"
	printf '%s\n' "$v" >"$VRAM_BASE_FILE"
	printf '已记下基线：显存 %s MiB → %s\n' "$v" "$VRAM_BASE_FILE"
	printf '现在去开一次共享，跑 20-30 秒，然后在**共享进行中**跑 ./try-gpu-config.sh verdict\n'
}

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
	local vram_now="" base="" delta=""
	if command -v nvidia-smi >/dev/null; then
		vram_now=$(vram_used_mib)
		nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader 2>/dev/null | sed 's/^/  VRAM: /'
		local in_smi
		in_smi=$(nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader 2>/dev/null | grep -c qq || true)
		printf '  nvidia-smi 进程列表里的 qq: %s 个\n' "${in_smi:-0}"
		if [ -s "$VRAM_BASE_FILE" ]; then
			base=$(cat "$VRAM_BASE_FILE")
			if [ -n "$vram_now" ] && [ -n "$base" ]; then
				delta=$((vram_now - base))
				printf '  显存相对基线（%s MiB）：%+d MiB\n' "$base" "$delta"
			fi
		else
			echo "  （还没记基线：共享前先跑 ./try-gpu-config.sh mark，才能看出涨了多少）"
		fi
	else
		echo "  没装 nvidia-smi"
	fi

	local pp="" devs="" lp=""
	pp=$(ppapi_pid)
	if [ -n "$pp" ]; then
		devs=$(drm_devs "$pp" | paste -sd' ' -)
		lp=$(llvmpipe_threads "$pp")
		printf '收帧/编码进程 ppapi: pid=%s 设备=%s %s llvmpipe线程=%s\n' "$pp" "${devs:-（无）}" "$(drm_info "$pp")" "$lp"
	fi

	local streams
	streams=$(video_streams)
	printf '活动采集流: %s 个\n' "${streams:-0}"

	echo "--- 结论（三条硬标准：renderD129 + llvmpipe=0 + 共享时显存上涨）"
	if [ -z "$pids" ]; then
		echo "  无法判断（QQ 没在运行）"
		return
	elif [ -z "$pp" ]; then
		echo "  ⚠️ 没找到收帧进程（还没开过共享？）—— 开一次共享后再跑本命令"
		return
	elif [ -z "$devs" ]; then
		echo "  ⚠️ 收帧进程没有打开任何 DRM 设备（还没开共享？）"
		return
	fi

	if ! printf '%s' "$devs" | grep -q renderD129; then
		echo "  ❌ 收帧进程在核显（i915）上 —— 共享占用记进系统内存（Shmem），这就是会冻机的那条路"
		[ "${n_nv:-0}" -gt 0 ] && echo "     （其它 qq 子进程碰过 renderD129，但堆积不记在它们身上，不算数）"
		return
	fi

	if [ "${lp:-0}" -gt 0 ]; then
		echo "  ⚠️ 假阳性：收帧进程确实打开了 renderD129，但里面还有 ${lp} 个 llvmpipe 线程"
		echo "     说明实际渲染仍走软件渲染，不是真上独显（prime-1 实测就是这样）"
		return
	fi

	echo "  ✓ 硬标准 1、2 通过：收帧进程打开了 renderD129，且没有 llvmpipe 线程"
	if [ "${streams:-0}" -gt 0 ] && [ -n "$delta" ]; then
		if [ "$delta" -ge 100 ]; then
			echo "  ✅ 硬标准 3 也通过：共享中显存涨了 ${delta} MiB —— 真的在独显上干活了"
			echo "     （记得确认对端能看到画面、且画面正常；顺便看 wayland-cast-doctor 的 Shmem 增速）"
		elif [ "$delta" -ge 32 ]; then
			echo "  🟡 显存涨了 ${delta} MiB（轻微）—— 有动静但不大，建议多跑一会儿再看"
		else
			echo "  ⚠️ 共享中显存几乎没涨（${delta} MiB）—— 可能只是挂了设备节点，实际没在独显上干活"
		fi
	else
		echo "  🟡 还差硬标准 3：先 ./try-gpu-config.sh mark，再开共享，然后共享进行中再跑本命令"
	fi
}

# ---- 配置矩阵 -------------------------------------------------------------
# 每个配置：环境变量 + 传给启动器的额外参数 +（可选）bwrap 前缀
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
		# 让 ANGLE 走 Vulkan，并把 Vulkan ICD 锁到 NVIDIA —— 实测失败：ANGLE 仍挑 Intel
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
		# ANGLE 桌面 GL 后端 —— 实测失败：GLX 需要 X11，Wayland 下回退到 Mesa
		cat <<-'EOF'
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			QQ_WAYLAND_FIX_ANGLE=off
		EOF
		;;
	prime-1)
		# Mesa 的 DRI_PRIME=1 —— 实测失败：节点开了，但实际仍走 llvmpipe
		cat <<-'EOF'
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			DRI_PRIME=1
			QQ_WAYLAND_FIX_ANGLE=off
		EOF
		;;
	xwayland-nvidia)
		# 让 QQ 跑在 XWayland 下：NVIDIA 的 GLX 在 X11 下才可用（Wayland 下 eglInitialize 失败）
		cat <<-'EOF'
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			QQ_WAYLAND_FIX_ANGLE=off
		EOF
		;;
	bwrap-hide-igpu | xwayland-bwrap)
		# 强制手段：给 QQ 一个只看得到 renderD129 的 /dev/dri
		cat <<-'EOF'
			__NV_PRIME_RENDER_OFFLOAD=1
			__GLX_VENDOR_LIBRARY_NAME=nvidia
			LIBVA_DRIVER_NAME=nvidia
			LIBVA_DRIVER_DEVPATH=/dev/dri/renderD129
			QQ_WAYLAND_FIX_ANGLE=off
		EOF
		;;
	*) return 1 ;;
	esac
}

config_args() {
	case "$1" in
	angle-gl-nvidia) printf '%s\n' '--use-gl=angle' '--use-angle=gl' ;;
	xwayland-nvidia | xwayland-bwrap) printf '%s\n' '--ozone-platform=x11' ;;
	*) : ;;
	esac
}

# bwrap 前缀：把 /dev/dri 换成只含 renderD129 的 tmpfs（核显节点对 QQ 不可见）
config_bwrap() {
	case "$1" in
	bwrap-hide-igpu | xwayland-bwrap)
		command -v bwrap >/dev/null || die "没装 bwrap。等价写法（需要 root 或 userns）：
  unshare -rm sh -c 'mount -t tmpfs tmpfs /dev/dri && mount --bind /dev/dri/renderD129 /dev/dri/renderD129 && exec linuxqq-wayland-fix'"
		printf '%s\n' bwrap --dev-bind / / --tmpfs /dev/dri \
			--dev-bind /dev/dri/renderD129 /dev/dri/renderD129 --die-with-parent
		;;
	*) : ;;
	esac
}

config_note() {
	case "$1" in
	baseline) echo "你当前在用的组合（核显 + llvmpipe + ANGLE off）。作为对照，画面正常。" ;;
	vulkan-nvidia) echo "锁 NVIDIA Vulkan ICD + ANGLE vulkan。实测失败（ANGLE 仍挑 Intel），且画面会变小。" ;;
	angle-gl-nvidia) echo "ANGLE 桌面 GL（经 GLX）。实测失败：GLX 需要 X11，Wayland 下回退到 Mesa。" ;;
	prime-1) echo "Mesa DRI_PRIME=1。实测失败：节点开了，实际仍走 llvmpipe。" ;;
	xwayland-nvidia) echo "【新】让 QQ 跑在 XWayland 下（--ozone-platform=x11）—— NVIDIA 的 GLX 只在 X11 下可用。风险：共享功能在 XWayland 下行为未知，画面可能异常。" ;;
	bwrap-hide-igpu) echo "【新】用 bwrap 给 QQ 一个只看得到 renderD129 的 /dev/dri（强制手段，不管 ANGLE 怎么挑都只有 N 卡）。风险：QQ 可能因找不到预期设备而完全不渲染。" ;;
	xwayland-bwrap) echo "【新】两者叠加：XWayland + 只暴露 N 卡。前两个都单独失败时的最后一击。" ;;
	esac
}

list_configs() {
	echo "配置矩阵（先 ./try-gpu-config.sh dry-run <名字> 看将要执行什么）："
	local c
	for c in baseline vulkan-nvidia angle-gl-nvidia prime-1 xwayland-nvidia bwrap-hide-igpu xwayland-bwrap; do
		printf '\n  %-18s %s\n' "$c" "$(config_note "$c")"
	done
	echo
	echo "判据（三条硬标准，缺一不可）："
	echo "  1) 收帧进程 ppapi 打开 renderD129"
	echo "  2) 该进程 llvmpipe 线程数 = 0"
	echo "  3) 共享中显存明显上涨（先 mark → 开共享 → verdict 看差值）"
	echo "并且：对端必须能看到画面、画面必须正常。只打开节点不算（prime-1 就是这么假阳性的）。"
}

show_plan() {
	local name="$1" envs args bwrap_prefix
	envs=$(config_env "$name") || die "不认识的配置：$name（用 list 看全部）"
	args=$(config_args "$name")
	bwrap_prefix=$(config_bwrap "$name")

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
	printf '  env %s %s%s %s\n' "$(printf '%s ' $envs)" \
		"${bwrap_prefix:+$(printf '%s ' $bwrap_prefix)}" "$LAUNCHER" "$(printf '%s ' $args)"
	if [ -n "$bwrap_prefix" ]; then
		echo "  （bwrap 把 /dev/dri 换成只含 renderD129 的 tmpfs；/dev/nvidia* 仍然可见）"
	fi
	echo "--- 启动后怎么验"
	echo "  ./try-gpu-config.sh mark        # 共享前先记显存基线"
	echo "  开一次共享，共享进行中跑： ./try-gpu-config.sh verdict"
	echo "  回到现在这个正常状态： ./try-gpu-config.sh baseline"
}

launch() {
	local name="$1" envs args bwrap_prefix
	envs=$(config_env "$name") || die "不认识的配置：$name（用 list 看全部）"
	args=$(config_args "$name")
	bwrap_prefix=$(config_bwrap "$name")

	if [ -n "$(qq_pids)" ]; then
		die "QQ 还在运行（$(qq_pids | wc -l) 个进程）—— 请先从托盘完全退出 QQ，再跑本命令。
本脚本不会替你杀进程：QQ 是单实例，不退出新实例不会接管。"
	fi
	command -v "$LAUNCHER" >/dev/null || die "找不到 $LAUNCHER"

	echo "=== 用配置 $name 启动 QQ"
	printf '%s\n' "$(config_note "$name")" | sed 's/^/    /'
	echo "    共享前先跑： ./try-gpu-config.sh mark"
	echo "    开共享后跑： ./try-gpu-config.sh verdict"

	# 环境变量逐条 export；bwrap 会继承；参数追加在启动器后面（启动器 exec "$QQ" … "$@"）
	while IFS= read -r kv; do
		[ -n "$kv" ] || continue
		export "${kv?}"
	done <<< "$envs"

	if [ -n "$args" ]; then
		# shellcheck disable=SC2086
		set -- $args
	else
		set --
	fi

	if [ -n "$bwrap_prefix" ]; then
		# shellcheck disable=SC2086
		exec $bwrap_prefix "$LAUNCHER" "$@"
	else
		exec "$LAUNCHER" "$@"
	fi
}

# ---- 入口 -----------------------------------------------------------------
case "${1:-verdict}" in
	list) list_configs ;;
	verdict | status) verdict ;;
	mark) mark ;;
	dry-run) [ -n "${2:-}" ] || die "用法：$0 dry-run <配置名>"; show_plan "$2" ;;
	baseline | vulkan-nvidia | angle-gl-nvidia | prime-1 | xwayland-nvidia | bwrap-hide-igpu | xwayland-bwrap) launch "$1" ;;
	-h | --help)
		sed -n '2,25p' "$0"
		;;
	*) die "不认识的参数：$1（用 list / verdict / mark / dry-run <配置名> / <配置名>）" ;;
esac
