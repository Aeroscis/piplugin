# minimal_plugin_imgui — 最小 imgui 插件

整个插件就是：descriptor + factory + **一个 draw 回调**。
嵌入、ImGui context 隔离、事件驱动全在 `piplugin_imgui` 套件里，插件不碰任何窗口句柄。

## 三步跑通

```powershell
cmake --preset conan-default          # 需要 conan 提供 imgui
cmake --build --preset conan-debug --parallel
cd bin\Debug
.\pi_example_minimal_host.exe pi_example_plugin_imgui.dll
```

窗口里会出现一行文字、一个滑块和一个按钮；按按钮会 `pi_plugin_host_post_message(0x8000)`，
宿主打印 `[plugin message] msg=0x8000 wparam=N`。

## 值得抄的几点

- 能力声明：`PI_PLUGIN_IID_PLUGIN_VIEW (PROVIDES)` + `PI_PLUGIN_IID_HOST_UI (OPTIONAL)`；
  headless 宿主下 `QueryInterface(PI_PLUGIN_IID_HOST_UI)` 失败 → **不创建 view**（本插件在
  `GetView()` 里就是这么判断的），插件照常被加载；
- 入口用 `PI_PLUGIN_ENTRY_DECL` —— 它自带 C 链接（C++ 的名字修饰会让宿主找不到
  `pi_plugin_entry`，这一点由宏负责，写插件的人不用管）；
- 套件回调（`draw` / `retain` / `release`）都在**宿主 GUI 线程**上被调用，
  所以里面可以直接发消息，不需要锁。

配套阅读：`docs/tutorial/adapters.md`、`adapters/imgui/pi_imgui_view.h`。