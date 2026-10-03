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

## 看不到日志 / 看起来没生效：先跑这个

**实测踩到的第一个坑（2026-10-03）：只设 `QQ_NVENC_ACTIVE=1` 是完全静默的。**

源码里的总开关是 `QQ_NVENC`（`src/qq-nvenc.c` 的构造函数）：

```c
if (!flag_on("QQ_NVENC", 0))
    return;                       /* 没设总开关 → 直接返回：不挂钩、不打日志、不加载 NVENC */
nv_active = flag_on("QQ_NVENC_ACTIVE", 0);   /* ACTIVE 只决定"要不要真接管" */
```

所以 `LD_PRELOAD=… QQ_NVENC_ACTIVE=1 …` 的现象是：**库确实被加载了（`/proc/PID/maps` 里有 `libqq-nvenc`），但一条日志都没有、`libnvidia-encode` 也没加载** —— 看起来像"没生效"，其实是从没开始 ✗

一条命令看结论：

```bash
./nvenc-status.sh          # 只读；会报总开关、库映射、日志行，并给出结论
```

它按这个顺序判断：

| 现象 | 含义 |
|---|---|
| `libqq-nvenc=0 段` | LD_PRELOAD 没生效（QQ 不是用带 LD_PRELOAD 的命令启动的） |
| 库在、**`QQ_NVENC` 未设** | **总开关没开，构造函数直接 return** —— 补上 `QQ_NVENC=1` 并重启 QQ |
| 总开关在、`libnvidia-encode=0 段` | 多半是**此刻没有在共享**（编码器对象是共享开始才创建） |
| `libnvidia-encode` 已加载 | NVENC 真在跑 ✓ |

另外两条容易误判的：

- **环境变量只在进程启动时读一次** → 改完必须**完全退出 QQ** 再启动（QQ 是单实例，不退出新实例不接管）
- **启动器每次启动都会截断日志**（`> $LOG`）→ 日志里只有**当前这次会话**的内容，看不到上一次的行是正常的


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

## 接管成功但对端转圈/黑屏：已查明的三点

**现象**（实测）：NVENC 真接管了 —— `libnvidia-encode` 5 段 / `libcuda` 6 段映射，日志有
`NVENC: 出流 28259 字节 qp=45 type=0`、`NVENC 出帧 idx=2/3 … → 回调已返回`；本机 Shmem 稳定 0 MB/s。
**但对端收不到可解画面（转圈）。**

### 已排除：会话几何与源帧不一致

PR 的会话几何**取自送进来的帧**，不是取自 Init 参数（`src/qq-nvenc.c`）：

```c
wpx = get32((char *)f + VF_OFF_W);   /* VideoFrame: +0x00 宽 / +0x04 高 */
hpx = get32((char *)f + VF_OFF_H);
if (!w->nv_on || wpx != w->w || hpx != w->h)        /* 尺寸一变就重开会话 */
    nv_open(w, wpx, hpx, w->fps ? w->fps : 30, w->kbps ? w->kbps : 2000);
```

本次共享协商到 **2560×1600 @60**（niri journal：`size: spa_rectangle { width: 2560, height: 1600 }`、
`framerate: spa_fraction { num: 60 }`），会话即按 2560×1600 开 —— **与源帧一致 ✓**
所以"会话几何 ≠ 源帧几何"**不是**病因。作者自测的 `1920×1088` 是他**源帧本身**的尺寸，不是对齐产物。

### 头号嫌疑：VideoPacket 字段偏移可能不匹配你这份 QQ

PR 只填四个字段：

```
PKT_OFF_IDX  0x08  帧号      PKT_OFF_QP   0x10  qp
PKT_OFF_LEN  0x14  长度      PKT_OFF_DATA 0x18  码流指针（64 位）
```

而**旁观模式**打印的"原实现 DoEncode 之后 packet 前 0x60"里（你自己的那次日志）：

```
+0x08: 25 00 00 00                = 37           ← 与 PR 的 IDX 对得上 ✓
+0x10: 25 00 00 00                = 37           ← QP？
+0x14: 3d b1 01 00                = 110909       ← 长度 ✓
+0x18: 40 40 c0 03                = 0x03c04040   ← ✗ 这不像指针
+0x28: d8 19 de 5c 29 7f 00 00    = 0x7f295cde19d8  ← ✓ 这才像堆指针
```

**若下游读的是 +0x28、而 PR 把指针写在 +0x18**，下游拿到的是垃圾/空数据 → 对端解不出画面 ✓ 与现象吻合。

**一次运行就能判死，不用改代码**：跑**旁观模式**（`QQ_NVENC=1`），日志里 PR 会转印原实现回调拿到的
packet 并打印它按 +0x18 解析的结果：

```
[qq-nvenc pid=…] 原实现回调第 N 次: ctx=… *pp_packet=…
[qq-nvenc pid=…]   cb packet 前 0x40: …
[qq-nvenc pid=…]   -> data=0x… len=… idx=…
```

看 `-> data=` 是不是一个**有效指针**（`0x7f…`）、`len` 是否与实际码流长度相符：

- **data 有效、len 合理** → +0x18 是对的，这条排除，转下面两条
- **data 为 0 或 `0x3c04040` 这类** → **偏移不匹配实锤** → 在 worktree 里把 `PKT_OFF_DATA` 改成 `0x28`，重建再试

### 第二嫌疑：同步回调

原实现是异步（DoEncode 返回后由编码线程回调），PR 是**在 DoEncode 里同步回调后立即 return 0**：

```c
((pkt_cb_fn)w->cb0)(w->cb_ctx, &pk, w->cb1);
return 0;
```

若下游要求"回调发生在 DoEncode 返回之后"（或对 packet 生命周期另有假设），帧可能被丢弃。
**代价小**：在 worktree 里把回调挪到线程（或返回后再触发）重建再试。

### 第三嫌疑：码流参数

PR 用 NVENC 默认（High profile）+ 每帧带 SPS/PPS + Annex-B；原实现是 OpenH264。
对端通常能接 Annex-B，但 **profile/level 或 SPS/PPS 频率**也可能被拒。
低成本验证：在 `nv_open()` 里给 `encodeCodecConfig.h264Config` 强制 **Baseline / Constrained Baseline**，重建再试。

### 下一步（按代价从低到高）

1. **旁观模式跑一次，抓 `-> data=… len=… idx=…` 那几行** —— 一条命令判定头号嫌疑
2. **换个共享几何再试**（`niri-portal-cast-tune 1080p`，或**共享单个窗口**让源帧不是 2560×1600）：对端能看到就说明与尺寸相关 ✓
3. worktree 里试小改：`PKT_OFF_DATA` 改 0x28 / 强制 Baseline / 回调异步化
4. 都不行 → 这是 PR 的实验性缺陷；把上面三条证据（含 `-> data=` 行）留给作者（**是否联系由用户决定**）

> 本节全部基于**只读**源码分析 + 已有日志；未改上游代码、未杀进程、未向上游发任何评论。

### 对头号嫌疑的补充判断（避免带偏）

有一点必须先说清：**`+0x18` 那个奇怪的值，很可能只是"当时还没填"** —— 原实现是**异步**的，
`DoEncode` 返回时 packet 还没构造完，真正填好在它自己的**回调**里。所以"原实现 DoEncode 之后的
+0x18 不像指针"**不能**直接证明偏移错 ✗。

因此更准确的判据是**看原实现回调里的那个 packet**（PR 已经会打印）：

```
[qq-nvenc] 原实现回调第 N 次: ctx=… *pp_packet=…
[qq-nvenc]   cb packet 前 0x40: …
[qq-nvenc]   -> data=0x… len=… idx=…
```

`-> data=` 有效 + `len` 合理 → **+0x18 是对的，偏移这条排除**；此时**嫌疑转到**：

**回调契约**（当前最可疑）—— 原实现回调给下游的是**它自己的 packet 对象**（`cb_tramp` 里拿到的是
`*pp_packet`），而 NVENC 路径是把**调用方给的 packet** 填好直接递下去：

```c
void *pk = packet;                       /* 调用方的 packet */
put32/put64(…);                          /* 只填 4 个字段 */
((pkt_cb_fn)w->cb0)(w->cb_ctx, &pk, w->cb1);   /* 同步回调 */
return 0;
```

下游若依赖"packet 是编码器内部对象"（对象池/释放回调/生命周期），或依赖**回调发生在 DoEncode
返回之后**（原实现就是如此），这两个差异都足以让它悄悄丢帧 → 对端转圈 ✓

**验证代价都很低**（都在 worktree 里改，别动主 clone）：
① 把回调挪到 DoEncode 返回之后触发（线程或延后）；② 强制 Baseline profile；
③ 若 ① 无效再动偏移。

---

## 变体试验：异步回调（`QQ_NVENC_ASYNCCB=1`）

**已构建好，一个环境变量切换，不用换 .so。** 这是上面「第二嫌疑：同步回调」的最小验证。

### 它改了什么

| | 原 PR（默认，`ASYNCCB` 未设） | 本变体（`QQ_NVENC_ASYNCCB=1`） |
|---|---|---|
| 回调时机 | **在 `DoEncode` 里同步回调**，随后立即 `return 0` | **推迟到下一次 `DoEncode` 进入时补发** —— 也就是"上一次 DoEncode 已经返回之后" |
| 递下去的 packet | **调用方给的那个 packet** (`void *pk = packet;`) | **我们自己的影子 packet**（先照抄调用方那份，再写 idx/qp/len/data 四个字段） |
| 递下去的码流 | 直接指 NVENC 的输出缓冲 | **复制一份**再递下去 |

**为什么必须复制码流**：`nv_encode()` 在返回前就调了 `nvEncUnlockBitstream(w->session, w->outbuf)`
—— 那块缓冲在解锁后**不保证还有效**。同步回调时"刚解锁、还没被复用"通常没事，但**延后一帧再递下去，
那块缓冲很可能已经被下一帧覆盖** ✗，所以变体里先 `memcpy` 到自己的缓冲再回调 ✓。

**最后一帧不会丢**：`UnInit` 里会先补发挂起的那一帧 ✓。

**默认行为完全不变**：`flag_on("QQ_NVENC_ASYNCCB", 0)` —— 不设这个变量就是原来那套同步回调 ✓。

### 怎么跑（一条命令）

```bash
# 完全退出 QQ（托盘；环境变量只在启动时读一次）
QQ_WAYLAND_FIX_ANGLE=off \
LD_PRELOAD=$HOME/coding/linuxqq-hwcodec/libqq-nvenc.so \
QQ_NVENC=1 QQ_NVENC_ACTIVE=1 QQ_NVENC_ASYNCCB=1 linuxqq-wayland-fix

# 开共享，动着屏幕 30 秒，然后：
cd ~/coding/linuxqq-hwcodec && ./nvenc-status.sh      # 应报「NVENC 在跑」
grep -aE 'ASYNCCB|出帧|回调' /run/user/1000/linuxqq-wayland-fix.log | tail -12
```

日志里应能看到配对的两行（延迟 + 补发）：

```
[qq-nvenc pid=…] NVENC: [ASYNCCB] 延迟回调 idx=2 28259 字节 qp=45（本次 DoEncode 直接返回，下次进入时补发）
[qq-nvenc pid=…] NVENC: [ASYNCCB] 补发上一帧回调 idx=2 len=28259
[qq-nvenc pid=…] NVENC: [ASYNCCB] 补发回调已返回
```

### 三组对照（同档位、都动着屏幕）

| 组 | 启动参数 | 看什么 | 结论 |
|---|---|---|---|
| A 同步（现状） | `QQ_NVENC=1 QQ_NVENC_ACTIVE=1` | 对端转圈/黑屏 | 复现已知问题 |
| **B 异步（本变体）** | **加 `QQ_NVENC_ASYNCCB=1`** | **对端出现画面** = 命中病因 ✓ | 若仍转圈 → 回调时序不是病因 |
| C 关掉 NVENC | 只 `QQ_NVENC=1`（或不注入） | 对端正常 | 对照组，证明"问题只在 NVENC 路径" |

**成功判据**：B 组里**对端能看到画面且不卡**（同时 `nvenc-status.sh` 仍报"NVENC 在跑"）。

### 如果 B 组也失败：把这几样留给作者

1. `grep -aE 'ASYNCCB|出帧|回调|会话就绪' …log | tail -20`
2. 同一次会话里 C 组的日志（对照）
3. 对端现象（转圈 / 黑屏 / 花屏 / 卡住）+ 你这边看到的源帧尺寸与帧率
4. 你这份 QQ 的版本号（`/opt/QQ/qq --version`）与 `libAVSDKPlugin.so` 的 sha256

### 说明

- 变体代码在 `/tmp/pr28-build` 这个 **worktree** 里（**未提交到上游**，也未碰主 clone）；改动已另存为
  `patches/pr28-asynccb.patch`（`+94/-5`，只动 `src/qq-nvenc.c`）
- 本变体**未经过真机共享验证**（构建与冒烟通过：`/bin/true`、`niri msg version` 不崩、开关默认关闭时行为不变）
- 有新代码路径就有新风险：若 B 组里对端/本端出现异常，直接去掉 `QQ_NVENC_ASYNCCB=1` 即回到 A 组行为 ✓

### 附：旁观模式 `-> data=… len=… idx=…` 的三种取值

这条是**第 1 步（旁观模式）**的判读，用来先排除"字段偏移不匹配"这个头号嫌疑：

| 看到 | 含义 | 下一步 |
|---|---|---|
| `data` 非空、`len` 合理（几千~几万字节）、`idx` 递增 | 偏移契约**对** ✓ | 排除偏移嫌疑 → 直接试本节的 B 组（异步回调） |
| `data` 为空/极小，或 `len`=0 / 超大（>4 MB） | `+0x18` / `+0x14` 偏移**与你这版 QQ 不匹配** ✗ | 先改偏移（别试回调，试了也判不出来） |
| `idx` 恒为 0 或不递增 | `+0x08` 不是帧号 ✗ | 下游可能按 idx 丢帧 → 也要先改偏移 |

抓法（旁观模式，`QQ_NVENC=1`，不改行为）：

```bash
grep -aE '原实现回调|-> data=' /run/user/1000/linuxqq-wayland-fix.log | tail -8
```

---

## 码流封装 / 参数对照实验（`QQ_NVENC_HEXDUMP` / `QQ_NVENC_BASELINE`）

同步版与异步回调版**对端都转圈** ✗，几何与 packet 偏移都已排除 → 剩下的最强嫌疑是**码流封装或编码参数和接收端不一致**（接收端拿到 NVENC 的 H.264 却解不出来）。
那两个开关是**只打印/改参数**的，不用换 `.so`，可以 A/B。

### A. 先看两条路径的码流首部（`QQ_NVENC_HEXDUMP=1`）

```bash
# 完全退出 QQ
QQ_WAYLAND_FIX_ANGLE=off LD_PRELOAD=$HOME/coding/linuxqq-hwcodec/libqq-nvenc.so \
QQ_NVENC=1 QQ_NVENC_ACTIVE=1 QQ_NVENC_HEXDUMP=1 linuxqq-wayland-fix

# 开共享、动着屏幕，然后：
grep -aE '\[原实现\]|\[NVENC\]' /run/user/1000/linuxqq-wayland-fix.log | tail -12
```

每个码流会打两行：`码流 len=… 首 16 字节: …` 和 `封装=… SPS=… PPS=… IDR=… profile=…`。
**原实现那条是在回调里【立刻】取的**（它复用同一块缓冲，延后读就没意义 ✗），NVENC 那条是在 `UnlockBitstream` **之前**取的 ✓。

| 看到 | 含义 | 下一步 |
|---|---|---|
| 两者都 `Annex-B`、都有 `SPS=有 PPS=有` | 封装一致 ✗ 不是病因 | 看 profile 是否不同 → 试 B |
| 原实现 `Annex-B`，NVENC `AVCC(4字节长度前缀)` | **封装不一致** ✓✓ 接收端按 Annex-B 解就会失败 | 这是 PR 需要修的（或本变体需要改封装） |
| NVENC 那条 `SPS=无` / `PPS=无` | 每帧没带参数集 → 接收端等不到关键帧信息 | 检查 `repeatSPSPPS` / `NV_ENC_PIC_FLAG_OUTPUT_SPSPPS` |
| `profile=` 不一致（如原实现 `0x42 Baseline`、NVENC `0x64 High`） | **档位不一致** ✓ 接收/转发链路可能只吃基线档 | 试 B |

### B. 强制 Baseline 档（`QQ_NVENC_BASELINE=1`）

```bash
# 完全退出 QQ
QQ_WAYLAND_FIX_ANGLE=off LD_PRELOAD=$HOME/coding/linuxqq-hwcodec/libqq-nvenc.so \
QQ_NVENC=1 QQ_NVENC_ACTIVE=1 QQ_NVENC_BASELINE=1 linuxqq-wayland-fix
```

生效时日志会写：`NVENC: 会话就绪 … , profile=Baseline(强制)`。它同时把熵编码切成 **CAVLC**（Baseline 不允许 CABAC）、**清空 VUI**、**去掉 AUD**。

**成功判据（A、B 都是这一条）：对端出现画面。** 还是转圈就按第三节"失败该留哪些证据"收集日志留给作者。

### 三组对照

| 组 | 命令里的开关 | 期望 |
|---|---|---|
| A 同步（现状） | `QQ_NVENC=1 QQ_NVENC_ACTIVE=1` | 对端转圈（已知 ✗） |
| B 异步回调 | `… QQ_NVENC_ASYNCCB=1` | 已试，仍转圈 ✗ |
| C 关掉 NVENC（可用态） | 只留 `QQ_NVENC=1` | 画面正常 ✓（对照组） |

（本轮新增：`QQ_NVENC_HEXDUMP`、`QQ_NVENC_BASELINE`；本地累计改动见 `patches/pr28-local-experiments.patch`，包含异步回调 + 这两个开关。）

## 病因与修复：SPS/PPS + 强制 IDR（`QQ_NVENC_FIXKEY=1`）

### 铁证（用户实测 HEXDUMP 输出）

```
[NVENC] 码流 len=171 首 16 字节: 00 00 00 01 09 30 00 00 00 01 61 e0 39 04 5f 00
[NVENC] 封装=Annex-B(4字节起始码) SPS=无 PPS=无 IDR=无 非IDR=有 profile=?
```

逐 NAL 拆开：`00 00 00 01 09 30` = AUD（type 9）；`00 00 00 01 61 e0 …` = **type 1，非 IDR**。
连续多帧都是这个形态 —— **没有 SPS/PPS、没有任何 IDR（关键帧）** ✗

H.264 必须先有 **SPS/PPS + 一个 IDR** 才能起播；只有 P 帧 = 一堆无法解码的差分数据 → **对端永远转圈** ✓
这就是病因（会话几何、packet 偏移、回调时序此前已逐个排除）。

### 为什么原来的设置没生效

代码里其实都设了：`idrPeriod = fps*2`、`repeatSPSPPS = 1`、`outputAUD = 1`，每帧也带
`NV_ENC_PIC_FLAG_OUTPUT_SPSPPS`，首帧还会加 `NV_ENC_PIC_FLAG_FORCEIDR`（`enablePTD = 1`，flag 合法）。
**但实测驱动就是不给 IDR、也不给参数集** —— 所以修法不能只依赖驱动守约 ✗

### 修了什么（`QQ_NVENC_FIXKEY=1`，默认关 = 原 PR 行为，可直接 A/B）

| 机制 | 说明 |
|---|---|
| **自己记 IDR 周期** | 首帧 + 每 `QQ_NVENC_IDR_INTERVAL`（默认 60）帧请求一次 `FORCEIDR`，不再依赖驱动的 `idrPeriod` |
| **参数集前缀** | 会话就绪后用 `nvEncGetSequenceParams` 缓存 SPS/PPS，**每帧码流前补一份**（码流里已含参数集时不重复补）；码流缓冲 Unlock 后失效，所以**先复制进我们自己的缓冲**再递下去 |
| **兜底重开** | `QQ_NVENC_REOPEN_IDR=1`：请求了 IDR 但驱动的 `pictureType` 仍不是 IDR → 下一帧重开会话（新会话必然带 SPS/PPS + IDR）|
| **诊断** | `QQ_NVENC_HEXDUMP=1` 时，前 8 帧打印 `req_flags` / `pictureType` / `is_idr` / 距上次 IDR 的帧数 |

### 怎么跑

```bash
# 完全退出 QQ（托盘；环境变量只在启动时读一次）
QQ_WAYLAND_FIX_ANGLE=off LD_PRELOAD=$HOME/coding/linuxqq-hwcodec/libqq-nvenc.so \
QQ_NVENC=1 QQ_NVENC_ACTIVE=1 QQ_NVENC_FIXKEY=1 QQ_NVENC_HEXDUMP=1 linuxqq-wayland-fix

# 开共享、动着屏幕，然后：
grep -aE '\[FIXKEY\]|封装=' /run/user/1000/linuxqq-wayland-fix.log | tail -12
```

### 判读

| 看到什么 | 结论 |
|---|---|
| `SPS=有 PPS=有 IDR=有` 且**对端出画面** | ✅ 修复成功 |
| `SPS=有 PPS=有` 但一直 `IDR=无` | 驱动忽略了 FORCEIDR → 加 `QQ_NVENC_REOPEN_IDR=1` 再试 |
| 仍 `SPS=无` | 参数集缓存失败（日志里有 `[FIXKEY] 取 SPS/PPS 失败 status=…`）→ 把 status 号发出来 |
| 码流已带参数集+IDR 但对端仍转圈 | 那就不是码流内容的问题，需要抓对端解码器日志（留给作者）|

### 离线验证（本机已过，不依赖 GUI）

把 `fixkey_build` 抠出来单测，7 项全过：P 帧补齐前缀 ✓、已带参数集（4 字节 / 3 字节起始码）不重复补 ✓、
垃圾输入安全 ✓、大帧多次调用容量增长不越界 ✓、未缓存参数集时退化为纯复制 ✓。
