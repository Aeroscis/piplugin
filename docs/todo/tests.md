# 测试与质量待办（Tests & Quality）

> 本文件记**还没做**的事（标题不带完成标记的条目），以及已完成项的**结论 + 复核入口**。
> 已完成条目不再保留当时的方案原文——那是历史，在 `git log` 与 `CHANGELOG.md` 里；
> 通用机制知识在 `docs/design/`，验收分工总表在 `docs/design/conformance.md` §7。

## 1. 单元测试框架 [P1] —— 已完成（roadmap BLK-06）

> **结论**：`tests/unit/`（纯 C，ctest `unit`，**210 项断言**）、`tests/unit_cpp/`
> （C++ RAII 层，`unit_cpp`，52 项 + Debug 下 `_CrtDumpMemoryLeaks()` 无泄漏断言）、
> `tests/unit/pi_thread_tests.c`（跨线程专项，`unit_threads`，70 项）。覆盖核心纯 C
> 逻辑：GUID、descriptor 能力查询与 properties 读法（含 0.3 前布局）、
> 引用计数（含 destroy 回调）、`pi_plugin_module_load` 失败路径（含线程局部错误串与
> `pi_plugin_module_get_load_error_r`）、`pi_plugin_host_services_create_default` 的 headless/GUI
> 两形态、`pi_plugin_api_version_compatible` 边界、`pi_plugin_host_services_create_ex` 钩子契约。
> **复核**：`ctest -C Debug`；用例注册表见 `tests/CMakeLists.txt`。

## 2. 宿主自动化验证脚本 [P1] —— 已完成（roadmap ECO-07，演化形态）

> **结论**：**没有**按本条目原方案落地——`scripts/run_host_tests.ps1` 与
> `--autoclose-ms` 从未实现（这条记下来是为了避免有人再去找这两个东西）。实际分工：
> ctest 的 headless 冒烟（严格退出码断言）+ 一致性验收 `scripts/run_selftest.ps1`
> （`--cycles`，退出码裁决）+ 像素级缩放回归 `scripts/verify_resize_fix.ps1`。
> **复核**：`docs/design/conformance.md` §7 的验收分工总表；一条命令入口
> `scripts/verify.ps1`。

## 3. 内存 / 线程卫生验证 [P1] —— 已完成（W-03）

> **结论**：sanitizer 跑道 = `scripts/verify_asan.ps1`（CI job `asan`）：把**同一棵**
> 构建树就地重配 `/fsanitize=address`、跑非 GUI 的那部分 ctest、再把缓存变量改回去并
> 重建。为什么就地重配而不是另起构建树、为什么 GUI 必须排除、MSVC 的 ASan 上**没有**
> 泄漏检测（`detect_leaks=1` 启动即死）、以及读日志时该怎么看（`LNK4044` / `LNK4300`
> 是预期噪音；启动即死表现为每个用例都报 `Required regular expression not found`、
> `0% tests passed`，看着像断言集体失效）——全部写在**脚本的注释里**，那是它唯一的
> 维护点，本文件不再重复。
> **复核**：`scripts/verify_asan.ps1 -SkipRestore`；反证探针见脚本注释。

## 4. 多场景覆盖矩阵 [P2]

| 场景 | 现状 |
|---|---|
| imgui 宿主 + imgui 插件 | ✅ 已演示 |
| Qt 宿主 + imgui 插件 | ✅ 已演示 |
| imgui 宿主 + Qt 插件 | ✅ 可跑（`pi_test_host_imgui.exe pi_test_plugin_qt.dll`），未纳入自动化（已核实 `tests/test_host_multi` 是纯 Win32 宿主、无 D3D，APP-08 的用例不覆盖此格） |
| headless 宿主 + GUI 插件 | ✅ 已演示（插件无头运行、不建 UI） |
| headless 宿主 + service 插件 | ✅ 已有示例并纳入自动化（APP-07：`pi_test_plugin_service.dll` + ctest `headless_host_service_lifecycle`，断言 start/poll/status/stop 全生命周期） |
| 多插件同进程 | ✅ 已覆盖（APP-08：`tests/test_host_multi` / ctest `multi_plugin_qt_in_one_process`，两个不同的 Qt 插件 DLL 同时加载、各自有 UI、各自跑定时器、一起卸载；W-05 补上 imgui 变体：ctest `multi_plugin_imgui_in_one_process`，两个不同的 **imgui** 插件模块各自渲染若干帧、各自心跳推进、一起干净卸载） |
| 嵌入窗口动态切换 | ✅ 已覆盖（W-02：`tests/test_host_multi --container-switch`，ctest `container_switch_runtime`（imgui 插件）/ `container_switch_runtime_qt`（Qt 插件）—— attach A → 切到 B → 切回 A → 尺寸往返 → 卸载，每步断言"插件窗口是**指定容器**的子窗口、可见、尺寸与容器客户区一致"，并断言 `pi_plugin_view_detach()` 后旧窗口确实已销毁） |

> **已知环境性退化（观察，非定论）**：在同一个长会话里反复跑 ctest 之后，imgui 那几个
> 用例会集体挂住——宿主日志显示心跳推进不了（`heartbeats=0/2`），
> `container_switch_runtime` / `multi_plugin_*` 以 Timeout 收场，且此后持续复现；
> 同时 `taskkill` 查不到任何残留进程，另起一棵树的副本跑 ctest 仍是 **21/21 全绿**。
> 所以它更像会话/环境问题而不是代码问题，但根因未定，这里只如实记录。
> **做法**：改动后**立刻**跑一次完整 `ctest` 作为证据，之后不要再靠"再跑一遍"举证
> ——再跑一遍很可能是在测环境而不是测代码；判定失败前先确认心跳在推进。

## 5. 线程安全专项 [P2] —— 已完成（W-04）

> **结论**：三个跨线程场景各有用例、全部按退出码判定——插件子线程调
> `pi_plugin_host_post_message`（`unit_threads` 3 线程 × 200 条 + `qt_view_post_from_worker_thread`
> 真插件子线程）、Qt 套件 `pi_plugin_qt_view_post` 跨线程 marshal（回调必须跑在**宿主 GUI
> 线程**上）、并发 AddRef/Release（成对操作回到基数、并发释放到零时 `destroy`
> **恰好一次**）。顺带修掉一个真 bug：`pi_plugin_qt_view_post()` 文档写着"marshal 到 Qt 线程"，
> 实现却是**内联执行**，照文档写的插件会从后台线程碰 QWidget。
> **复核**：`ctest -C Debug`；线程契约见 `docs/design/interfaces.md` §6。

## 6. 负向测试 [P2] —— 已完成（roadmap ECO-08）

> **结论**：四类负向输入都有自动化断言，且都在 ctest 里——加载不存在的 DLL、加载不含
> `pi_plugin_entry` 的 DLL、`pi_plugin_create_instance` 传未知 class GUID（`PI_E_NOINTERFACE`
> + `*out` 为 NULL）、headless 宿主加载 `HOST_UI REQUIRED` 插件（ctest
> `capability_gate_rejects_gui_required_plugin`）。顺带修掉两个测试插件在
> `PI_E_NOINTERFACE` 路径上没把 `*out` 置 NULL 的问题（违反终审约定 2.4）。
> **复核**：`docs/design/conformance.md` 的覆盖表；用例在 `tests/unit` 与 ctest。

## 7. imgui 宿主拖边框时左侧 IMGUI 面板被瞬时缩放 [已修复，人工确认通过] [P2]

> **结论**：根因不是渲染逻辑、不是 Qt、也不是 Present 顺序，而是**帧尺寸与窗口尺寸
> 无法在同一时刻同时正确**（DWM 合成与宿主线程并发）；解法是把两者解耦——
> 交换链用 `DXGI_SCALING_NONE`（尺寸不等时 1:1 左上对齐**裁切**而非缩放），后备缓冲
> **只增不减**，使缩放比恒为 1，内容形状不可能改变。
> **复核**：`scripts/verify_resize_fix.ps1`（程序化 `SetWindowPos` + 截图 + 像素扫描 +
> 日志断言）；通用机制、两处竞态时刻表与排查清单见
> `docs/design/d3d-window-resizing.md`——本条只保留"本仓库这一例"的结论。
> "拖动过程中是否还有可感知闪烁"属**瞬态**观感，自动化覆盖不到，须人工确认。

## 8. 代码质量工具 [P2] —— 已完成（W-13；clang-format 的强制与否仍待维护者拍板）

> **结论**：
>
> 1. **文档-仓库漂移 lint 已落地并强制**：`scripts/verify.ps1` 第 5 项 + 规则表
>    `scripts/doc_drift_rules.json`。规则是**数据不是代码**，且只在特征真的存在时才
>    生效（`tests/unit/pi_unit_tests.c` 存在 ⇒ 任何否认它有单测的现行陈述非法），
>    所以这张表不会腐烂成对"已经变了的仓库"的断言。扫描范围 = `git ls-files '*.md'`
>    （未跟踪的临时派工件天然不在内），`CHANGELOG.md` 显式排除——它讲的就是历史。
>    判定口径、上下文过滤与逐行豁免见规则表的 `_comment`。
> 2. **clang-format：仍然只报告、不拦截**，但第 4 项会把**漂移清单逐条打印**，这就是
>    "报告 vs 强制"的拍板材料：强制化的第一步是对这批文件跑 `clang-format -i`，代价是
>    一次覆盖全仓的格式化 diff —— **待维护者拍板**。
> 3. **clang-tidy / cppcheck 只评估、未接入**：cppcheck 本机与 Linux 机器上都没装；
>    clang-tidy 两处都有，但 **LLVM ≥ 17 起默认检查集为空**，裸跑直接以
>    `Error: no checks enabled.` 退出——所以"接入 clang-tidy"的真身是**选检查集**，
>    那是维护者口径，不该由写脚本的人顺手替它拍。
>    **最小接入面（建议，未实施）**：挂在 **Linux job** 上最省事——`compile_commands.json`
>    只有 Makefile / Ninja 生成器能导出（Windows 用的 VS 生成器不行），而 Linux job 正是
>    默认生成器，且 ubuntu runner 自带 clang-tidy。加
>    `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 与一步 `clang-tidy -p build-<cc> <core sources>`
>    即可，范围先限核心 + 宿主 kit。实测（LLVM 22，
>    `--checks='-*,clang-diagnostic-*,bugprone-*,performance-*'`，`src/*.c`）：
>    **4 warning / 0 error**，全部落在 `src/pi_plugin_host.c`，其中一条是 glibc 特性宏
>    `_DEFAULT_SOURCE` 命中 `bugprone-reserved-identifier`——这类"实现保留名、但由用户
>    定义"的宏必须先加进允许表，否则一开就是误报。结论：**值得接入，但先定检查集与
>    允许表，再谈强制**。
>
> **复核**：`scripts/verify.ps1`（第 4、5 项）。漂移 lint 的"有牙齿"验收：故意把某个
> 已完成项改回「未做」（去掉标题里的完成标记），lint 会指名报出该行与它违反的特征、
> 并让 `verify.ps1` 以退出码 1 结束；原样输出见 `CHANGELOG.md`。
