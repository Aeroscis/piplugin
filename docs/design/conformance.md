# 一致性验收（Conformance Harness）

> 对应 roadmap 的 **ECO-02**。本文说明如何用**官方宿主**验收任意插件 DLL / 社区适配器，
> 以及"跑过 cycles"在生态里的含义。

## 1. 是什么

插件生命周期里最容易出事的地方（顺序错一次就是崩溃）都在**宿主侧**：
加载、门禁、实例化、attach、idle 驱动、尺寸转发、以及七步卸载序列。
因此"插件能被加载"这句话不足以保证它可用——必须在真实宿主里跑完整轮回。

harness 就是官方 imgui 测试宿主的 `--cycles` 模式：对**你给的任意插件 DLL 列表**
反复跑真实生命周期，并由**退出码**给出裁决。

## 2. 怎么跑

推荐用脚本（自带判定，失败会给出非零退出码）：

```powershell
# 默认：两个官方插件各 3 轮
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_selftest.ps1

# 验收你自己的插件（可给多个，逗号分隔；支持绝对路径）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_selftest.ps1 `
    -Plugin "my_plugin.dll,C:\path\to\other_plugin.dll" -Cycles 5 -IdleFrames 20
```

或者直接调宿主（日志写在 exe 同目录的 `pi_test_host.log`）：

```
pi_test_host_imgui.exe --cycles 3 --plugin my_plugin.dll --idle-frames 12
```

| 参数 | 含义 |
|---|---|
| `--cycles N` | 每个插件跑 N 轮（`0`/缺省 = 不进入验收模式） |
| `--plugin a.dll[,b.dll,...]` | 被测插件列表；绝对路径原样使用，否则按 exe 目录解析 |
| `--idle-frames N` | 每轮里让插件视图存活多少渲染帧（默认 20） |
| `--skip-detach` | 诊断用：卸载时跳过 `pi_view_detach()`（复现"宿主直接丢模块"路径） |

## 3. 一轮里做了什么

```
load（模块 + 双向能力门禁 + 实例化 + 初始化）
  -> attach（嵌进宿主容器）
  -> idle 若干帧（真正跑插件的每帧逻辑）
  -> 拉伸宿主窗口到 1.25 倍（走完整 WM_SIZE -> 交换链策略 -> 插件 resize 转发）
  -> 恢复原尺寸
  -> detach + release view
  -> terminate + release plugin
  -> unload module
```

全程由宿主 kit（L0 会话 + L1 嵌入胶水）执行，所以这同时也是对 kit 自身顺序的回归。

## 4. 退出码与判定

| 退出码 | 含义 |
|---|---|
| `0` | 列表里**每个**插件的**每一轮**都完整跑完并通过 |
| `2` | 出现失败（插件没发布 view、加载失败、门禁拒绝等）；日志里有 `selftest: FAIL` 行 |

日志里的判定行：

- 通过：`selftest: PASS (N plugin(s) x M cycles)`
- 失败：`selftest: FAIL - no view after load` + `selftest: FAIL - aborted at plugin i/N`
- 结束：`exit: plugins=N cycles/plugin=M failed=0 aborted=0 done=1`

`scripts/run_selftest.ps1` 还会额外要求日志中出现 `selftest: PASS` ——
防止"进程退出码是 0 但用例根本没跑完"这种情况被当成通过。

## 5. 生态含义：跑过 cycles = 进生态列表

**约定**：社区交付的适配器或插件，只要能在官方宿主上跑过 cycles（退出码 0），
就可以列入 README 的生态列表——这是"可用"的最低客观门槛，不需要维护者手工审查
每一行代码。

> 生态列表本身要等 README 就位（roadmap BLK-02）；本约定先行生效。

**写套件的人先读** [`adapter-spec.md`](adapter-spec.md)：它把适配器与框架/宿主之间的
契约（生命周期、线程模型、shutdown 契约、trace 约定）写成可照做的条款，并附一份
交付前自查清单。`examples/minimal_kit_win32/` 就是照那份规范写出来的最小套件
（~250 行纯 C，零工具包），它通过本 harness 的 3 轮验收 —— 也就是说"照 spec 写出的
套件可用"这件事本身是被验证过的，不是声称的。

## 6. 覆盖与不覆盖

**覆盖**：

- 插件能在真实宿主里被加载、实例化、初始化；
- 插件声明的能力与宿主提供的服务**双向**匹配（门禁在实例化前生效）；
- 视图能 attach、被每帧驱动、接收尺寸变化、detach；
- 卸载序列（含插件自身 teardown）不会崩、不会卡死；
- 多轮重复执行（暴露"第二次加载才崩"这类状态残留问题）。

**不覆盖**（各有归属工作项）：

| 不覆盖 | 归属 |
|---|---|
| 负向输入（不存在的 DLL、无 entry 的 DLL、未知 class GUID、headless 加载 GUI 插件） | ECO-08 —— **已落地**：前三条在 `tests/unit`，第四条是 ctest `capability_gate_rejects_gui_required_plugin`（`tests/test_plugin_guirequired`） |
| 画面内容是否正确（只看"有没有崩、有没有 view"） | 需人工/截图断言，见 `scripts/verify_resize_fix.ps1` |
| 多个 Qt 插件同进程 | **已覆盖**（APP-08）：ctest `multi_plugin_qt_in_one_process` |
| 多个 imgui 插件同进程 | **已覆盖**（W-05）：ctest `multi_plugin_imgui_in_one_process`（两个不同的 imgui 插件模块各自渲染若干帧、各自心跳推进、一起干净卸载） |
| 嵌入窗口运行时切换（容器 A→B→A + 尺寸往返） | **已覆盖**（W-02）：ctest `container_switch_runtime` / `container_switch_runtime_qt` |
| 跨线程（插件子线程 → 宿主 / 套件 post / 并发引用计数） | **已覆盖**（W-04）：ctest `unit_threads`、`qt_view_post_from_worker_thread` |
| Linux / macOS | FUT-01 / FUT-02 |
| 崩溃隔离（插件崩了宿主也崩） | FUT-05（跨进程插件） |

## 7. 现有验证入口的分工

| 入口 | 管什么 |
|---|---|
| `scripts/verify.ps1` | **一条命令跑完全部检查**（CI 调用的就是它）：下面三项按顺序跑，退出码裁决 |
| `ctest -C Debug` | 核心单元测试 + headless 冒烟 + 版本门禁负向用例；非 GUI，最快（见 quickstart 4.1） |
| `scripts/run_selftest.ps1` | **本 harness**：生命周期 + 尺寸往返，多插件，退出码裁决 |
| `scripts/verify_resize_fix.ps1` | 缩放修复的**像素级**回归：截图量测面板/插件边缘是否恒定 |
| `scripts/drag_measure.ps1` | 交互拖拽路径的耗时/失败计数诊断 |

CI 见 `.github/workflows/ci.yml`：它只做"装依赖 + 构建"，然后调用 `scripts/verify.ps1`
（检查项留在仓库脚本里，本地可复现）。harness 覆盖哪些插件由 `scripts/verify.ps1`
从 `bin/<CONFIG>` 里自动发现 —— 所以 CI 上（Qt 关闭）只测 imgui 插件，本机则两个都测。
