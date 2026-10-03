# linuxqq-hwcodec

实验项目：让 LinuxQQ 的屏幕共享走 **硬件编解码（NVIDIA NVENC/NVDEC）**。

- **Phase 1 已完成**：探针查明它选了哪条编码路径 —— **全程没有任何编码库被 dlopen**、`NVENC 初始化 0 次`
  → 软编静态编进了 `broadcast-core.so`，NVENC 从未被尝试。
- **Phase 2 已就绪**：COM 层观测（编码器选择发生在哪个接口方法里），**默认关闭**，`HWPROBE_VTABLE=1` 才开。

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

## 故意不包 `dlvsym`

试过包 `dlvsym`（想让"调用方用版本化查找取符号"那条路也能被介入），**结果是破坏性的**：
glibc 自己会用它做版本探测，而我们的包装在解析不到真实实现时返回 `nil`：

```
dlvsym(0xffff...ffff, "dlopen", "GLIBC_2.34") -> (nil) [没找到]
```

动态加载链整体退化（`test/run-test.sh` 段 D 当场失败）。**结论：`dlvsym` 属于 glibc 内部机制，
不碰**；目标符号靠"导出同名符号"那条路介入。

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
