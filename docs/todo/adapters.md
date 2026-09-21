# UI 适配器待办（Adapters）

## 1. Qt 套件改为 SHARED 并共享运行时 [P1]

**现状**（`adapters/qt/README.md` 明确标注）：
- 套件是 STATIC 库，链接进**每个** Qt 插件 DLL；
- 若一个进程加载多个各自链接套件的 Qt 插件 → 多个 `QApplication` 冲突。

**建议**：
1. 将 `pipluginframework_qt` 改为 SHARED 库，让所有 Qt 插件共享同一个
   `PiQtRuntime`（进程级唯一 `QApplication`）；
2. 需要解决：套件内部进程级状态（`g_rt_mutex` 等）从"每 DLL 一份"变成"进程共享一份"，
   静态库模式下这些全局量在各 DLL 中独立，SHARED 后统一。

## 2. 新增 gtk 套件（pipluginframework_gtk）[P2]

插件用 GTK 写 UI 时，按 `qt/` 套件同样的模式实现：私有线程跑 GTK 主循环 +
X11 嵌入。README（`adapters/qt/README.md`）已预留此方向。

## 3. 新增 webview 套件（pipluginframework_webview）[P2]

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

## 6. imgui 套件多宿主并发 [P2]

imgui 套件渲染时切换全局 ImGui context（`with_own_context`），若同一 host
进程内多个插件各建 imgui view，理论上 context 保存/恢复能正确工作，
但未被测试覆盖——补充多插件 imgui 场景的测试。

## 7. 套件版本资源（.rc）[P2]

各套件 CMakeLists 中 Windows `.rc` 资源模板被注释（`version_dll.rc.in 暂未提供`），
提供模板后可让 DLL 带版本信息。