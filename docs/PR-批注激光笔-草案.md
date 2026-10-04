# PR 草案：修 QQ 屏幕共享的批注 / 激光笔（叠加窗口尺寸未按输出缩放换算）

## 结论

QQ 的批注 / 激光笔靠一块**独立的"绘制窗口"**实现：它被创建为叠加层，再由 AVSDK
（`libAVSDKPlugin.so`）通过 `StartDrawingWindowCapture` 单独捕获、合成进共享流。
**这块窗口声明的逻辑尺寸恒等于屏幕的物理分辨率（2560×1600），从未按 Wayland 的输出缩放换算** ✗，
因此在分数缩放（1.5 / 1.25）下它比逻辑屏幕大 1/scale 倍，导致坐标系原点错位。

## 根因证据（本机实测：niri，输出 2560×1600，scale 1.5）

| 输出 scale | 逻辑屏幕 | 叠加窗口自报（`niri msg windows`）| 结果 |
|---|---|---|---|
| 1 | 2560×1600 | 2560×1600 | **正确** ✓（用户反馈"scale 1 更正常"）|
| **1.5** | **1706×1066** | **2560×1600** | 大 **1.5** 倍 ✗ |
| 2 | 1280×800 | 2560×1600 | 大 **2** 倍 ✗ |

症状链（全部可由上表解释）：画布比屏幕大 → 坐标系原点错 → 工具条出现在"逻辑中心"、
笔迹落点偏移 → 把它设成全屏时整块不透明表面铺满屏幕 → **所有窗口变黑** ✗

## 实现落点（本次新取证）

- `libAVSDKPlugin.so` 里存在这条链（`strings` 可见 mangled 名字与日志串）：
  - `_ZN27QRTCServiceInterfaceWrapper25StartDrawingWindowCaptureEjm`
  - `_ZN27QRTCServiceInterfaceWrapper26RemoveDrawingWindowCaptureEj`
  - `_ZN27QRTCServiceInterfaceWrapper14LogDrawingRectEPKcjPS1_S2_Pmjj`
  - 日志串：`avsdk output(wrapper): StartDrawingWindowCapture Screen Capture` /
    `RemoveDrawingWindowCapture No Existing Drawing window capture`
  → **批注窗口的"捕获 + 合成"在 AVSDK 里** ✓，与本项目已有挂钩手法（PR #28 挂钩
  `CreateH264Encoder`）同属一个库，落点可行 ✓
- **QQ 主进程是原生 Wayland 客户端** ✓：`/proc/PID/fd` 里 **0 个 X11 socket**、4 个 Wayland socket
  （`libX11` 虽被映射但未建连接）→ 修法必须是 **Wayland/客户端侧**，不能靠 X11 几何 hook ✗
- **`libqq-borderfix.so` 与这块窗口无关** ✗：其源码里没有 X11 / `wl_surface` / GTK / 尺寸相关调用
  （它管的是共享边框与截图覆盖层标题 ✓，不是这块绘制窗口）

## 已试过且无效的路（避免作者重复走）

| 做法 | 结果 | 原因 |
|---|---|---|
| `--force-device-scale-factor=1` 启动 QQ | ✗ 无效 | 不影响这块窗口的逻辑尺寸 |
| niri `set-window-width/height`（合成器侧改尺寸）| ✗ 客户端仍按错基准画 | 改的是合成器侧，不是客户端声明的逻辑尺寸 |
| niri `window-rule`（含 `default-floating-position`）| ✗ 同上 | 规则改不了客户端自报尺寸 |
| 把该窗口全屏 | ✗ 全屏后整块不透明 → **所有窗口变黑** | 表面本身就是 1.5 倍大且不透明 |

## 修法候选（择一或组合，均需 scale=1 幂等）

**A. 在 AVSDK 捕获链上做几何补偿（推荐先试）** ✓
挂钩 `StartDrawingWindowCapture`（必要时含 `RemoveDrawingWindowCapture` / `LogDrawingRect`），
把绘制窗口的**捕获几何**按其应为的逻辑尺寸换算（除以当前输出 scale，或按"物理/逻辑"比缩放），
使合成到共享流里的位置与尺寸正确。用项目既有手法（`dlsym` + 校验入口字节 + 失败可回落）实现 ✓

**B. 在窗口创建处修正逻辑尺寸**
先定位**谁创建**这块窗口（QQ 的 Electron/JS 侧？某个 native lib？），再把其逻辑尺寸按输出缩放相除。
需要进一步取证（见下"待确认"）。

## 必须满足的约束

- **scale = 1 时幂等**（除以 1 = 不变 ✓）—— 这是审查会盯的点
- 不改变其它 QQ 窗口，不影响共享本身
- 不破坏 X11 / KDE / GNOME（各环境缩放机制不同）
- 失败必须回落（画不出 ≠ 崩），与项目既有安全网一致

## 验证方法（可复现、可留证）

1. scale 1.5 下开共享 → 开批注 → **画一条从左到右的横线**
2. 修前：观众端位置偏 / 画不出；修后：**观众端与本地一致** ✓
3. 留"修前/修后"两张对照图 ✓（`LogDrawingRect` 日志可用于量化矩形坐标 ✓）
4. 回归：scale 1、scale 2 各测一次 ✓；确认其它窗口不受影响 ✓

## 建议的 PR 内容清单

1. 代码：新增一个极小的注入库（或并入既有库），只做上面的换算 ✓
2. `--doctor`：新增一条检查 —— 对比"输出逻辑尺寸"与"绘制窗口自报尺寸"，不一致就提示 ✓
3. README / docs：一节说明该问题与**临时 workaround（把输出设成整数缩放，scale 1 时正常）** ✓
4. 复现步骤 + 前后对照图 ✓

## 待确认（下一步取证，越少越好）

1. 那块绘制窗口**由谁创建**（Electron/JS 还是 native lib）—— 决定候选 B 是否可行
2. `StartDrawingWindowCapture` 的两个参数语义（`uint` 是否为窗口 id、`ulong` 是什么）✓
3. 捕获几何的确切公式：AVSDK 拿到的是"窗口物理像素"还是"逻辑尺寸"（决定换算用 `1/scale` 还是 `buffer/logical`）✓
4. 是否只影响批注，还是激光笔共用同一块窗口（从符号看应共用 ✓，待实测确认）
