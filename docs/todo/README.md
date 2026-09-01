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

| 主题 | 关键项 |
|---|---|
| 跨平台 | Linux/macOS UI 嵌入（XEmbed/NSView）、imgui 套件非 Windows backend、插件 DLL 非 Windows 构建 |
| UI 适配器 | Qt 套件多插件共享（SHARED）、gtk/webview 新套件、Qt 套件在 Qt 宿主中的用法 |
| 核心框架 | C++ 包装层、事件/信号机制、插件热重载、官方 Rust/C# FFI 示例 |
| 构建打包 | 套件纳入 conan 包、CI、CMake presets 完善、版本号现代化 |
| 测试质量 | 单元测试框架、自动化宿主测试、内存/线程 sanitizer、示例矩阵 |