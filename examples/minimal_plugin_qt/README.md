# minimal_plugin_qt — 最小 Qt 插件

与 imgui 例子同构：descriptor + factory + 一个 **widget 工厂**。
`QApplication`、嵌入宿主容器、事件泵全部由 `piplugin_qt` 套件负责。

## 三步跑通

```powershell
# 1) 构建（Qt 是本地安装；找不到就给它路径，见 README 的"Qt 相关目标"）
cmake --preset conan-default -DPI_QT_PREFIX="C:/Qt/5.15.2/msvc2019_64"
cmake --build --preset conan-debug --parallel

# 2) 进开发目录（piplugin_qt*.dll 与 Qt 运行时 DLL 都在这里）
cd bin\Debug

# 3) 跑
.\pi_example_minimal_host.exe pi_example_plugin_qt.dll
```

点击按钮会更新标签并发 `pi_plugin_host_post_message(0x8000)`，宿主打印出来。

## 值得抄的几点

- **套件是 SHARED 库**（roadmap APP-08）：插件运行时必须能找到 `piplugin_qt<后缀>.dll`
  （本仓库的构建会把它部署到 `bin/<CONFIG>/`，别人的部署也要带上）；
- `pi_plugin_terminate()` 里调 `pi_plugin_qt_view_shutdown_owner(this)`：套件是进程共享的，
  不带 owner 的 `pi_plugin_qt_view_shutdown()` 会把**别的** Qt 插件的控件一起拆掉；
- widget 工厂返回的控件必须是**全新、未 show() 过**的控件：套件要在原生窗口创建前
  告诉 Qt "你的父窗口是宿主容器"（详见 `adapters/qt/README.md` 的嵌入说明）；
- 宿主是 Qt 程序时不要用这个套件（让插件控件直接进宿主自己的 Qt 事件循环）。

配套阅读：`docs/tutorial/adapters.md`、`adapters/qt/README.md`。