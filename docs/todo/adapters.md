# UI 适配器待办（Adapters）

> 本文件记**还没做**的事（标题不带完成标记的条目），以及已完成项的**结论 + 复核入口**。
> 已完成条目不再保留当时的方案原文——那是历史，在 `git log` 与 `CHANGELOG.md` 里；
> 套件的机制与契约在 `docs/design/adapter-spec.md`。

## 1. Qt 套件改为 SHARED 并共享运行时 [P1] —— 已完成（roadmap APP-08）

> **结论**：`piplugin_qt` 现在是 **SHARED** 库（导出宏 `PI_QT_API`，部署到
> `bin/<CONFIG>/`），进程级状态（唯一 `QApplication`、活视图登记表）全进程一份，因此
> 同一进程可以同时加载多个 Qt 插件 DLL。配套：`pi_qt_view_shutdown_owner(owner)` 只拆
> 本插件的视图（不带 owner 的 `pi_qt_view_shutdown()` 保留为进程级大锤）；
> `QApplication` 由最后一个销毁的视图析构。改回 STATIC 时第二个 attach 会撞上 Qt 的
> `"there should be only one application object"` 断言。
> **复核**：ctest `multi_plugin_qt_in_one_process`（`tests/test_host_multi`）；判定口诀与
> STATIC/SHARED 取舍见 `docs/design/adapter-spec.md` §8。

## 2. 新增 gtk 套件（piplugin_gtk）[P2]

插件用 GTK 写 UI 时，按 `qt/` 套件同样的模式实现：私有线程跑 GTK 主循环 +
X11 嵌入。README（`adapters/qt/README.md`）已预留此方向。

## 3. 新增 webview 套件（piplugin_webview）[P2]

插件 UI 用 HTML/JS（WebView2 / WebKitGTK / WKWebView）时，封装成同一模式。

## 4. Qt 宿主内嵌 Qt 插件的一等用法 [P2] —— 已完成（W-06）

> **结论**：这类组合**不可用**（而不是"能用但不完美"）——套件的 `attach()` 在
> `piqt_app_create()` 就失败，因为进程里已存在宿主的 `QApplication`，套件的
> `PI_QT_VIEW_TRACE=1` 表现为日志只到 `attach: enter` 一行。正确姿势是宿主直连插件的
> `QWidget*`（插件不链接 `piplugin_qt`），并用能力门禁把走错路的插件在实例化**之前**
> 挡下（`pi_host_session_require()` → `plugin does not provide iid ...`）。
> **复核**：教程 `docs/tutorial/qt-host-direct.md`（症状与判定、宿主×插件选型表、
> 四条硬规则、常见错误对照表）；可运行示例 `examples/qt_host_direct/`，其
> `--self-test` 程序化点击插件按钮 → 断言消息到达宿主 → 按顺序卸载 → 退出码判定。

## 5. imgui 套件非 Windows backend [P1]

见 `platform.md` 第 1/2 条：imgui 套件目前只有 D3D11（Windows），
需 OpenGL/Vulkan/Metal backend 才能在 Linux/macOS 使用。

## 6. imgui 套件多宿主并发 [P2] —— 已完成（W-05）

> **结论**：同进程同时加载**两个不同的 imgui 插件模块**（`pi_test_plugin_imgui2.dll`，
> 与 A 只差 class GUID / 显示名 / 心跳码 `0x2002` vs `0x2003`），各自嵌进自己的容器、
> 由同一个宿主循环驱动渲染若干帧，然后一起干净卸载。断言的是并发本身而不是"没崩"：
> 两个窗口都在**自己的**容器里且可见、两个插件的心跳都必须推进（"画了一帧就冻住"会被
> 抓到）、没有串容器、卸载后两个窗口都消失。
> **为什么必须是两个不同的模块**：imgui 套件是 STATIC，每个插件模块各带一份套件与
> imgui 状态，而进程级问题（窗口类注册、各自的 ImGui context 与 D3D11 设备互不干扰）
> 只有加载两个不同模块才暴露得出来——历史上正是这条路崩过（进程级窗口类名只有一份）。
> **复核**：ctest `multi_plugin_imgui_in_one_process`（`tests/test_host_multi --imgui-pair`）；
> 清单见 `docs/design/adapter-spec.md` §8 与 `docs/design/conformance.md` 的覆盖表。

## 7. 套件版本资源（.rc）[P2] —— 已完成（W-07）

> **结论**：两个套件都调用了 `piplugin_add_version_resource()`，注释里那句
> `version_dll.rc.in 暂未提供` 已消失。`piplugin_qt`（SHARED）实测属性页
> `FileVersion=0.4.0` / `ProductVersion=0.4.0` / `FileDescription="piplugin Qt adapter
> kit"` / `OriginalFilename=piplugin_qtd.dll`；`piplugin_imgui`（STATIC）的 `.rc` 随
> `.lib` 备着但**不会**出现在任何二进制里（MSVC 链接器只按符号需求拉取静态库成员的
> 实测结论），改成 SHARED 那天自动生效。版本号来自 `project(VERSION)`，不会漂移。
> **复核**：`cmake/version_resource.cmake` 的注释；`docs/todo/framework.md` #8。
