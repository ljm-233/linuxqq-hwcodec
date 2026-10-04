# Easy Effects 排除名单：加了 TRAE（2026-10-04）

## 症状与来源

`linuxqq-wayland-fix --doctor` 报：

```
! 检测到 Easy Effects：请在「输入」「输出」的排除名单里加上 TRAE，否则 QQ 一开通话/共享就会崩
```

上游 `docs/原理详解.md` 写明机制：Easy Effects 会把新出现的流移到自己的设备上，QQ 音频模块
收到「流被移动」后在采集线程里重新枚举设备，与主线程并发 → 竞态 → 崩溃。

## 键是哪个（照上游代码与文档，不猜）

排除名单在 `~/.config/easyeffects/db/easyeffectsrc` 的 `[StreamInputs]` / `[StreamOutputs]`
里，键是 **`blocklist=`（逗号分隔）**。上游代码特别指出：写成 `exclude=` 之类的键
Easy Effects **不读**，配了也不生效。

## 本次改动

`~/.config/easyeffects/db/easyeffectsrc`（备份 `easyeffectsrc.bak-1791088498`）：

```ini
[StreamInputs]
inputDevice=alsa_input.pci-0000_80_1f.3.analog-stereo
blocklist=TRAE            # ← 新增

[StreamOutputs]
outputDevice=alsa_output.pci-0000_80_1f.3.analog-stereo
blocklist=TRAE            # ← 新增
```

`TRAE` 是 QQ 音频流在 `wpctl status` 里显示的客户端名（实测确实是它）。

## 验证与生效

- 验证：`linuxqq-wayland-fix --doctor` → `✓ Easy Effects 已排除 QQ（TRAE）`（已实测）
- 生效：配置在 Easy Effects 启动时读入 —— 它当时**没在运行**，所以下次启动即生效；
  若它在运行，需要重载配置或重启它（没有替用户动进程）

## 崩溃记录（顺带看，未归因）

`~/.cache/linuxqq-wayland-fix/crash/` 里有 5 份 tomb（10-03 22:36/22:41/22:46/22:51、10-04 10:52）。
抽看两份：`SIGSEGV`，`rip` 落在极小地址（`0x15d0` / `nil`），堆栈只有 `ld-linux` 与 `libc`，
是**启动早期**的崩溃；tomb 里**没有** Easy Effects、也没有我们注入库的名字 ✗ —— 从这些文件
**无法归因**，只能说"不像 Easy Effects 那条流移动竞态"（那种崩在 `PulseAudioWrapper`）。
