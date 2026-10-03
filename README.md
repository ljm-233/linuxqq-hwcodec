# linuxqq-hwcodec

实验项目：让 LinuxQQ 的屏幕共享走 **硬件编解码（NVIDIA NVENC/NVDEC）**。
当前处于 **Phase 1：探针** —— 先看清它到底选了哪条编码路径，不急着强制切换。

## 已知事实（本机静态调查，2026-10-03）

| 事项 | 结论 |
|---|---|
| 采集/编码核心 | QQ 自带 `avsdk/broadcast-core.so`（470 KB） |
| 它导出的符号 | **只有 3 个**：`DllGetClassObject`、`DllCanUnloadNow`、`RegisterLog` —— 是 COM 风格插件 |
| `NvEncoder_*` / `OpenH264Encoder_*` / `AmfEncoder_*` | 只存在于**字符串表**（内部 vtable），**不是动态符号，无法按名字 hook** |
| 它会 `dlopen` | `libnvidia-encode.so`、`libcuda.so`、`libnvcuvid.so`、`libmfx.so.1`、`libamfrt64.so.1`、`libopenh264.so` |
| 本机库 | NVIDIA 三个都在，且实测 **dlopen 全部成功**（`libcuda.so`、`libnvcuvid.so` 这些无版本号的软链也在）→ **缺的不是库** |
| 用户可见开关 | **没有**：QQ 配置里没有 encoder/hardware/codec 键，也没有真正的环境变量 |
| 实测线索 | 2560×1600 时客户端每秒只编码得动约 **6 fps** —— 正是软件编码（OpenH264）的量级 |

## 用法（三步）

```bash
./build.sh                      # 1. 编译出 libhwprobe.so
./linuxqq-hwcodec-probe         # 2. 用它启动 QQ（会打印日志路径）
                                #    然后开一次屏幕共享，跑 30 秒左右，正常退出 QQ
cat ~/.cache/linuxqq-hwcodec/probe-*.log   # 3. 看日志（把它发回即可）
```

探针是 `LD_PRELOAD` 库，**不修改 QQ 任何文件**；恢复原状只要照常从原来的图标启动。

## 判读日志

| 日志里出现 | 含义 |
|---|---|
| `NvEncodeAPICreateInstance(...) -> 0` 且映射了 `libnvidia-encode.so` | **已经在用 NVENC 硬编** → 那 6 fps 天花板与编码器无关 |
| 只映射了 `libopenh264.so`，`NVENC 初始化调用 0 次` | **当前是软件编码**（Phase 2 才谈强制切换） |
| `dlopen("libnvidia-encode.so") -> 失败` | 才是缺库/名字不对（本机不会发生） |

关键几行：`后端加载情况：…`、`NVENC 初始化调用 N 次`、`--- 进程映射到的编码相关库 ---`。

## Phase 2 会做什么（等 Phase 1 结论）

如果确认是软编：在 `dlopen`/`dlsym` 层把 NVENC 入口接管，让 broadcast-core 的 `NvEncoder_IsSupport`
走到"支持"，并观察是否真的建会话、是否真的编码（判据：`nvidia-smi` 出现 Enc 会话、ppapi CPU 下降、
内存净增长从 398 MB/s 掉到接近 0）。

## 风险与免责

- 纯实验。`LD_PRELOAD` 出问题最多让 QQ 起不来 —— 用原来的图标启动即可恢复，**不需要改任何文件**
- 退出 QQ 请用托盘/界面，**不要 `pkill qq`**（本项目不提供任何按名字杀进程的脚本）

## 关联项目

- [niri-portal-cast](https://github.com/ljm-233/niri-portal-cast)：合成器侧（让 niri 的共享对 Electron 可用 + 限帧率/分辨率 + 内存刹车）
- [wayland-cast-doctor](https://github.com/ljm-233/wayland-cast-doctor)：排查共享不出画面的诊断脚本
- [linuxqq-wayland-fix](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix)：QQ 客户端侧注入修复（本项目的 hook 写法参考它）
