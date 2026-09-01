# 跨平台待办（Platform）

## 1. Linux（X11）UI 嵌入 [P1]

**现状**：框架核心（`dlopen` 路径）与 `PiNativeWindow`（`unsigned long` X Window）已在
`pi_plugin_types.h` / `pi_plugin_host.c` 中就绪；但两个 UI 套件都只有 Windows 嵌入实现。

- `adapters/qt/pi_qt_view.cpp` `create_and_embed()`：`#else` 分支只有 `TODO: X11 XEmbed 嵌入`，
  控件创建后不嵌入宿主窗口。
- `adapters/imgui/pi_imgui_view.cpp`：整个 D3D11 路径是 Windows 专属。

**建议**：
1. Qt 套件实现 X11 XEmbed（`XReparentWindow`）或走 `QWidget::winId()` + 嵌入协议；
2. imgui 套件增加 X11 + OpenGL 3 backend（复用 imgui 官方 `imgui_impl_x11.cpp` /
   `imgui_impl_opengl3.cpp`，或在 `backends/` 下添加）。

## 2. macOS（NSView）UI 嵌入 [P1]

**现状**：`PiNativeWindow` 已定义为 `void*`（NSView），但无嵌入实现。

**建议**：
1. Qt 套件：`createNativeChildView`/`addSubview` 嵌入宿主容器；
2. imgui 套件：Metal 或 OpenGL backend（imgui 官方有 `imgui_impl_osx.mm` /
   `imgui_impl_metal.mm`）。

## 3. 插件 DLL 非 Windows 构建 [P1]

**现状**：`tests/test_plugin/CMakeLists.txt` 显式 `if(NOT WIN32) … DISABLED`——
Linux/macOS 上插件测试目标被跳过；`pi_qt_main.cpp` / `pi_imgui_main.cpp` 用
`__declspec(dllexport)`（Windows 专用）。

**建议**：
1. 把 `extern "C" __declspec(dllexport)` 换成宏（复用 `PI_EXPORT` / `PI_PLUGIN_ENTRY_DECL`，
   该宏在非 Windows 下已是 `visibility("default")`）；
2. 移除/放宽 `if(NOT WIN32)` 门，让 Linux/macOS 也能产出 `.so` / `.dylib` 插件。

## 4. 编译器矩阵验证 [P2]

- 在 Linux 上验证 GCC/Clang 构建（`/W4 /permissive-` 等价物已在
  `cmake/pi/pi_project.cmake` 配置，但未实测）。
- macOS 上验证 Clang + `-Wall -Wextra -Wpedantic`。

## 5. `PiNativeWindow` Linux 定义确认 [P2]

- Linux 下定义为 `unsigned long`；X11 上 `Window` 实际是 XID（`unsigned long` 通常兼容），
  建议在真实 Linux 构建中验证并视需要调整为 `uintptr_t` 或专门类型。