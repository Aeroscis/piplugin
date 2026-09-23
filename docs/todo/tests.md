# 测试与质量待办（Tests & Quality）

## 1. 单元测试框架 [P1] —— 已完成（roadmap BLK-06）

> **状态**：已落地。`tests/unit/`（纯 C，ctest 用例 `unit`，**210 项断言**）与
> `tests/unit_cpp/`（C++ RAII 层用例 `unit_cpp`，52 项 + `_CrtDumpMemoryLeaks()` 在 Debug
> 下按退出码断言无泄漏）；跨线程专项在 `tests/unit/pi_thread_tests.c`
> （ctest `unit_threads`，70 项）。覆盖超出原清单：GUID、descriptor 能力查询与
> properties 读法（含 0.3 前布局）、引用计数（含 destroy 回调）、
> `pi_module_load` 失败路径（含 W-01 的线程局部错误串与
> `pi_module_get_load_error_r`）、`pi_host_services_create_default` headless/GUI
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

## 3. 内存 / 线程卫生验证 [P1] —— 已完成（W-03）

> **状态**：sanitizer 跑道已落地，入口 `scripts/verify_asan.ps1`，CI 上是独立
> job `asan`（`.github/workflows/ci.yml`）。它把**现有的构建树就地**重配
> `/fsanitize=address`、跑非 GUI 的那部分 ctest、再把缓存变量改回去并重建
> （`-SkipRestore` 给 CI：跑完即弃的机器不需要还回去）。
>
> 为什么就地和为什么不用 CMakeLists 开关：本仓库把每个测试二进制 POST_BUILD
> 部署到 `<repo>/bin/<Config>`，另起一棵构建树会把别的构建的产物覆盖成插桩版，
> 而那棵树的 ctest 还以为跑的是自己的；就地重配则让「树的配置」与「bin 里的
> 二进制」始终一致。编译选项经 `-D` 注入缓存变量，不动根 `CMakeLists.txt` /
> `cmake/`（并行线文件边界）。
>
> **已验证**（本地实跑，日志 `build/verify-asan.log`）：非 GUI 用例 **9/9 全绿**，
> 含 `unit` / `unit_cpp` / `headless_host_*` / `descriptor_properties_*` /
> `capability_gate_*` / `app_defined_host_service_*` / `version_gate_*`；
> 反证：同一套开关编译一个故意越界的探针，ASan 报 `heap-buffer-overflow` 并以
> 退出码 1 结束——跑道是有牙齿的，不是「跑绿即无事」。
>
> **三个实测结论**：
> 1. **MSVC 的 ASan 在 Windows 上没有泄漏检测**——`detect_leaks=1` 会让运行时
>    直接以 "detect_leaks is not supported on this platform" 死在 `main()` 之前。
>    所以这条跑道覆盖**悬垂/越界/重复释放**，「无泄漏」那一半仍然由
>    `unit_cpp` 的 `_CrtDumpMemoryLeaks()` 断言承担（脚本里写明，不会静默假装
>    跑过泄漏检查）。
> 2. **GUI 必须排除——这不是洁癖**：本机整跑 ctest（含两个 example 宿主）时，
>    ASan 在一次窗口创建路径上从 `USER32` → `MSCTF` 抓到一个**第三方输入法
>    （SogouPY.ime）内部的 heap-use-after-free**，与本仓库代码无关。跑道跑的是
>    非 GUI 子集，正是为了不把这类报告算到我们头上。
> 3. `-NoParallel` 是给「沙箱禁用 MSBuild 多节点命名管道」的环境用的开关；
>    CI 不需要它。
>
> 另外两条读日志时该认得的东西：插桩构建的**链接期**会打印
> `LNK4044 无法识别的选项 "/fsanitize=address"`（它是编译期开关，链接器不认）与
> `LNK4300 忽略 "/INCREMENTAL"`（输入含 ASan 元数据），两条都是预期噪音，不是失败。
> 而 `detect_leaks=1` 那种启动即死**不打报告、也不像崩溃**：表现出来是整套用例
> 全报 `Required regular expression not found`、`0% tests passed`——看着像断言集体
> 失效，实际是运行时在 `main()` 之前就退了。
>
> **遗留（如实记录）**：本轮执行环境无外网，`asan` job 在 GitHub runner 上的
> 首次运行没有被观察到；本地证据是上面的日志与探针。job 依赖 runner 装有 VS 的
> ASan 组件，脚本会先找 `clang_rt.asan*_dynamic*.dll`，找不到就带着「装哪个
> 组件」的提示直接失败，而不是让一堆测试以「启动即 0xC0000135」收场。

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
| 多插件同进程 | ✅ 已覆盖（APP-08：`tests/test_host_multi` / ctest `multi_plugin_qt_in_one_process`，两个不同的 Qt 插件 DLL 同时加载、各自有 UI、各自跑定时器、一起卸载；W-05 补上 imgui 变体：ctest `multi_plugin_imgui_in_one_process`，两个不同的 **imgui** 插件模块各自渲染若干帧、各自心跳推进、一起干净卸载） |
| 嵌入窗口动态切换 | ✅ 已覆盖（W-02：`tests/test_host_multi --container-switch`，ctest `container_switch_runtime`（imgui 插件）/ `container_switch_runtime_qt`（Qt 插件）—— attach A → 切到 B → 切回 A → 尺寸往返 → 卸载，每步断言"插件窗口是**指定容器**的子窗口、可见、尺寸与容器客户区一致"，并断言 `pi_view_detach()` 后旧窗口确实已销毁） |

> **已知环境性退化（观察，非定论）**：在同一个长会话里反复跑 ctest 之后，imgui 那几个
> 用例会集体挂住——宿主日志显示心跳推进不了（`heartbeats=0/2`），
> `container_switch_runtime` / `multi_plugin_*` 以 Timeout 收场，且此后持续复现；
> 同时 `taskkill` 查不到任何残留进程，另起一棵树的副本跑 ctest 仍是 **21/21 全绿**。
> 所以它更像会话/环境问题而不是代码问题，但根因未定，这里只如实记录。
> **做法**：改动后**立刻**跑一次完整 `ctest` 作为证据，之后不要再靠"再跑一遍"举证
> ——再跑一遍很可能是在测环境而不是测代码；判定失败前先确认心跳在推进。

## 5. 线程安全专项 [P2] —— 已完成（W-04）

> **状态**：已落地（W-04）。三个跨线程场景各有用例，全部按退出码判定：
>
> | 场景 | 用例 | 断言要点 |
> |---|---|---|
> | 插件子线程调 `pi_host_post_message` | `unit_threads`（框架层，3 线程 × 200 条）+ `qt_view_post_from_worker_thread`（真插件子线程） | 宿主回调**真的在子线程上**被调到（否则场景没被覆盖到）、不丢不重、宿主自己排队后**在主线程上**恰好投递一次 |
> | Qt 套件 `pi_qt_view_post` 跨线程 marshal | `qt_view_post_from_worker_thread`（插件子线程调用） | 回调必须跑在**宿主 GUI 线程**上（`wparam==1` 的回报 + 回报本身也在主线程到达） |
> | 并发 AddRef/Release | `unit_threads`（4 线程；帮助函数与 vtbl 槽位各一遍） | 成对操作后计数回到基数且不得销毁；N 线程并发释放到零时 `destroy` **恰好一次**；只加不减的紧循环计数分毫不差 |
>
> 顺带修掉一个真 bug：`pi_qt_view_post()` 的文档写"任意线程可调、marshal 到
> Qt 线程"，实现却是**内联执行**（回调在调用线程上跑）—— 照文档写的插件会从
> 后台线程碰 QWidget。现在同线程内联、跨线程异步排队由宿主 `pi_on_idle()` 执行
> （不引入第二条线程、不阻塞），detach / 析构后未执行的调用被丢弃。改回内联时
> 新用例稳定失败（已做反证）。套件头文件 / `adapters/qt/README.md` /
> `interfaces.md` §6（那张表还写着"私有后台线程"）/ `tutorial/adapters.md` 已同步。
>
> 用例文件：`tests/unit/pi_thread_tests.c`（+ `tests/common/pi_test_thread.h`）、
> `tests/test_plugin/pi_qt_test_plugin.cpp`（探针由 `PI_QT_TEST_POST_THREAD=1`
> 打开，其余用例行为不变）、`tests/test_host_multi`（`--post-thread` 模式）。

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
- 小尾巴（**位置已变**：D3D 那段现在在 L1 kit 里，不在本测试的宿主文件里）：
  `host_kits/dx11/pi_host_dx11.cpp` 的 `frame_latency_waitable` 只创建 + 关闭、
  不再被等待（`SetMaximumFrameLatency(1)` 仍生效），下次清理可删。

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

## 8. 代码质量工具 [P2] —— 已完成（W-13；clang-format 的强制与否仍待维护者拍板）

> **状态**：
>
> **1. 文档-仓库漂移 lint 已落地并强制**：`scripts/verify.ps1` 第 5 项 + 规则表
> `scripts/doc_drift_rules.json`。规则是**数据不是代码**，且只在特征真的存在时才生效
> （`tests/unit/pi_unit_tests.c` 存在 ⇒ 任何"没有单元测试"的现行陈述非法），
> 所以这张表不会腐烂成对"已经变了的仓库"的断言。
>
> - 扫描范围 = `git ls-files '*.md'`（未跟踪的临时派工板天然不在内）；
>   `CHANGELOG.md` 显式排除——它讲的就是历史；
> - 判定口径：**已完成章节里的原文不算"现行陈述"**。todo 的惯例是"状态块 + 保留原文"，
>   所以标题带 已完成/已落地/已达成/已修复 的章节整段豁免（豁免沿标题层级向下继承），
>   个别行还可以用 `<!-- doc-drift: ignore -->` 逐行豁免；
> - 上下文过滤：规则可选要求陈述行**同时**含某关键词（现有唯一用处是 Linux 行），
>   免得误伤"下一行的 macOS 仍然成立"这种平台行；
> - 文档与规则都按 **UTF-8 显式读取**：两边都是中文，`Get-Content` 默认编码在
>   Windows PowerShell 5.1 下会把它们一起解错，于是永远匹配不上（实测踩过）。
>
> **验收实测**（在隔离副本里做，改完还原）：故意把已完成项改回「未做」（去掉标题里的
> 完成标记）→ lint 报
> `docs/todo/tests.md:13: says '没有单元测试', but tests/unit/pi_unit_tests.c exists`
> 并让 `verify.ps1` 以退出码 1 结束；改回后 **37 篇全绿、退出码 0**。
>
> **2. clang-format：仍然只报告、不拦截**，但第 4 项现在把**漂移清单逐条打印**
> （上限 40 条 + 余量计数），这就是"报告 vs 强制"的拍板材料：强制化的第一步是对这批文件
> 跑 `clang-format -i`，代价是一次覆盖全仓的格式化 diff——**没有擅自改为强制**。
>
> **3. clang-tidy / cppcheck 评估（只评估，结论如下）**：
> - **cppcheck：本机与 Linux 机器上都没装**，任何接入都要先加安装步骤，暂不值得。
> - **clang-tidy：本机（LLVM 19）与 Linux（LLVM 22）都有，但默认一个检查都不开**
>   ——LLVM ≥ 17 起默认检查集为空，裸跑 `clang-tidy <file> -- <flags>` 直接以
>   `Error: no checks enabled.` 退出（实测）。所以"接入 clang-tidy"的真身是**选检查集**，
>   那是维护者口径，不该由写脚本的人顺手替它拍。
> - **最小接入面（建议，未实施）**：挂在 **Linux job** 上最省事——(a) `compile_commands.json`
>   只有 Makefile / Ninja 生成器能导出（Windows 用的 VS 生成器不行），而 Linux job 正是
>   默认生成器；(b) ubuntu runner 自带 clang-tidy。加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
>   与一步 `clang-tidy -p build-<cc> <core sources>` 即可，范围先限核心 + 宿主 kit。
> - **实测数据**（LLVM 22，`--checks='-*,clang-diagnostic-*,bugprone-*,performance-*'`，
>   `src/*.c`）：**4 warning / 0 error**，全部落在 `src/pi_plugin_host.c`
>   ——`bugprone-multi-level-implicit-pointer-conversion` ×2、
>   `bugprone-easily-swappable-parameters` ×1、
>   `bugprone-reserved-identifier` ×1（命中的是 glibc 特性宏 `_DEFAULT_SOURCE`：这类
>   "实现保留名、但由用户定义"的宏必须先加进允许表，否则一开就是误报）。
>   结论：**值得接入，但先定检查集与允许表，再谈强制**。

- 已配置 `.clang-format`（根目录），建议接入 CI 检查（`clang-format --dry-run --Werror`）；
- 评估 clang-tidy / cppcheck（MSVC 环境下可用 clang-tidy 对翻译单元分析）。