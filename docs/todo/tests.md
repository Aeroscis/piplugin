# 测试与质量待办（Tests & Quality）

## 1. 单元测试框架 [P1]

**现状**：没有单元测试；`tests/` 全是演示宿主/插件（集成式）。
核心纯 C 逻辑（GUID 比较、descriptor 能力查询、引用计数、模块加载）
非常适合加单元测试。

**建议**：
- 引入轻量 C 测试框架（如 `greatest.h` 单头文件 / `unity`）或 CTest 原生 `add_test`；
- 覆盖：`pi_guid_equal`、`pi_descriptor_find_capability/provides/requires`、
  `pi_refcounted_add_ref/release`（含 destroy 回调）、`pi_module_load` 失败路径、
  `pi_host_services_create_default` 的 headless/GUI 两种形态。

## 2. 宿主自动化验证脚本 [P1]

**现状**：三个测试宿主都写了 `pi_test_host.log` / `pi_qt_host.log` 供"自动化验证"，
但仓库里没有驱动它们的脚本。

**建议**：
- 提供 `scripts/run_host_tests.ps1` / `.sh`：
  1. `pi_test_host_headless.exe pi_test_plugin_qt.dll`（纯 console，断言退出码 0 + 输出关键字）；
  2. GUI 宿主以"自动加载 + 定时退出"模式跑（命令行传 DLL 路径，加 `--autoclose-ms` 选项），
     然后断言 log 内容；
- 接入 CI（见 `build.md` 第 2 条）。

## 3. 内存 / 线程卫生验证 [P1]

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
| imgui 宿主 + Qt 插件 | ✅ 可跑（`pi_test_host_imgui.exe pi_test_plugin_qt.dll`），未纳入自动化 |
| headless 宿主 + GUI 插件 | ✅ 已演示（插件无头运行、不建 UI） |
| headless 宿主 + service 插件 | ❌ 无示例（见 `framework.md` 第 1 条） |
| 多插件同进程 | ❌ 未覆盖（Qt 套件共享受限，见 `adapters.md` 第 1 条） |
| 嵌入窗口动态切换 | ❌ 未覆盖（`pi_host_default_set_ui_window` 运行时切换） |

## 5. 线程安全专项 [P2]

- `pi_host_post_message` 从插件子线程调用宿主的场景（测试插件目前只在 UI 回调里发消息）；
- Qt 套件 `pi_qt_view_post` 跨线程 marshal；
- 引用计数并发增减压力测试。

## 6. 负向测试 [P2]

- 加载不存在的 DLL → `pi_module_load` 返回 NULL，错误描述正确；
- 加载不含 `pi_plugin_entry` 的 DLL → 报 "does not export"；
- `pi_create_instance` 传未知 GUID → `PI_E_NOINTERFACE`；
- headless 宿主加载 `HOST_UI REQUIRED` 插件 → 能力门拒绝。

## 7. imgui 宿主拖边框时左侧 IMGUI 面板被瞬时缩放（未解决）[P2]

> 明确留给"更强的模型/后来者"：症状、已测数据、试过的方向、剩下的候选实验都在下面，
> 不用重新推导。相关代码：`tests/test_host/pi_imgui_test_host.cpp` 的
> `RunSizeLoop()` / `PrepareFrameFor()` / `RenderFrame()` / `CreateDeviceD3D()`；
> 相关提交：`e3b2c2a`（插件嵌入修复）、`993d99c`（自建缩放循环）。

**现象**：`pi_test_host_imgui.exe` + Qt 插件（或任何插件）时，**拖动窗口边框**缩放过程中，
左侧那块 IMGUI 绘制的 `Plugin Control` 面板会**横向缩窄一下（或拉伸一下）再弹回**，
幅度与拖动速度/步长成正比；右侧 Qt 插件区（子窗口）不缩放。
拖动**左边框**时几乎看不出（面板跟着窗口移动）；**不加载插件**时也看不出——因为整窗
统一缩放、屏幕上没有"未缩放的参照物"可对比。

**为什么可以断定不是 IMGUI 画错**（实测，图像像素测量）：

| 帧 | 插件子窗口左边缘 | IMGUI 面板宽度 | 面板→子窗口间隙 |
|---|---|---|---|
| 正常 | 824（图像px，1 图像px = 1/0.75 设备px） | **397 设备px**（代码写的是 400） | 10 设备px（=410-400，正常） |
| 异常 | **824（纹丝不动）** | **367 设备px** | 33 设备px |

面板少了 33px、右边多露出 33px 的宿主清屏色，而子窗口位置完全没动 → 这一帧是
**DWM 按"窗口新尺寸 / 后台缓冲区旧尺寸"把整帧缩放了**（文字纵向也同比压缩），
不是 IMGUI 把窗口画窄了（IMGUI 只能画 400）。

**测量方法（可复用）**：截图后用 System.Drawing 按行扫描——面板深色底 `[16,16,16]`
的右边界、插件区亮底 `[241,241,241]` 的左边界；用"子窗口左边缘 = 客户区 x=410"
做标定得到图像缩放比，再换算成设备px。

**时间线（每步都有实测支撑）**：

1. 宿主原来只在主循环出帧。拖动时系统跑自己的模态循环，主循环被卡在
   `DispatchMessage()` 里 → 全程不 Present → DWM 一直拉伸旧帧。
   **修**：`WM_ENTERSIZEMOVE/EXITSIZEMOVE` + 每步 `WM_SIZE` 出帧。→ 大幅改善，
   但有了新的"细微闪烁"。
2. 怀疑"窗口先变大、我们后 Present"的间隔。**修**：`WM_SIZE` 里先出帧（Present）
   再改容器/插件（把 Qt 同步重绘挪出关键路径）。
3. 怀疑旧帧来源是 `Present` 慢。**实测**：`present avg=6.44ms max=7.62ms`
   （≈ 一个刷新周期），`handler avg` 与它相同 → **Present 才是瓶颈**。
4. 怀疑 `WM_SIZING` 预渲染能把它降到微秒。**修**：`WM_SIZING` 里先把新尺寸的帧渲染好
   （不 Present），`WM_SIZE` 只 Present。**实测**：预渲染命中 609/609，
   但 `present avg` 仍 6.27ms → 预渲染有效、阻塞依旧。
5. 换 `CreateSwapChainForHwnd` + `FRAME_LATENCY_WAITABLE_OBJECT` +
   `SetMaximumFrameLatency(1)`，并在 `WM_SIZING` 里先等待 DWM。踩坑：
   **`ResizeBuffers` 必须传与创建时相同的 flags**，否则 `E_INVALIDARG`、缓冲区永不
   更新（症状会变成"面板一直缩放 + 卡顿"）。修好后 `present` 仍是 6.2~6.4ms。
   **关键对照实验**：从外部 `PostMessage(WM_ENTERSIZEMOVE)` + `SetWindowPos` 分步缩放
   （走同一条"等待→预渲染→Present"，但中间**没有**系统改窗口尺寸）→
   `present avg=0.00ms`。**结论：Present 是否阻塞取决于"它前面有没有发生窗口尺寸变化"。**
6. 因此改成**宿主自己接管缩放循环**（拦截 `WM_NCLBUTTONDOWN` 的
   `HTLEFT..HTBOTTOMRIGHT`），顺序为：
   `ResizeBuffers+渲染新尺寸(不Present) → Present → SetWindowPos(窗口→新尺寸) → 容器/插件`。
   逻辑上每一刻屏幕上的帧都与当时的窗口尺寸匹配。**但用户实测：仍然会闪。**

**剩下的候选方向**（按我的判断排序）：

1. **`DXGI_SCALING_NONE`**（唯一没做过的实验）：交换链始终保持在 ≥ 窗口的尺寸
   （例如监视器尺寸或只增不减），过期帧就只会被**裁切**而不是**缩放**，面板永不形变。
   **必须先确认** `CreateSwapChainForHwnd` + `FLIP_DISCARD` 是否允许该 flag
   （文档口径含糊；若返回 `DXGI_ERROR_INVALID_CALL` 则此路不通，退化为 2）。
2. 接受"一帧不一致"，但把它压到 ≤1 帧：目标是把
   `present` 的阻塞挪到窗口未变尺寸之前（本轮已做）**并**保证 DWM 在"系统应用新矩形的
   那一刻"不会先合成一帧——目前无法从用户态控制，可能需要
   `DwmSetWindowAttribute` / `DwmFlush` 之类手段，或改用 `CreateSwapChainForComposition`
   + DirectComposition 自己合成（复杂）。
3. 换个渲染架构：把左侧面板放进**自己的子窗口**（尺寸只在纵向变化时改），
   横向拖动时该子窗口的缓冲区根本不需要 resize，也就不会被缩放。
4. 承认现状：把它当作"Windows 上 D3D 窗口交互式缩放的固有限制"，在 README 里说明。

**当前代码状态（`993d99c` 之后）**：

- 拖动**缩放**由宿主自己的循环接管（见 `RunSizeLoop()`），因此**失去系统贴边吸附**；
  标题栏拖动（移动）仍交给系统，**仍有吸附/拖到顶部最大化**。
- 窗口有了 520x360 客户区最小尺寸。
- 每次拖动会在 `pi_test_host.log` 里记录一行：
  `resize: host size loop ended after N steps (failures=0) present avg=..ms max=..ms prepare avg=..ms`，
  用来判断路径是否按预期走（`failures` 应为 0；`present max` 是 6ms 级属正常且无害，
  因为此时窗口尺寸还没变）。
- 小尾巴：`g_frameLatencyWaitable` 现在只创建/关闭、不再被等待
  （`SetMaximumFrameLatency(1)` 仍生效），下次清理可删。

**验证工具（当时是临时脚本，已删除，需要时可重建）**：

- 几何探针：`EnumChildWindows` + `GetWindowRect`/`ScreenToClient` 断言
  "插件窗口 == 容器客户区 且 in-parent=(0,0)"（覆盖拖动以外的所有缩放路径，
  本轮与 `e3b2c2a` 都靠它回归）；
- 模拟拖动：`PostMessage(WM_ENTERSIZEMOVE)` → 多次 `SetWindowPos` → `PostMessage(WM_EXITSIZEMOVE)`；
- 模拟进入缩放循环：`PostMessage(WM_NCLBUTTONDOWN, HTRIGHT)`（左键未按下时循环会立即干净退出，
  可用于冒烟测试入口/退出路径）；
- 截图测量：上述"扫描面板/插件边界 + 用 410px 偏移标定"的方法；
- 读日志：宿主用 `fopen(...,"a")` 独占，进程活着时普通 `Get-Content` 会失败，
  用 `CreateFileW(..., share=7)` 原始读可绕过。

## 8. 代码质量工具 [P2]

- 已配置 `.clang-format`（根目录），建议接入 CI 检查（`clang-format --dry-run --Werror`）；
- 评估 clang-tidy / cppcheck（MSVC 环境下可用 clang-tidy 对翻译单元分析）。