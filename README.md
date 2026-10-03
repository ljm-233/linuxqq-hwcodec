# linuxqq-hwcodec

实验项目：让 LinuxQQ 的屏幕共享走 **硬件编解码（NVIDIA NVENC/NVDEC）**。

- **Phase 1 已完成**：探针查明它选了哪条编码路径 —— **全程没有任何编码库被 dlopen**、`NVENC 初始化 0 次`
  → 软编静态编进了 `broadcast-core.so`，NVENC 从未被尝试。
- **Phase 2 已就绪**：COM 层观测（编码器选择发生在哪个接口方法里），**默认关闭**，`HWPROBE_VTABLE=1` 才开。

> 想让它跑在 N 卡上（不是编码，是渲染/解码，价值是堆积落显存而不是系统内存）：见文末「让 QQ 跑在 N 卡上：实验矩阵」。

## 结论：为什么用户侧切不了硬编

**PC 版 QQ 有 NVENC 实现，但用户侧没有任何入口能让它走硬编**；现在实际在编码的是
`libAVSDKPlugin.so` 自带的**软件**编码器（OpenH264 量级，实测 2560×1600 下每秒只编得动约 6 帧）。
完整证据、命令与日志片段见 [`docs/硬件编码调查.md`](docs/硬件编码调查.md)。

| # | 尝试 | 结果 |
|---|---|---|
| 1 | NVENC 实现在不在 | **在** `avsdk/broadcast-core.so`：`NvEncoder_Create/InitEncConfig/DoEncode`、`NvEncodeAPICreateInstance`、NVDEC（`cuvidCreateDecoder`）、AMD AMF、软编兜底 `OpenH264Encoder_*`；源码路径串 `…/broadcast-core/linux/NvEncoder.c` |
| 2 | 库齐不齐 | **齐**：`libnvidia-encode.so.1` / `libcuda.so.1` / `libnvcuvid.so.1` 都在，dlopen 实测全部成功 → **缺的不是库**（`libmfx.so.1`、`libamfrt64.so.1` 缺失） |
| 3 | 走 COM 入口强制 | **此路不通**：`broadcast-core.so` 只导出 3 个符号；探针实测共享全程 `dlsym=0`、`dlvsym` 77 次**无一**取 `DllGetClassObject`、`DllGetClassObject=0` → 那条路**根本没被走** |
| 4 | 按名字 hook `BroadcastCore_*` | **不行**：这些名字只在**字符串表**里（COM 接口方法名），没有导出 |
| 5 | 拦编码器本体（`libAVSDKPlugin.so`） | **不行**：它带 `FLAGS: SYMBOLIC`（`readelf -d`），内部调用绑定到自己，`LD_PRELOAD` 拦不到内部调用点 |
| 6 | SDK 自带的测试配置文件（最后一搏） | **实测无效**：键写对了、文件放进了 QQ 的工作目录、QQ 也在文件之后重启过 —— 日志**毫无输出**，见下一节 |
| 7 | 要真硬编 | 需**腾讯在 Electron 侧给出开关**（或换客户端）；配置、环境变量、符号介入三条路都到不了那个判断点 |

定位过程本身仍有价值：探针现在能证明「谁加载了 broadcast-core、取过哪些符号、调用了哪些接口」——
只是这次它证明的是**那条路没人走**。

## 已知事实（本机调查，2026-10-03）

| 事项 | 结论 |
|---|---|
| 采集/编码核心 | QQ 自带 `avsdk/broadcast-core.so`（470 KB） |
| 它导出的符号 | **只有 3 个**：`DllGetClassObject`、`DllCanUnloadNow`、`RegisterLog` —— COM 风格插件 |
| `BroadcastCore_EnumEncoderDevice` / `IsSupportedHardware` / `SwitchEncoderDevice` | 只在**字符串表**里 → 是 COM 接口的**方法名**，调用走虚表，**按名字 hook 不到** |
| 内建编码器 | NVENC（`NvEncoder_*`、`NvEncodeAPICreateInstance`）、NVDEC（`cuvidCreateDecoder`）、AMD AMF、软件兜底 `OpenH264Encoder_*` |
| 它会 `dlopen` | `libnvidia-encode.so`、`libcuda.so`、`libnvcuvid.so`、`libmfx.so.1`、`libamfrt64.so.1`、`libopenh264.so` |
| 本机库 | NVIDIA 三个都在，实测 **dlopen 全部成功** → **缺的不是库**；`libmfx.so.1`（Intel QSV）缺失 |
| 用户可见开关 | **没有**：QQ 配置里没有 encoder/hardware/codec 键，也没有真正的环境变量 |
| 实测线索 | 2560×1600 时客户端每秒只编码得动约 **6 fps** —— 正是软件编码（OpenH264）的量级 |

## 用法

**顺序错了日志就白跑**（2026-10-03 实测过一份 3768 行全是噪音的日志）：

```bash
./build.sh                                   # 1. 编译出 libhwprobe.so

pgrep -x qq                                  # 2. 必须没有任何输出：先完全退出 QQ（含托盘）

HWPROBE_VTABLE=1 ./linuxqq-hwcodec-probe     # 3. 用它启动 QQ（只看后端就不加 HWPROBE_VTABLE=1）
#    开一次屏幕共享，跑 30 秒左右

./linuxqq-hwcodec-probe --selfcheck-once     # 4. ★ 必须先看到「自检结论：可以采集」
cat ~/.cache/linuxqq-hwcodec/probe-*.log     # 5. 再回传日志
```

探针是 `LD_PRELOAD` 库，**不改 QQ 任何文件**；恢复原状照常从原图标启动即可。

## 判读日志

| 日志里出现 | 含义 |
|---|---|
| `NvEncodeAPICreateInstance(...) -> 0` 且映射了 `libnvidia-encode.so` | **已经在用 NVENC 硬编** → 那 6 fps 天花板与编码器无关 |
| `NVENC 初始化 0 次`、没映射任何编码库 | **软编且从未尝试硬编**（Phase 1 实测结果） |
| `dlopen("libnvidia-encode.so") -> 失败` | 才是缺库/名字不对（本机不会发生） |

Phase 2 还要看这些（`HWPROBE_VTABLE=1` 时才有）：

| 日志里出现 | 含义 |
|---|---|
| `已代理 IClassFactory` + `vtable 已挂钩：obj=… 原 vtable=…` | 接口对象已被接管，之后**每次方法调用都会记一行** |
| `vtable[7] ret this=0x0000000000000000` | **重点**：某个方法返回了 0/false —— 很可能就是『硬件编码是否受支持』的判断点 |
| `vtable[3] 调用 1 次` 这类热点统计 | 哪个槽位被反复调用（每帧都调的多半是"送一帧"） |
| `vtable hook 跳过：…不可写` | 安全阀生效（宁可不挂钩，也不冒险写坏 QQ 内存） |

## 为什么这三个符号必须"导出"

`LD_PRELOAD` 的介入靠**动态链接器按名字解析**，所以我们要拦的符号必须自己也是**同名动态符号**：

| 名字 | 为什么 |
|---|---|
| `DllGetClassObject` | broadcast-core 唯一的入口。`libAVSDKPlugin.so` 是**直接链接**它走 COM 的（`QRTCServiceInterfaceWrapper::InitBroadcastCore`），全程没有 `dlsym` —— 只做"static 包装 + dlsym 转发"的话，这条路**根本不经过我们** |
| `NvEncodeAPICreateInstance` / `NvEncodeAPIGetMaxSupportedVersion` | 同上：直接链接时只有导出符号才拦得住 |

以前这三个是 `static`，`nm -D` 里看不到 —— 于是接口创建、NVENC 初始化全都观测不到（日志里
`vtable` 一直 0 行）。现在 `./build.sh` 结束会直接把导出情况打出来：

```
--- 动态符号：被介入的目标（必须是导出的同名符号；否则直接链接的调用方绕过我们）
  ✓ DllGetClassObject
  ✓ NvEncodeAPICreateInstance
  ✓ NvEncodeAPIGetMaxSupportedVersion
```

**怎么知道这次走的是哪条路**（日志里各有一行）：

| 日志 | 含义 |
|---|---|
| `符号介入路径：DllGetClassObject 被调用 —— 导出符号介入生效（动态链接器直接解析到我们，全程没有 dlsym）` | 真实调用方走的就是这条 |
| `符号介入路径：DllGetClassObject 是调用方用 dlsym 取走的（交给我们的实现）` | 调用方先 dlsym 拿指针 |

两个坑记在这，免得再踩：

1. **转发目标只能用 `RTLD_NEXT`**，绝不能拿 `RTLD_DEFAULT` 兜底 —— 我们自己就导出这个名字，
   `RTLD_DEFAULT` 会解析回我们 → **无限递归 → 栈溢出**（实测 rc=139）。
   所以 `next_definition()` 只走 `RTLD_NEXT`，并再挡一道"结果等于自己就作废"。
2. `dlvsym(handle, name, NULL)` 是**未定义行为**（glibc 把 version 标成 nonnull），
   在无版本符号上直接段错误。无版本符号要用真正的 `dlsym`。

回归测试：`test/direct.c` 故意**直接链接**假 COM 模块（不走 dlsym），配合
`test/run-test.sh` 的 D 段断言"导出符号介入生效 + 工厂照样被代理"。

## 长命进程也要能当场看到计数（周期汇总）

主进程 `/opt/QQ/qq` 是长命的，而汇总只在**进程退出时**才写 —— 于是"它到底有没有 dlopen
broadcast-core、有没有走 DllGetClassObject"在日志里永远看不到（2026-10-03 的日志就是这样）。
现在每个相关进程每隔一段时间写一行定长摘要（`write()`，不碰 stdio/malloc）。

**惰性打印，不使用任何信号与定时器。** 摘要是在我们本来就会执行的地方顺手判断的
（`dlopen` / `dlsym` / `DllGetClassObject` / NVENC 入口被调用时，距上次 ≥ `HWPROBE_PERIOD`
就写一行）；没有这些调用就不打印 —— 这够用，因为要观测的正是这些调用。周期摘要只对
"相关进程"生效（cmdline 含 `qq`/`ppapi`，或设了 `HWPROBE_ALL=1`）。

> ⚠️ 早期版本用 `signal(SIGALRM)+alarm()` 实现这件事：本库被注入到**每一个子进程**里，
> 于是每个进程 20 秒后都会收到 SIGALRM —— 默认动作是终止进程，**等于把宿主打死**。
> 2026-10-03 用户实测：启动器被 SIGALRM 杀掉、QQ 根本没起来。所以本探针
> **不安装任何信号处理、不使用任何定时器**（`grep -nE 'alarm|setitimer|timer_create|sigaction|signal *\(' src/` 应为空）。
> 也因此**不再支持用 `SIGUSR1` 中途 dump**：需要中途数据就看周期摘要，或正常退出时的汇总。
> 回归测试：`test/run-periodic-test.sh`（长命宿主 9 秒 + fork 子进程，断言宿主不被信号杀死、
> 且周期摘要 ≥2 行）。

```
[hwprobe] 周期汇总 pid=4132253 dlopen=0(失败 0) dlsym=0 DllGetClassObject=0 nvenc_api=0 nvenc_ver=0 broadcast-core=否 dlopen库数=0 编码库=0
```

- 默认 20 秒一条；`HWPROBE_PERIOD=1` 可改成 1 秒（调试用），`=0` 关闭
- 汇总里新增 **`--- 本进程 dlopen 过的库（N 个）---`**：库名去重列表，这样"谁在什么时候
  加载了 broadcast-core"一眼可见（之前只记次数，名字全丢）

## `dlvsym` 必须包，但只能纯透传

**为什么必须包**：QQ 的调用方是 `dlopen("broadcast-core.so")` 之后用
`dlvsym(handle, "DllGetClassObject", "VERS_1.0")` 取入口的 —— 符号本身带版本
（`nm -D` 显示 `DllGetClassObject@@VERS_1.0`），所以日志里 `dlsym` 一直是 0。
**只包 `dlsym` 会把整条路漏掉。**

**第一版为什么翻车**：当时"解析不到真实实现就返回 `nil`"，而 glibc 自己也会用 `dlvsym`
做版本探测 —— `dlvsym(RTLD_DEFAULT, "dlopen", "GLIBC_2.34")` 变 `nil`，动态加载链整体退化
（`run-test.sh` 段 D 当场失败）。现在的规矩：

1. 只对三个目标名字做拦截，其余**原样透传**（含 `version == NULL` 的调用）
2. 真实实现优先 `real_dlsym(RTLD_NEXT, "dlvsym")`，取不到就**扫 ELF 动态符号表**
   （`dl_iterate_phdr`，不经过任何 PLT）；再取不到才退化成不带版本的 `dlsym`
   —— **绝不无缘无故返回 `nil`**
3. 引导必须扫 ELF：我们一旦导出 `dlvsym`，连自己文件里的 `dlvsym(...)` 调用也会被
   动态链接器解析回我们自己，而"真实的 dlvsym"没法再用 dlsym/dlvsym 取（鸡生蛋）。
   注意现代 glibc 的 `libc.so.6` **只有 `DT_GNU_HASH`、没有 `DT_HASH`** —— 只认后者会
   直接扫不到（实测踩过）

**还有一个只有包了才会遇到的坑**：包上之后，**我们自己**的符号解析
（`hwprobe_lookup_real` / `next_definition` 里的 `dlvsym(...)`）也会落进我们的包装，
如果目标名字恰好是 `DllGetClassObject`，`route_special` 会把它当成"调用方来取符号"，
把 `cls_handle` 覆盖成 `RTLD_NEXT` —— 真实实现再也找不到，A/B/C 三段测试同时挂掉。
修法是内部解析走 `internal_lookup` 标志短路：**纯转发、不计数、不路由**。

回归测试：`test/dlvdriver.c` 复现真实取法（`dlvsym` + 版本号），`run-test.sh` 的 E 段断言
"拦截生效 + 工厂照样被代理 + **glibc 自己的版本化查找没被弄坏**"，F 段再做 `ldd` /
`python3 -c import ctypes` 等冒烟。

## 一个被数据推翻的推断（别再去主进程钩）

曾经推断"COM 对象是在主进程里创建、ppapi 由 fork 继承，所以要在主进程里钩"。实测**不成立**：
在一次真正的共享中，自检给出

```
自检：ppapi(pid=3955126) 探针=在 修复库=在 broadcast-core=在 共享=进行中 | 主进程(pid=3952917) 探针=在 | 加载 broadcast-core 的进程: 3955126
```

**broadcast-core 只出现在 ppapi 进程里**，主进程根本没有它 —— 代码不在那儿，COM 对象也不可能
在那儿创建。所以钩子该在**收帧进程**里生效，自检也从"只报 ppapi"改成**同时报主进程的探针状态
和所有加载了 broadcast-core 的进程**（别假定一定是 ppapi）。

## 实现要点：转发桩为什么不挪栈

常见做法是把参数寄存器压到栈上再调真实函数 —— 那样 `rsp` 跑到调用者的栈参数下面去了，
**方法一旦有 7 个以上整数参数（走栈），真实实现读到的就是错位数据**。这里改成：参数寄存器与
`xmm0-7` 全存进 **TLS**（不在栈上，被调函数碰不到），日志记完原样恢复；调真实方法时把 `rsp`
还原成"调用者直接 call 它"的值，**栈参数位置与直接调用完全一致**，返回值也经 TLS 原样交还。
测试里专门放了 `m8(1..7)`（7 个整数参数）钉死这一点：

```
$ test/run-test.sh
  ✓ m8(1..7) = 1234567（栈参数没错位）   ← 改成搬栈写法，这条立刻失败
  ✓ 默认模式不代理工厂                   ← 不设 HWPROBE_VTABLE 时不动宿主
  ✓ 日志被限流（HWPROBE_VTABLE_MAXLOG 生效）
```

## 已知副作用（实测，已做对照实验）

- **`dlsym` 拦截本身会改变某些驱动的探测**：`eglinfo` 无 preload 时输出 7 段 EGL 信息、退出码 1，
  挂上「正确的纯转发 dlsym 包装」后变 6 段、退出码 2；**我们的探针与那个纯转发包装结果完全一致** ——
  所以不是本项目代码的问题，是这一手段固有的副作用（Phase 1 在 QQ 上跑过两份各 1 分钟日志，无异常）。
- 不设 `HWPROBE_VTABLE` 就完全不碰 COM 层。

## 风险与免责

- 纯实验。`LD_PRELOAD` 出问题最多让 QQ 起不来 —— 用原来的图标启动即可恢复，不需要改任何文件
- 退出 QQ 请用托盘/界面，**不要 `pkill qq`**（本项目不提供按名字杀进程的脚本）

## 关联项目

- [niri-portal-cast](https://github.com/ljm-233/niri-portal-cast)：合成器侧（让 niri 的共享对 Electron 可用 + 限帧率/分辨率 + 内存刹车）
- [wayland-cast-doctor](https://github.com/ljm-233/wayland-cast-doctor)：排查共享不出画面的诊断脚本
- [linuxqq-wayland-fix](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix)：QQ 客户端侧注入修复（本项目的 hook 写法参考它）

## 日志里只该有 QQ 进程

`LD_PRELOAD` 会被**每一条子命令**继承 —— 启动器自己跑的 `date`/`pgrep`/`grep`/`head`/`pw-dump` 也不例外，
它们会各自在日志里写一份十几行的汇总块。2026-10-03 用户交回来的一份日志就是这样：
**3768 行 / 232 个进程块，几乎全是噪音，有效数据为零**。两个地方都堵上了：

- 启动器里所有辅助命令都摘掉 `LD_PRELOAD` 再跑（`nopre()`；实测这些命令现在在日志里占 0 行）
- 探针只对 **cmdline 命中 `qq`/`ppapi`** 的进程输出汇总块，其它进程只留一行
  `hwprobe 已注入 pid=… cmdline=…（非 QQ 进程，只记这一行）`；要看全部进程就设 `HWPROBE_ALL=1`
  （自检脚本用的就是它）

所以日志里看到 `命令行：date`、`命令行：pgrep` 这类块，说明跑的不是现在这版启动器。

## 收帧进程是 fork 出来的，它的块长这样

`--type=ppapi` 不是新 exec 的进程，而是主进程 **fork** 出来的：探针的构造函数不会重跑，
而且 Chromium 会关掉它不认识的 fd —— 继承来的日志 fd 就此消失。2026-10-03 实测：
ppapi 进程里探针在（maps 里能看到）、`HWPROBE_LOG` 也设了，**却一行都没写**，日志里只剩辅助命令的噪音块。

现在每次写日志前都会确认 fd 还活着，不活就按 `HWPROBE_LOG` 重开，并留一行：

```
[hwprobe] … 日志 fd 被继承后关闭，已重新打开（本进程 pid=… 多半是从主进程 fork 出来的）
```

所以**收帧进程的块没有开头那几行**（`=== hwprobe 已注入 ===` / `命令行：` / `环境：`），
只有这行重开提示加上后面的 `dlopen` / `dlsym` / `vtable` 记录 —— 看到它就说明**抓到了**。
另外探针还会往 stderr 写一行（启动器把 QQ 的 stderr 收进 `/run/user/1000/linuxqq-wayland-fix.log`），
用来确认构造函数到底跑没跑。

`test/run-fork-test.sh` 复现并验证了这条路径（fork → 关掉 fd≥3 → dlopen → 日志仍在）。

## 先看自检结论，再决定这份日志值不值得看

`--selfcheck-once` 会把状态行与结论行**同时打印并写进日志**（结论行以 `自检结论：` 开头）。只有这一种
情况日志里的 `vtable[N]` 才是真数据：

| `自检结论：` | 含义 |
|---|---|
| `可以采集 —— 探针=在 修复库=在 broadcast-core=在 共享=进行中` | **唯一有效**，可以回传 |
| `失败 —— 没有收帧进程。请先完全退出 QQ（含托盘，pgrep -x qq 应为空），再用本启动器启动，然后开一次共享。` | QQ 没开或没共享就跑了探针 → 按顺序重来 |
| `失败 —— 探针不在收帧进程（它属于另一个 QQ 实例？先完全退出 QQ 再启动）` | 旧实例还活着，探针进不去 → 完全退出后重来 |
| `失败 —— 修复库不在收帧进程里（这样共享会失败；确认走的是 linuxqq-wayland-fix）` | 启动器链路不对 |
| `等待 —— 探针已就位，但共享还没开始` / `等待 —— 正在共享，但 broadcast-core 还没加载` | 开共享 / 等几秒再跑一次 |

每个进程块都带 `命令行：…` 与 `父进程：N`，不用再猜哪块是哪个进程。不想启动 QQ、只想看现状：
`./linuxqq-hwcodec-probe --selfcheck-once`（失败时退出码非 0）。

## 那次"最后一搏"：测试配置文件实测无效（2026-10-03）

SDK 里的字符串确实承诺了两个"本地测试配置文件"开关：

| 文件 | 键 | 命中时会打的日志 |
|---|---|---|
| `aMavEngineConfig.txt` | `uiUseHw`、`uiUseHWAccelerate` | `aMavEngineConfig.txt: uiUseHw[%d->%d]` |
| `aConfig.txt` | `dwUseHWAccelerate`、`HwEnc` | `be careful local has test config file aConfig.txt: dwUseHWAccelerate[%d->%d]` |

实测过程（**文件按 SDK 的相对路径语义放进 QQ 的进程工作目录**）：

```bash
readlink /proc/$(pgrep -x qq | head -1)/cwd        # → /home/link，证明工作目录
printf 'uiUseHw=1\nuiUseHWAccelerate=1\n' > /home/link/aMavEngineConfig.txt
printf 'HwEnc = 1\ndwUseHWAccelerate = 1\n' > /home/link/aConfig.txt
# 完全退出 QQ → 重启（进程启动时间晚于两个文件的 mtime）→ 开一次共享
grep -aiE 'HwEnc|uiUseHw|UseHWAccelerate|hardware:|use_hardware' \
     /run/user/1000/linuxqq-wayland-fix.log | tail
```

结果：**没有任何输出**（既没有 `[0->1]`，也没有加载/解析失败的任何提示）。

判定：这段"本地测试配置文件"代码在 PC 版里**很可能未启用** —— 旁证是库里那条配置路径写的是
Android 的绝对路径 `/sdcard/Android/data/com.tencent.mobileqq/aMavEngineConfig.txt`（该库是手机端
共用的）。因此**用户侧无手段切换到硬编**，这就是本项目的最终答案。

## 查找名统计（2026-10-03 新增）

日志以前只有计数（`dlvsym 77 次`），没有名字，无法判断那些查找是否与编码器有关。现在每次
`dlsym`/`dlvsym` 的查找名都会**去重计数**，并由汇总打印 top：

```
dlsym/dlvsym 查找名 top8（共 4 种）：cos=2 sin=1 pow=1 NvEncodeAPICreateInstance=1
```

退出汇总印 top-8，周期汇总印 `查找名top3=`；想看逐条明细仍然用 `HWPROBE_ALL=1`。

## 让 QQ 跑在 N 卡上：实验矩阵

**为什么做这件事**：QQ 跑在核显（i915）上时，它持有的缓冲记账在**系统内存**（`/proc/meminfo`
的 `Shmem`），共享时会一路涨到把机器冻死；跑在独显上，同样的堆积落在**显存（8 GB）**，机器不容易冻。
注意这与"硬件**编码**"是两件事 —— 后者今天已证明用户侧改不了（见顶部结论）。

**已知走不通的两条**（先前会话实测，别再试）：
- 强制 NVIDIA **EGL**（`__EGL_VENDOR_LIBRARY_FILENAMES=10_nvidia.json`）：EGL 确实换成了 NVIDIA、
  llvmpipe 消失、ppapi CPU 从 ~273% 降到 ~5%，**但对端没有画面**；`eglinfo` 的原因：
  `Wayland platform: eglInitialize failed`（Mesa 在同一平台成功）
- `DRI_PRIME=2`：被 Mesa 拒绝（`Should be < 2 (GPU devices count)`）

| 配置 | 改什么 | 期望 | 风险 |
|---|---|---|---|
| `baseline` | 什么都不改（= 你现在在用的） | 核显 + llvmpipe，画面正常 | 无（作为对照与回退） |
| `vulkan-nvidia` | `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` 锁 NVIDIA ICD + `QQ_WAYLAND_FIX_ANGLE=vulkan` | ANGLE 走 NVIDIA Vulkan，最可能真上独显 | **画面可能变小** —— `--use-angle=vulkan` 正是"视频画面缩放错乱"的元凶（你今天为此固化了 `QQ_WAYLAND_FIX_ANGLE=off`） |
| `angle-gl-nvidia` | `QQ_WAYLAND_FIX_ANGLE=off` + 追加 `--use-gl=angle --use-angle=gl`（启动器会把参数原样透传） | 走 ANGLE 桌面 GL（经 GLX/NVIDIA），绕开 EGL 那个失败点 | GLX 在 Wayland 下不一定可用 |
| `prime-1` | `DRI_PRIME=1` | Mesa 自己选设备 | 先前只试过 2/0，1 未试；可能仍落在核显 |

**判据只有一条**：`verdict` 里**收帧进程 ppapi** 打开的是 `renderD129`（NVIDIA）。
其它子进程碰过 renderD129 不算 —— 堆积记在 ppapi 身上。

```
./try-gpu-config.sh verdict            # 只读：现在到底在哪个 GPU 上（随时可跑）
./try-gpu-config.sh dry-run vulkan-nvidia   # 先看将要执行什么，不启动
./try-gpu-config.sh vulkan-nvidia      # 用该配置启动（要求 QQ 已完全退出）
./try-gpu-config.sh baseline           # 回到你现在这个正常状态
```

**两个目标可能冲突**：让 ANGLE 走 NVIDIA（`vulkan-nvidia`）与"画面不变小"（需要 `QQ_WAYLAND_FIX_ANGLE=off`）
目前看是矛盾的。建议顺序：先试 `angle-gl-nvidia`（不碰 ANGLE 的 vulkan 后端，画面风险最小），
再试 `vulkan-nvidia`（最可能上独显，但要接受画面可能变小）；**优先保证画面正常** —— 上不了独显只是
"会冻机"的老问题（已有档位与刹车兜住），画面坏了共享就没法用了。

本脚本**不会杀任何进程**：QQ 还在跑时它会拒绝启动（QQ 是单实例，不退出新实例不会接管）。
