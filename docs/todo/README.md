# 待办清单总览

> 本文件夹按主题拆分待办：`platform.md`（跨平台）、`adapters.md`（UI 适配器）、
> `framework.md`（核心框架）、`build.md`（构建/打包）、`tests.md`（测试与质量）。
> 来自代码注释中已标注的 TODO 与架构推演出的下一步工作。

## 优先级图例

| 标记 | 含义 |
|---|---|
| [P0] | 高优先级：影响正确性/安全/核心承诺 |
| [P1] | 中优先级：明确的功能缺口 |
| [P2] | 低优先级：打磨/增强 |

## 快速一览

> 已在 release-roadmap（BLK/APP/ECO 系列）落地的工作不再列在这里；下表的
> `W-xx` 指下一波改进（卡片与并行线划分见 `docs/todo/parallel-improvements.md`
> 派工板），`FUT-xx` 指 release-roadmap 的未来方向。

| 主题 | 关键项 |
|---|---|
| 跨平台 | Linux/macOS UI 嵌入（XEmbed/NSView）、imgui 套件非 Windows backend、插件 DLL 非 Windows 构建、Linux headless CI（W-10） |
| UI 适配器 | gtk/webview 新套件（FUT-08）、~~Qt 宿主内嵌 Qt 插件一等用法~~（**已完成 W-06**：`tutorial/qt-host-direct.md` + `examples/qt_host_direct/`，含宿主直连 Qt 插件的 `--self-test`）、~~imgui 套件多宿主并发~~（**已完成 W-05**：`pi_test_plugin_imgui2.dll` + ctest `multi_plugin_imgui_in_one_process`——两个不同 imgui 插件模块各自渲染、心跳推进、一起干净卸载）、.rc 版本资源（W-07） |
| Qt 版本 | Qt 5.15 EOL 去向（FUT-09）：**评估已出（W-12）** —— 0.4.x/1.0 保持单版本 5.15.2、双版本套件否决、迁移走「一次性切 Qt6」且前置是 Qt6 编译跑道；结论与重估触发条件见 `design/qt6-assessment.md`，拍板仍在维护者 |
| 核心框架 | 插件热重载（FUT-04）、~~`g_load_error` 线程安全~~（**已完成 W-01**：错误串改线程局部 + 新增 `pi_module_get_load_error_r(buf,size)`，ctest `unit` 有 4 线程并发回归）、~~插件发现试水~~（**已完成 W-11**：`examples/plugin_scan/` 扫目录 + descriptor 清单，版本选择为占位）、.rc 版本资源（W-07） |
| 构建打包 | 仓库内置 CMakePresets（W-09）、cpack 产出（W-08）、Linux CI job（W-10）、~~`CMAKE_INSTALL_PREFIX` 未设置导致 INSTALL 失败~~（**已修复**：`bin/<Config>` 改由 POST_BUILD 维护，前缀固定到 `build/install`，见 `install-design-review-prompt.md`） |
| 测试质量 | ASan/sanitizer CI（W-03）、~~线程安全专项~~（**已完成 W-04**：ctest `unit_threads` 覆盖子线程 post_message 与并发引用计数，ctest `qt_view_post_from_worker_thread` 覆盖套件跨线程 marshal——并修掉 `pi_qt_view_post` 内联执行的 bug）、~~嵌入窗口运行时切换~~（**已完成 W-02**：ctest `container_switch_runtime` / `container_switch_runtime_qt`，A→B→A + 尺寸往返 + detach 同步性）、clang-tidy/cppcheck 评估（W-13）、~~imgui 宿主拖动缩放时 IMGUI 面板瞬时被缩放~~（**已修复**，见 `tests.md` 第 7 条；平台机制知识见 `design/d3d-window-resizing.md`） |