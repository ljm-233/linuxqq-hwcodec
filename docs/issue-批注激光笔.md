**发现环境：niri** —— 以下实测都在 niri 上做的，**KDE / GNOME 未验证**。

| 项 | 值 |
|---|---|
| 合成器 | **niri**（自编译，含 `niri-portal-cast`，采集只宣告 SHM）|
| 输出 | 2560×1600 @ **scale 1.5** → 逻辑 1706×1066；分数缩放走 `wp_fractional_scale_v1` |
| 共享 / 客户端 | `xdg-desktop-portal` + PipeWire；QQ 3.2.34-53644 + `linuxqq-wayland-fix` 0.2.14.test3-1 |

## 现象

批注 / 激光笔靠一块独立的**绘制窗口**（叠加层）实现。该窗口自报的**逻辑尺寸恒等于物理分辨率（2560×1600）**，不随输出缩放变化：

| 输出 scale | 逻辑屏幕 | 绘制窗口自报（`niri msg windows`）| 结果 |
|---|---|---|---|
| 1 | 2560×1600 | 2560×1600 | 正确 |
| **1.5** | **1706×1066** | **2560×1600** | **大 1.5 倍** |
| 2 | 1280×800 | 2560×1600 | 大 2 倍 |

（该窗口为 `Title "屏幕共享"`、`App ID "QQ"`、`Is floating: yes`。）

症状链：画布比屏幕大 → 坐标原点错 → 工具条出现在"逻辑中心"、笔迹落点偏移 → 把它设成全屏时整块不透明表面铺满 → **所有窗口变黑**。

## 复现（niri）

```bash
niri msg output eDP-1 scale 1.5                    # 确保是分数缩放
# 开共享 → 开批注 / 激光笔
niri msg windows | grep -B2 -A6 '屏幕共享'          # 看那块浮动窗口自报的尺寸
```

判据：自报 2560×1600 而逻辑屏只有 1706×1066 → 复现；把 scale 改成 1 / 2 再看，自报**始终** 2560×1600（完全不随缩放变化）。

## 已试过、无效

| 做法 | 结果 |
|---|---|
| `--force-device-scale-factor=1` 启动 QQ | 不影响该窗口的逻辑尺寸 |
| niri `set-window-width` / `set-window-height` / `window-rule` | 改的是合成器侧尺寸，客户端仍按错基准画 |
| 把该窗口设成全屏 | 整块不透明铺满 → 所有窗口变黑 |

## 线索

- 绘制窗口的捕获 / 合成在 **`libAVSDKPlugin.so`**：`StartDrawingWindowCapture` / `RemoveDrawingWindowCapture` / `LogDrawingRect`，日志串 `StartDrawingWindowCapture Screen Capture`（与 PR #28 挂钩的 `CreateH264Encoder` 同属一个库）。
- **QQ 主进程是原生 Wayland 客户端**（`/proc/PID/fd`：0 个 X11 socket、4 个 wayland socket）→ X11 几何 hook 不可行。
- `libqq-borderfix.so` 与此无关（其源码里没有 X11 / `wl_surface` / GTK / 尺寸相关调用）。

## 临时规避

需要批注 / 激光笔时，把输出临时设成 **scale 1**：`niri msg output eDP-1 scale 1`（用完改回）。

## 未验证

**KDE / GNOME 的 150% 缩放是否同样表现，我们没有环境验证** —— 不确定这是否为分数缩放合成器共有，欢迎复现或反驳。

**我们不提 PR，交给作者修。**
