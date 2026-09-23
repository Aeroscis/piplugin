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

> **状态**：仍开放。**W-10 实测把范围钉死了**（Arch Linux，gcc 16.2.1 / clang 22.1.8）：
> 门不止 `tests/test_plugin` 一处 —— **每个**插件目标都在 `if(NOT WIN32)` 后面
> （`tests/test_plugin_*`、`examples/*plugin*`），包括纯 C 无 UI 的 service /
> badversion / guirequired / events 插件。因此 Linux 上目前**一个可加载的插件都没有**，
> W-10 的 Linux job 只能构建核心与宿主 kit，**无法**跑完整 headless 冒烟
> （`pi_test_host_headless` 需要一个 `argv[1]` 插件路径）。
> 另有两处同因阻塞（都不在 W-10 的文件边界内，故只记录不改）：
> `tests/common/pi_test_thread.h:130`、`tests/test_headless_host/pi_headless_host.c:313`
> 调用 `nanosleep()` 却没有给 glibc 平台层宏（`-std=c11` 下 gcc ≥ 14 直接以
> implicit declaration 报错）；以及 `tests/*/CMakeLists.txt` 里 `add_test` 的 COMMAND
> 一律硬编码 `.exe`，Linux 上 ctest 一律 "Unable to find executable"
> （实测 `unit_cpp` 在 Linux 上构建通过却 Not Run）。

**现状**：`tests/test_plugin/CMakeLists.txt` 显式 `if(NOT WIN32) … DISABLED`——
Linux/macOS 上插件测试目标被跳过；`pi_qt_main.cpp` / `pi_imgui_main.cpp` 用
`__declspec(dllexport)`（Windows 专用）。

**建议**：
1. 把 `extern "C" __declspec(dllexport)` 换成宏（复用 `PI_EXPORT` / `PI_PLUGIN_ENTRY_DECL`，
   该宏在非 Windows 下已是 `visibility("default")`）；
2. 移除/放宽 `if(NOT WIN32)` 门，让 Linux/macOS 也能产出 `.so` / `.dylib` 插件；
3. 顺带：测试侧的 `nanosleep` 补平台层宏（或改用项目自己的封装）、`add_test` 的
   `.exe` 后缀按平台取 `${CMAKE_EXECUTABLE_SUFFIX}` —— 这三件做完，W-10 的 Linux job
   才能把 headless 冒烟从「核心能编译」推进到「宿主能真的加载一个插件」。

## 4. 编译器矩阵验证 [P2] —— 已完成（W-10，Linux 侧）

> **状态**：Linux 侧已实测并进入 CI。本机（Arch Linux）实测：**gcc 16.2.1** 与
> **clang 22.1.8** 各自 configure + 构建核心（`piplugin`）、宿主 kit
> （`piplugin_host` / `piplugin_events`）与 `unit_cpp`，并**运行** C++ 层用例
> —— `checks=52 failures=0`、`RESULT: PASS`（gcc 与 clang 各一次）。
> CI 的 `linux` job（`.github/workflows/ci.yml`）复刻同一套开关，两种编译器各跑一轮。
> 首轮实测还暴露出三处平台分支编译错误，都在 `src/` 内就地修了：
> `dlsym` 的「对象指针 → 函数指针」转换（`--pedantic-errors` 下是错误）、
> `strdup`/`syscall` 缺 glibc 平台层宏、以及 `$<TARGET_PDB_FILE>` 这个 MSVC 专属
> 生成表达式在非 Windows 上让**生成阶段**直接失败（`src/piplugin/CMakeLists.txt`
> 与 `adapters/qt/CMakeLists.txt` 同因同解）。
> macOS 侧仍未验证（无机器）。

- 在 Linux 上验证 GCC/Clang 构建（`/W4 /permissive-` 等价物已在
  `cmake/pi/pi_project.cmake` 配置，但未实测）。
- macOS 上验证 Clang + `-Wall -Wextra -Wpedantic`。

## 5. `PiNativeWindow` Linux 定义确认 [P2] —— 已完成（W-10）

> **状态**：已用真实 Xlib 头文件验证，`unsigned long` 是对的，不需要改 `uintptr_t`。
> 实测（Arch Linux / x86_64，libx11 1.8.13）：用项目自己的编译选项
> （`gcc -std=c11 -Wall -Wextra --pedantic-errors`）编译一个同时包含
> `<X11/Xlib.h>` 与 `pi_plugin_types.h` 的探针，`Window` ⇄ `PiNativeWindow`
> 双向赋值通过，`sizeof(Window) == sizeof(XID) == sizeof(PiNativeWindow) == 8`，
> `PI_INVALID_WINDOW` / `PI_IS_VALID_WINDOW` 行为正确（探针退出码 0）。
> 32 位 Linux 上 `unsigned long` 与 XID 同为 4 字节，结论同样成立；
> 结论只对「尺寸与转换」负责，不涉及 FUT-01 的嵌入实现。

- Linux 下定义为 `unsigned long`；X11 上 `Window` 实际是 XID（`unsigned long` 通常兼容），
  建议在真实 Linux 构建中验证并视需要调整为 `uintptr_t` 或专门类型。