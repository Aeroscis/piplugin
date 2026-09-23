# UI 适配器待办（Adapters）

## 1. Qt 套件改为 SHARED 并共享运行时 [P1] —— 已完成（roadmap APP-08）

> **状态**：已落地。`piplugin_qt` 现在是 SHARED 库（导出宏 `PI_QT_API`，
> 部署到 `bin/<CONFIG>/`），进程级状态（唯一 `QApplication`、活视图登记表）
> 全进程一份，因此同一进程可以同时加载多个 Qt 插件 DLL。
> 配套改动：`pi_qt_view_shutdown_owner(owner)` 只拆本插件的视图（不带 owner 的
> `pi_qt_view_shutdown()` 保留为进程级大锤）；`QApplication` 由最后一个销毁的视图
> 析构。回归用例 `tests/test_host_multi`（ctest `multi_plugin_qt_in_one_process`）：
> 两个不同的 Qt 插件模块同时加载、各自嵌进自己的容器、各自继续跑 Qt 定时器、
> 一起干净卸载；改回 STATIC 时第二个 attach 会撞上 Qt 的
> `"there should be only one application object"` 断言。

## 2. 新增 gtk 套件（piplugin_gtk）[P2]

插件用 GTK 写 UI 时，按 `qt/` 套件同样的模式实现：私有线程跑 GTK 主循环 +
X11 嵌入。README（`adapters/qt/README.md`）已预留此方向。

## 3. 新增 webview 套件（piplugin_webview）[P2]

插件 UI 用 HTML/JS（WebView2 / WebKitGTK / WKWebView）时，封装成同一模式。

## 4. Qt 宿主内嵌 Qt 插件的一等用法 [P2]

当前 Qt 套件**只面向非 Qt 宿主**（README 明确限制）。补充文档/示例：
宿主本身是 Qt 时，插件控件直接进入宿主 Qt 事件循环的正确集成方式
（不经过本套件），或提供专门的 "Qt-host 直连" 桥接。

实测补充：`pi_test_host_qt.exe` 加载 Qt 插件时，套件的 `attach()` 在
`piqt_app_create()` 处就失败（进程内已存在宿主的 `QApplication`，套件拒绝再建一个），
插件控件根本不会被创建——即"Qt 宿主 + 本套件"当前是静默不可用，而不是"能用但不完美"。
`PI_QT_VIEW_TRACE=1` 时表现为日志只到 `attach: enter` 一行。

## 5. imgui 套件非 Windows backend [P1]

见 `platform.md` 第 1/2 条：imgui 套件目前只有 D3D11（Windows），
需 OpenGL/Vulkan/Metal backend 才能在 Linux/macOS 使用。

## 6. imgui 套件多宿主并发 [P2] —— 已完成（W-05）

> **状态**：已落地（W-05）。新增第二个 imgui 插件变体（`pi_test_plugin_imgui2.dll`，
> 与 A 只差 class GUID / 显示名 / 心跳码 0x2002 vs 0x2003，见
> `tests/test_plugin_imgui/CMakeLists.txt`），并把 `tests/test_host_multi` 扩成
> `--imgui-pair` 模式（ctest `multi_plugin_imgui_in_one_process`）：同进程同时加载
> **两个不同的 imgui 插件模块**、各自嵌进自己的容器、由同一个宿主循环驱动渲染
> 若干帧，然后一起干净卸载。
>
> 断言的是并发本身，而不是"没崩"：两个插件各自的窗口都在**自己的**容器里且可见；
> 每个插件在自己的 draw 回调里把帧计数报给宿主（心跳），两边都必须推进
> （"画了一帧就冻住"会被抓到）；帧循环之后两个窗口都还活着且没有串容器；卸载后
> 两个窗口都消失。
>
> 为什么必须是两个**不同的**模块：imgui 套件是 STATIC 库，每个插件模块各带一份
> 套件与 imgui 状态（宿主日志里那行 "statically linked into each plugin" 就是它），
> 而"第二个模块的窗口类注册 / 各自的 ImGui context 与 D3D11 设备互不干扰"这类
> 进程级问题只有加载两个不同模块才暴露得出来 —— 历史上正是这条路崩过（见
> CHANGELOG 的 "imgui adapter kit used ONE process-wide window class name"）。

imgui 套件渲染时切换全局 ImGui context（`with_own_context`），若同一 host
进程内多个插件各建 imgui view，理论上 context 保存/恢复能正确工作，
但未被测试覆盖——补充多插件 imgui 场景的测试。

## 7. 套件版本资源（.rc）[P2]

各套件 CMakeLists 中 Windows `.rc` 资源模板被注释（`version_dll.rc.in 暂未提供`），
提供模板后可让 DLL 带版本信息。