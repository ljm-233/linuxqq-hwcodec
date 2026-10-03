# PR #28 试跑：屏幕共享改用 NVENC 编 H.264

上游实验 PR：**`SHORiN-KiWATA/linuxqq-wayland-fix` #28**，作者 **FJKiXfaR**（就是提内存泄漏 issue #19 的那位）。
分支 `pr28`，提交 `b0657a8`，**未合并、0 评审** —— 纯实验，按下面的顺序试。

## 为什么值得试（和我们之前的结论不冲突）

我们此前测到：客户端**每秒只编得动约 6 帧**（2560×1600），剩下的帧留在 i915 GEM / 系统内存里 → **398 MB/s** 堆积。
如果这个 PR 真把编码接到 NVENC，**消费速度上去，堆积可能直接消失** —— 也就是最初想要的"用 N 卡搞硬编"。

**它绕开了我们今天撞的那堵墙**：NVENC/CUDA 只需要 `libcuda.so` + `libnvidia-encode.so`（运行时 `dlopen`），
输入是 CPU 侧 IYUV 缓冲 —— **不需要 EGL/GLX**，所以"客户端 GL 拿不到 N 卡"这个死结与它无关 ✓

## 它做了什么

inline hook `libAVSDKPlugin.so` 的 `CreateH264Encoder`（导出符号，偏移 `0xdfce60`，入口 14 字节正好是 5 条完整指令），
把返回对象的 vtable 指向自己的实现（按 C++ ABI 从 vptr 数：[2]Init [3]UnInit [4]GetErrorCode [5]SetCodecCallback
[6]SetParam [7]GetParam [8]DoEncode），用 NVENC 出流后按原契约填 `VideoPacket`
（`+0x08` 帧号 / `+0x10` 平均 QP / `+0x14` 长度 / `+0x18` 码流指针）并回调原实现的 `fEncoderDoneCallback`。

## 构建（本机已验证成功）

只需构建期头文件 `ffnvcodec/nvEncodeAPI.h`（**不需要 CUDA 头**，CUDA 是运行时 dlopen）。

正式装包：Arch = `nv-codec-headers`（AUR）；Debian/Ubuntu = `libffmpeg-nvenc-dev`；然后 `make` 即可。
不想装包时，用 `NVENC_INC` 指一个含 `ffnvcodec/` 的目录（本机就是这么验的）：

```bash
cd ~/coding/linuxqq-wayland-screenshare-fix
git worktree add -f /tmp/pr28-build pr28            # 不打扰当前工作区（那边有未提交改动）
git clone --depth 1 https://github.com/FFmpeg/nv-codec-headers /tmp/nv-codec-headers
cd /tmp/pr28-build && make NVENC_INC=/tmp/nv-codec-headers/include libqq-nvenc.so
```

本机结果：`libqq-nvenc.so` 113,976 字节，`NEEDED` 只有 `libc.so.6`，77 条 `NVENC` 日志串 ✓

> 仓库启动器带 `--doctor` 的挂钩字节自检，但那是 **pr28 版的启动器**（未安装）。用下面的 `LD_PRELOAD` 叠加方式即可，
> 不依赖它。

## 怎么挂上去

**已安装的启动器会前置它自己的四个库、但保留已有的 `LD_PRELOAD`**
（`export LD_PRELOAD="$preload${LD_PRELOAD:+:$LD_PRELOAD}"`），所以直接叠加：

```bash
LD_PRELOAD=$HOME/coding/linuxqq-hwcodec/libqq-nvenc.so QQ_NVENC=1 linuxqq-wayland-fix
```

三个开关（库内读取）：

| 变量 | 作用 |
|---|---|
| `QQ_NVENC=1` | **挂钩并旁观**：只挂钩、透传，行为与不注入一致（默认完全不做事） |
| `QQ_NVENC_ACTIVE=1` | 配合上面：**真正用 NVENC 编码** |
| `QQ_NVENC_PROBE_DUMP=0` | 只挂钩不打字节（排查用） |

## 测试三步（**必须按顺序**）

### 第 1 步：旁观模式（只验证挂钩，不改行为）

```bash
# 完全退出 QQ（托盘），然后：
LD_PRELOAD=$HOME/coding/linuxqq-hwcodec/libqq-nvenc.so QQ_NVENC=1 linuxqq-wayland-fix
# 开一次共享 30 秒
grep -aE 'ppapi 进程，等待 AVSDK 加载|NVENC' /run/user/1000/linuxqq-wayland-fix.log | tail -20
```

应看到 `ppapi 进程，等待 AVSDK 加载（QQ_NVENC=1, ACTIVE=0）` 以及挂钩成功的信息。
**这一步画面必须完全正常**（旁观模式不该改变任何行为）—— 不正常就停，回退。

### 第 2 步：启用 NVENC（同时验证"真的在用"）

```bash
LD_PRELOAD=$HOME/coding/linuxqq-hwcodec/libqq-nvenc.so QQ_NVENC=1 QQ_NVENC_ACTIVE=1 linuxqq-wayland-fix
# 开共享 30 秒，然后：
grep -aE 'NVENC: (会话就绪|出流|出帧|回调)' /run/user/1000/linuxqq-wayland-fix.log | tail -10
P=$(pgrep -f 'type=ppapi' | head -1)
grep -c 'libnvidia-encode' /proc/$P/maps ; grep -c 'libcuda' /proc/$P/maps
nvidia-smi | sed -n '/Processes/,$p' | grep -i qq        # ★ 必须用默认输出的 Processes 表
cd ~/coding/linuxqq-hwcodec && ./try-gpu-config.sh mark   # 记基线（含"归因到 qq"的显存）
# 共享中再跑：
./try-gpu-config.sh verdict
```

**判读**（用的都是修正后的有效指标）：

| 指标 | 期望 |
|---|---|
| 日志 | `NVENC: 会话就绪 <WxH> @<fps>, <kbps> kbps CBR`、`NVENC: 出流 … -> 出帧 … -> 回调已返回` |
| ppapi 映射 | `libnvidia-encode` 与 `libcuda` 都有映射（>0 段） |
| `nvidia-smi` | **默认输出的 Processes 表**里出现 qq 的行 ← ❌ **不要**用 `--query-compute-apps`（只列计算进程，QQ 永不出现，是无效指标） |
| 显存 | `verdict` 里**归因到 qq** 的显存增长（不是全卡未归因的那个数） |

### 第 3 步：量内存堆积（**这才是重点**）

同档位、同内容，**开/不开 `QQ_NVENC_ACTIVE=1` 各测一次**：

```bash
wayland-cast-doctor          # 看【五】的 Shmem 增速（共享中跑）
# 需要更硬的 30 秒采样（带刹车，防冻机）：
/usr/bin/niri-shm-attrib 0.5 30 --guard 4 --dump /tmp/nvenc-test-dump.txt > /tmp/nvenc-test.csv
```

- 现在档位是 `saver`（15fps + 960×600）；**建议再测一次 `1080p` 或全分辨率** —— 那里差异最大（基线 398 MB/s）
- **成功判据**：同一档位下 Shmem 增速**显著下降**（理想接近 0），且对端画面正常
- 共享时**要动着窗口/滚页面**，否则静态画面本来就不产生新帧，测不出真实速率

## 回退

```bash
# 去掉 QQ_NVENC_ACTIVE=1 即可恢复（想彻底摘掉就整条 LD_PRELOAD 都不要）：
QQ_WAYLAND_FIX_ANGLE=off linuxqq-wayland-fix      # = 你现在能用的现状（画面正常 + saver 档 + 刹车）
```

库自身也有安全网：编码过程中任一步失败 → 该对象标记 `nv_dead`，**回落原实现、不丢帧**；
QQ 更新导致入口字节变化 → **拒绝挂钩**并写日志（走原来的软件编码）。

## 风险（如实列）

- **未合并、0 评审的实验补丁**，作者自己也在 iteration
- **对 QQ 版本敏感**：挂钩点靠入口字节匹配，QQ 一更新就可能拒绝（拒绝是安全的，不会崩）
- 作者修过一个真实崩溃：IYUV 输入里 U/V 是**半行距**，按整行距推进会越界写（1920×1088 时写出 4.46 MB 而缓冲只有 3.34 MB）→ ppapi 段错误。**所以务必先旁观、再启用**
- 它只改**编码**：渲染/上独显那条线（今天测的七个方案）不受影响，结论也不因此改变
- 码率/分辨率由 PR 内部策略决定（作者实测 1920×1088@23fps 2700 kbps、2560×1440@23fps 3200 kbps CBR）

## 与我们既有结论的关系

| 此前的说法 | 现在 |
|---|---|
| 「用户侧没有腾讯给的开关」 | ✅ 仍然成立（配置/环境变量/COM 入口都没有） |
| 「**用户侧做不到**硬件编码」 | ❌ **被本 PR 证伪** —— 社区用 inline hook 直接换掉了编码器 |
| 「硬编要等腾讯」 | ⚠️ 改为：**腾讯不给也能做**，代价是对 QQ 版本敏感的实验性 hook |
| `uiUseHw` / `broadcast-core` 的 COM 路径 | ❌ **已被本 PR 证伪**：`libAVSDKPlugin.so` 里没有任何 NVENC/QSV/AMF 引用（静态 OpenH264），`AVSDK_SetHWAbility` 打开也没用（没有硬件实现可指） |
