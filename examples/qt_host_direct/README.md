# qt_host_direct — Qt 宿主直连 Qt 插件（不走套件）

**宿主自己就是 Qt 程序**时的正确集成方式，与 `minimal_plugin_qt`（走 Qt 适配器套件）
正好是一对对照。

## 为什么需要这个例子

Qt 适配器套件（`piplugin_qt`）面向**非 Qt 宿主**：它负责创建进程里唯一的
`QApplication` 并从宿主的 `pi_plugin_on_idle()` 里驱动 Qt。宿主自己已经有 `QApplication`
时这条路径**不可用**——套件在 `piqt_app_create()` 里就会撞上宿主的实例（Qt 只允许
一个 application object），`PI_PLUGIN_QT_VIEW_TRACE=1` 时日志只到 `attach: enter` 就不再前进。
详见 `docs/tutorial/qt-host-direct.md`。

直连的集成只有一行：**插件把 `QWidget*` 交给宿主，宿主塞进自己的 `QLayout`。**

## 三步跑通

```powershell
# 1) 在仓库根构建（Qt 是本地安装，找不到时本例子自动跳过）
cmake --preset conan-default
cmake --build --preset conan-debug --parallel

# 2) 进开发目录（框架 DLL、Qt 运行时 DLL 都在这里）
cd bin\Debug

# 3) 跑：交互模式（关窗即卸载），或自检模式（退出码判定，适合脚本）
.\pi_example_qt_direct_host.exe
.\pi_example_qt_direct_host.exe --self-test
.\pi_example_qt_direct_host.exe --self-test pi_example_plugin_qt_direct.dll
```

自检模式期望输出（实测）：

```
== piplugin Qt host, direct integration ==
plugin: pi_example_plugin_qt_direct.dll
mode:   self-test
  [kit] load[0]: gate passed (category=Example/UI, capabilities=1)
  [kit] load[0]: ready (view:N service:N events:N)
  PASS plugin loaded
loaded: Example Qt Direct Plugin 1.0.0 (Example/UI)
  PASS plugin implements the app's widget protocol
[qt-direct plugin] created widget ... for the host
  PASS plugin created a widget for us
  PASS the adopted widget is visible inside the host's layout
  [host] clicking the plugin's button...
  [host] message from the plugin: msg=0x8000 wparam=1
  PASS plugin -> host message arrived end to end
[qt-direct plugin] destroying widget ... (host is done with it)
  [kit] unload[0]: begin / terminate plugin / unload module / done
  PASS slot is empty after unload
RESULT: PASS
```

注意 `ready (view:N service:N events:N)`：这个插件**没有** `IPiPluginView`，
UI 走的是 app 自定义协议（通道 A），不是套件通道。

## 故意指错插件会怎样（这也是本 example 的重点）

把宿主指向"为套件写的" Qt 插件，加载会**当场失败并指名原因**，而不是静默无界面：

```
> .\pi_example_qt_direct_host.exe --self-test pi_example_plugin_qt.dll
  [kit] load[0]: pi_example_plugin_qt.dll
  [kit] rollback[0]: unload module
load failed (hr=-8): plugin does not provide iid data1=0x9C3E71B5 required by this host
RESULT: FAIL
```

`pi_plugin_host_session_require()` 声明了"本宿主的插件必须实现我的 widget 协议"，门禁在
**实例化之前**执行，所以这类不匹配在 descriptor 阶段就被拦下。

## 读代码的顺序

1. `pi_qt_direct_protocol.h`：app 自定义协议（一个 `QWidget* create_widget()` +
   一个 `destroy_widget()`），以及三条契约（线程 / 所有权 / 销毁时机）；
2. `pi_qt_direct_host.cpp`：① 自己建 `QApplication` 与 `QLayout`；②
   `pi_plugin_host_session_require()` 声明生态要求；③ 加载后 QI 到协议；
   ④ `layout->addWidget(pi_plugin_qt_direct_create_widget(...))` —— 集成就这一行；
   ⑤ `TearDown()` 的销毁顺序（**控件 → 协议指针 → 卸载模块**）；
3. `pi_qt_direct_plugin.cpp`：插件侧只有 UI 代码，没有任何套件/原生窗口代码。

## 与相关文档的分工

| 文档 | 讲什么 |
|---|---|
| `docs/tutorial/qt-host-direct.md` | 何时走套件、何时直连；直连的四条硬规则与常见错误 |
| `docs/tutorial/adapters.md` | 适配器套件本身的用法（imgui / Qt 套件） |
| `examples/minimal_plugin_qt/` | 走套件的 Qt 插件（宿主不是 Qt 程序时用这个） |
| `host_kits/qt/`（`PiPluginEmbedArea`） | Qt 宿主嵌**非 Qt**插件（imgui / 原生 HWND）时用 |
