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

## 7. 代码质量工具 [P2]

- 已配置 `.clang-format`（根目录），建议接入 CI 检查（`clang-format --dry-run --Werror`）；
- 评估 clang-tidy / cppcheck（MSVC 环境下可用 clang-tidy 对翻译单元分析）。