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

```bash
./build.sh                                  # 1. 编译出 libhwprobe.so

./linuxqq-hwcodec-probe                     # 2a. Phase 1：只看后端（不碰 COM）
HWPROBE_VTABLE=1 ./linuxqq-hwcodec-probe    # 2b. Phase 2：连带观测 COM 方法调用

#    两种都用它启动 QQ，然后开一次屏幕共享跑 30 秒，最后从托盘正常退出 QQ
cat ~/.cache/linuxqq-hwcodec/probe-*.log    # 3. 看日志（发回即可）
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
