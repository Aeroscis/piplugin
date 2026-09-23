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
| UI 适配器 | gtk/webview 新套件（FUT-08）、Qt 宿主内嵌 Qt 插件一等用法（W-06）、imgui 套件多宿主并发（W-05）、.rc 版本资源（W-07） |
| 核心框架 | 插件热重载（FUT-04）、`g_load_error` 线程安全（W-01）、插件发现试水（W-11）、.rc 版本资源（W-07） |
| 构建打包 | 仓库内置 CMakePresets（W-09）、cpack 产出（W-08）、Linux CI job（W-10）、~~`CMAKE_INSTALL_PREFIX` 未设置导致 INSTALL 失败~~（**已修复**：`bin/<Config>` 改由 POST_BUILD 维护，前缀固定到 `build/install`，见 `install-design-review-prompt.md`） |
| 测试质量 | ASan/sanitizer CI（W-03）、线程安全专项（W-04）、嵌入窗口运行时切换（W-02）、clang-tidy/cppcheck 评估（W-13）、~~imgui 宿主拖动缩放时 IMGUI 面板瞬时被缩放~~（**已修复**，见 `tests.md` 第 7 条；平台机制知识见 `design/d3d-window-resizing.md`） |