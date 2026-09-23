# 测试与质量待办（Tests & Quality）

## 1. 单元测试框架 [P1] —— 已完成（roadmap BLK-06）

> **状态**：已落地。`tests/unit/`（纯 C，ctest 用例 `unit`，109 项断言）与
> `tests/unit_cpp/`（C++ RAII 层用例 `unit_cpp`，`_CrtDumpMemoryLeaks()` 在 Debug
> 下按退出码断言无泄漏）。覆盖超出原清单：GUID、descriptor 能力查询与
> properties 读法（含 0.3 前布局）、引用计数（含 destroy 回调）、
> `pi_module_load` 失败路径、`pi_host_services_create_default` headless/GUI
> 两形态、`pi_api_version_compatible` 边界、`pi_host_services_create_ex`
> 钩子契约；注册表见 `tests/CMakeLists.txt`。

**现状**：没有单元测试；`tests/` 全是演示宿主/插件（集成式）。
核心纯 C 逻辑（GUID 比较、descriptor 能力查询、引用计数、模块加载）
非常适合加单元测试。

**建议**：
- 引入轻量 C 测试框架（如 `greatest.h` 单头文件 / `unity`）或 CTest 原生 `add_test`；
- 覆盖：`pi_guid_equal`、`pi_descriptor_find_capability/provides/requires`、
  `pi_refcounted_add_ref/release`（含 destroy 回调）、`pi_module_load` 失败路径、
  `pi_host_services_create_default` 的 headless/GUI 两种形态。

## 2. 宿主自动化验证脚本 [P1] —— 已完成（roadmap ECO-07，演化形态）

> **状态**：自动化已覆盖，但**没有**按本文原方案落地——`scripts/run_host_tests.ps1`
> 与 `--autoclose-ms` 从未实现（roadmap ECO-07 的演化形态）。实际分工：
> ctest 的 headless 冒烟（BLK-06，严格退出码断言）+ 一致性验收
> `scripts/run_selftest.ps1`（`--cycles`，退出码裁决）+ 像素级缩放回归
> `scripts/verify_resize_fix.ps1`；分工总表见 `docs/design/conformance.md` §7，
> 一条命令入口是 `scripts/verify.ps1`。

**现状**：三个测试宿主都写了 `pi_test_host.log` / `pi_qt_host.log` 供"自动化验证"，
但仓库里没有驱动它们的脚本。

**建议**：
- 提供 `scripts/run_host_tests.ps1` / `.sh`：
  1. `pi_test_host_headless.exe pi_test_plugin_qt.dll`（纯 console，断言退出码 0 + 输出关键字）；
  2. GUI 宿主以"自动加载 + 定时退出"模式跑（命令行传 DLL 路径，加 `--autoclose-ms` 选项），
     然后断言 log 内容；
- 接入 CI（见 `build.md` 第 2 条）。

## 3. 内存 / 线程卫生验证 [P1] —— 已上收（W-03）

> **状态**：ASan/sanitizer 跑道仍未建立（roadmap §8 原计划「随 BLK-05 加」，落地时
> 静默丢掉）。已上收为下一波工作 **W-03**（`docs/todo/parallel-improvements.md`
> 派工板，CI/脚本线）：MSVC `/fsanitize=address` 先只跑 unit + headless，
> 避开 DWM/GUI 噪音。

- 插件卸载顺序（view → plugin → factory → module → host）是最容易出错的地方；
  现有 unwrap 顺序在代码里手工维护。建议：
  - 宿主/插件跑一遍 **Dr. Memory / Valgrind / ASan**（MSVC 可用
    `/fsanitize=address` 实验）验证无泄漏/悬垂；
  - 特别验证 Qt 套件 `finalize()` 的 5s 超时泄漏路径（`tryAcquire` 超时分支）
    与 imgui 套件 `destroy_resources` 在未 attach 时调用的安全性。

## 4. 多场景覆盖矩阵 [P2]

| 场景 | 现状 |
|---|---|
| imgui 宿主 + imgui 插件 | ✅ 已演示 |
| Qt 宿主 + imgui 插件 | ✅ 已演示 |
| imgui 宿主 + Qt 插件 | ✅ 可跑（`pi_test_host_imgui.exe pi_test_plugin_qt.dll`），未纳入自动化（已核实 `tests/test_host_multi` 是纯 Win32 宿主、无 D3D，APP-08 的用例不覆盖此格） |
| headless 宿主 + GUI 插件 | ✅ 已演示（插件无头运行、不建 UI） |
| headless 宿主 + service 插件 | ✅ 已有示例并纳入自动化（APP-07：`pi_test_plugin_service.dll` + ctest `headless_host_service_lifecycle`，断言 start/poll/status/stop 全生命周期） |
| 多插件同进程 | ✅ 已覆盖（APP-08：`tests/test_host_multi` / ctest `multi_plugin_qt_in_one_process`，两个不同的 Qt 插件 DLL 同时加载、各自有 UI、各自跑定时器、一起卸载） |
| 嵌入窗口动态切换 | ❌ 未覆盖（`pi_host_default_set_ui_window` 运行时切换）——已上收 **W-02**（派工板，测试线） |

## 5. 线程安全专项 [P2] —— 已上收（W-04）

> **状态**：仍开放。已上收为下一波工作 **W-04**（派工板，测试线第 2 位）：
> 下述三项全部做成 ctest 注册的压测用例。

- `pi_host_post_message` 从插件子线程调用宿主的场景（测试插件目前只在 UI 回调里发消息）；
- Qt 套件 `pi_qt_view_post` 跨线程 marshal；
- 引用计数并发增减压力测试。

## 6. 负向测试 [P2] —— 已完成（roadmap ECO-08）

> **状态**：四类负向输入都有自动化断言，且都在 `ctest` 里：
>
> | 负向输入 | 断言位置 |
> |---|---|
> | 加载不存在的 DLL | `tests/unit`（`pi_module_load` 返回 NULL + 错误描述含路径） |
> | 加载不含 `pi_plugin_entry` 的 DLL | `tests/unit`（拿单测自身当模块，错误描述含 `pi_plugin_entry`） |
> | `pi_create_instance` 传未知 class GUID | `tests/unit`（`PI_E_NOINTERFACE` + `*out` 为 NULL + 非法参数 + **正向控制**） |
> | headless 宿主加载 `HOST_UI REQUIRED` 插件 | ctest `capability_gate_rejects_gui_required_plugin`（`tests/test_plugin_guirequired`，断言拒绝理由） |
>
> 顺带成果：写这组用例时抓到两个测试插件在 `PI_E_NOINTERFACE` 路径上**没有把 `*out`
> 置 NULL**，违反终审约定 2.4 —— 已修。

原来的建议（保留作为检查项清单）：

- 加载不存在的 DLL → `pi_module_load` 返回 NULL，错误描述正确；
- 加载不含 `pi_plugin_entry` 的 DLL → 报 "does not export"；
- `pi_create_instance` 传未知 GUID → `PI_E_NOINTERFACE`；
- headless 宿主加载 `HOST_UI REQUIRED` 插件 → 能力门拒绝。

## 7. imgui 宿主拖边框时左侧 IMGUI 面板被瞬时缩放 [已修复，人工确认通过] [P2]

> **状态**：已修复并经人工快速拖动确认。根因与通用解法已抽为独立知识文档
> **`docs/design/d3d-window-resizing.md`**（Windows D3D flip 模型窗口交互式缩放），
> 本条只保留「本仓库这一例」的结论与验证方式，避免两处重复维护。

**一句话根因**：不是渲染逻辑、不是 Qt、也不是 Present 顺序，而是
**帧尺寸与窗口尺寸无法在同一时刻同时正确**——DWM 合成与应用并发，
总有某一刻 DWM 手上的帧尺寸与当时窗口尺寸不一致，于是帧被**缩放**。
（此前几轮攻「消除 Present 阻塞」是在攻错误目标：Present 返回 ≠ DWM 取帧。）

**一句话解法**：把帧尺寸与窗口尺寸**解耦**——
交换链改用 `DXGI_SCALING_NONE`（尺寸不等时 1:1 左上对齐**裁切**而非缩放），
后备缓冲**只增不减**，使缓冲恒 >= 窗口 ⇒ 缩放比恒为 1 ⇒ 内容形状不可能改变。

**本仓库的改动**（`tests/test_host/pi_imgui_test_host.cpp`）：

| 改动 | 位置 |
|---|---|
| `sd.Scaling = DXGI_SCALING_NONE`（失败则降级回 STRETCH 并记日志） | `CreateDeviceD3D()` |
| `SetBackgroundColor()` = 清屏色，兜住「窗口>缓冲」那一帧 | `CreateDeviceD3D()` |
| 新增 `EnsureSwapChainSizeAtLeast()`：缓冲只增不减 | 供 `PrepareFrameFor()` / `WM_SIZE` 共用 |
| 拖动步骤不再逐步 `ResizeBuffers`，布局由覆盖 `io.DisplaySize` 驱动 | `PrepareFrameFor()` |
| 非拖动的 `WM_SIZE`（最大化/程序化缩放）同样走只增不减 | `WndProc` |

**验证**：

- 自动回归 `scripts/verify_resize_fix.ps1`（程序化分步 `SetWindowPos` +
  `PrintWindow` 截图 + 像素扫描 + 日志断言）。
  关键场景：窗口缩到 520x274 而缓冲仍为 1257x663（**2.4 倍**）时，
  面板右边界恒为 **404px**、插件左边界恒为 **414px**、间隙恒为 **10px**
  ——若 DWM 仍在缩放帧，面板会缩到约 167px。
- 插件升降载回归 `scripts/run_selftest.ps1`（`--cycles 3`，`PASS (3 cycles)`、`failed=0`）。
- 人工快速拖动：**面板形变消失，确认通过**。

**当前代码状态**：

- 拖动缩放由宿主自己的循环接管（`RunSizeLoop()`），因此**失去系统贴边吸附**；
  标题栏拖动（移动）仍交给系统，**仍有吸附/拖到顶部最大化**。
- 窗口有 520x360 客户区最小尺寸。
- 日志：`d3d: swap chain ... scale:none buffer=WxH`（创建时）、
  `resize: swap chain grows to WxH`（按需增长时）。
- 小尾巴：`g_frameLatencyWaitable` 现在只创建/关闭、不再被等待
  （`SetMaximumFrameLatency(1)` 仍生效），下次清理可删。

**验证工具**：

- 几何探针：`EnumChildWindows` + `GetWindowRect`/`ScreenToClient` 断言
  「插件窗口 == 容器客户区 且 in-parent=(0,0)」（覆盖拖动以外的所有缩放路径）；
- 模拟拖动：`PostMessage(WM_ENTERSIZEMOVE)` → 多次 `SetWindowPos` → `PostMessage(WM_EXITSIZEMOVE)`；
- 模拟进入缩放循环：`PostMessage(WM_NCLBUTTONDOWN, HTRIGHT)`（左键未按下时循环会立即干净退出，
  可用于冒烟测试入口/退出路径）；
- 截图测量 + 扫描面板/插件边界、按客户区左偏移标定：已固化为
  `scripts/verify_resize_fix.ps1`；
- 读日志：宿主用 `fopen(...,"a")` 独占，进程活着时 `Get-Content` 会失败，
  用 `[System.IO.File]::Open(path, 'Open','Read','ReadWrite')` 可绕过
  （脚本内已实现，含重试）。

## 8. 代码质量工具 [P2] —— clang-format 部分落地；其余已上收（W-13）

> **状态**：clang-format 已接入 `scripts/verify.ps1` 第 4 项——**只报告漂移、
> 不拦截**（这是对 roadmap BLK-05 原定 `--dry-run --Werror` 强制检查的有意偏离，
> 脚本注释有说明；是否升级为强制是待维护者拍板项，见派工板）。clang-tidy /
> cppcheck 评估已上收为 **W-13**（派工板，CI/脚本线，含「文档-仓库 drift」lint）。

- 已配置 `.clang-format`（根目录），建议接入 CI 检查（`clang-format --dry-run --Werror`）；
- 评估 clang-tidy / cppcheck（MSVC 环境下可用 clang-tidy 对翻译单元分析）。