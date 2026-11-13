# Qt 宿主里跑 Qt 插件：套件和直连都行，看插件要不要跨宿主

> 配套可运行示例：**`examples/qt_host_direct/`**（三步跑通 + `--self-test` 退出码）。
> 本文讲判断依据与硬规则，示例是它的可执行版本。
> 相关：`docs/tutorial/adapters.md`（套件本身的用法）、
> `docs/design/adapter-spec.md`（自写套件的契约）、
> `docs/todo/adapters.md` #4（本问题的原始记录与修正）。

## 1. 两条路，不是一条

Qt 宿主 × Qt 插件有**两种**合法接法。选哪个取决于插件要不要同时进非 Qt 宿主，而不是
"套件能不能用"：

| 路线 | 插件链接 | 集成方式 | 什么时候选 |
| --- | --- | --- | --- |
| **套件** | `piplugin_qt` | 插件实现 `IPiPluginViewProvider`（枚举 + 按 id 给 `IPiPluginView`）；宿主用 `PiPluginEmbedArea` 承载 | 插件要同时进非 Qt 宿主（imgui / 裸 Win32 / wxWidgets），一份代码到处跑 |
| **直连** | 只链 `piplugin` | 插件用 app 自定义协议交出 `QWidget*`，宿主 `layout->addWidget(...)` | 插件只服务 Qt 宿主，想少一层原生窗口嵌入 |

两条路都已在仓库里验证：套件那条的回归用例是 `qt_host_loads_qt_plugin`（Qt 宿主加载
`pi_plugin_test_plugin_qt.dll`，走完 attach → detach → 七步卸载），直连那条是
`examples/qt_host_direct` 的 `--self-test`。

### 曾经的错误结论，以及它错在哪

本文早先写过「Qt 宿主 + Qt 套件不可用，只能直连」，依据是实测 trace 只到
`attach: enter` 就停住。**那个症状是真的，但归因错了**：它不是 Qt 的限制，是套件的一句
多余判断——`pi_plugin_qt_app_create()` 无条件 `new QApplication`，撞上宿主已有的实例。

Qt 确实只允许一个 application object，但**这不构成"套件不能用"**。套件里 `g_app` 的
全部用途只有三件：创建它、销毁它、判断它存不存在。真正驱动 Qt 的地方（`on_idle` 里的
`processEvents`、post 队列里的 `postEvent`）走的是 `QCoreApplication::instance()`，
即"进程里那个 application"，与谁创建无关。所以正确做法是让套件**复用**宿主已有的实例、
且只销毁自己创建的那个——代码量很小，见 `pi_plugin_qt_app_create()` 的 `g_app_owned`。

## 2. 选型：一张表定生死

看**宿主**用什么工具包，再看**插件**用什么工具包：

| 宿主 | 插件 | 怎么办 |
|---|---|---|
| 非 Qt（imgui / 裸 Win32 / wxWidgets / 无头） | Qt（套件） | **走 Qt 套件**：套件建 `QApplication`、嵌原生子窗口、由宿主 `pi_plugin_on_idle()` 驱动。这是套件的目标场景（`examples/minimal_plugin_qt`） |
| **Qt** | Qt | **套件或直连都行**，见本文 §1：插件要同时进非 Qt 宿主就用套件（套件复用宿主的 `QApplication`），只服务 Qt 宿主可用直连（`examples/qt_host_direct`） |
| Qt | imgui | 走 imgui 套件 + 宿主侧 `PiPluginEmbedArea`（`host_kits/qt/`）：套件画在原生子窗口里，`PiPluginEmbedArea` 负责把它嵌进 Qt 控件树并转发尺寸/`pi_plugin_on_idle()` |
| Qt | 裸 Win32 / 自写套件 | 同上，用 `PiPluginEmbedArea` |
| 非 Qt | 非 Qt 且宿主愿意自绘 | 自写套件，照 `docs/design/adapter-spec.md`（可参考 `examples/minimal_kit_win32`） |

一句话：**套件的职责是"给没有 Qt 的宿主补一个 Qt 事件循环"；宿主自己就有事件循环
时，套件要补的那部分不但多余，而且和宿主的 `QApplication` 直接冲突。**

## 3. 直连的正确姿势

没有框架接口能表达"给我一个 `QWidget*`"（框架词汇表是工具包中立的），所以**由 app
定义协议**——这正是通道 A 的用途（`docs/design/interfaces.md` §5）：

```cpp
/* 应用自定义协议（C++ only：vtable 里带 QWidget*，这是 app 自己的协议，
 * 不改变框架的 C ABI —— 插件侧 IPiPluginBase/IPiPluginFactory 仍是纯 C） */
typedef struct IQtDirectWidgetVtbl {
    IPiUnknownVtbl base;
    QWidget* (PI_CALL *create_widget)(void* this_ptr);       /* 新建、无父、未 show */
    void     (PI_CALL *destroy_widget)(void* this_ptr, QWidget* widget);
} IQtDirectWidgetVtbl;
```

宿主侧全部集成代码：

```cpp
QApplication app(argc, argv);                 // 宿主自己的（关键）

pi_plugin_host_services_create_default(&OnHostMessage, &host, PI_INVALID_WINDOW, &services);
pi_plugin_host_session_create(services, &session);
pi_plugin_host_session_require(session, &PI_PLUGIN_QT_DIRECT_WIDGET_IID);   // 见 §4.3
pi_plugin_host_session_load(session, plugin_path, &slot);

IPiPluginBase* plugin = pi_plugin_host_session_get_plugin(session, slot);   // borrowed
IQtDirectWidget* ifc = nullptr;
pi_iunknown_query_interface((IPiUnknown*)plugin, &PI_PLUGIN_QT_DIRECT_WIDGET_IID, (void**)&ifc);

layout->addWidget(pi_plugin_qt_direct_create_widget(ifc));   // <- 直连就是这一行

app.exec();                                           // 宿主自己的事件循环
```

插件侧：只写 UI（`new QWidget` + 布局 + 信号槽），**不链接 `piplugin_qt`**，
不建 `QApplication`，不做任何原生窗口/嵌入：

```cpp
m_caps[0].iid = PI_PLUGIN_QT_DIRECT_WIDGET_IID; m_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;
/* 注意这里没有 PI_PLUGIN_IID_VIEW_PROVIDER：直连模式下 UI 不走框架的视图通道
 * （插件根本不暴露 provider，宿主 QI 它会失败），
 * 也没有 PI_PLUGIN_IID_HOST_UI：容器是宿主的 QLayout，不是原生窗口。 */
```

`examples/qt_host_direct/` 是这段代码的完整、可运行版本（含自定义协议的
wrapper 对象写法：两个 vtable 不能同时位于同一对象的 offset 0）。

## 4. 四条硬规则

### 4.1 线程：直连天然满足，但不要自作聪明

Qt 对象只能在 `QApplication` 所在线程创建/销毁。宿主调用插件协议方法发生在宿主 GUI
线程，也就是 `QApplication` 线程，所以**直连不需要任何
marshal**。反过来：不要为了"并行"把控件创建挪到工作线程——原生窗口的父子关系由同一
个线程服务，跨线程销毁/重挂会死锁（这条坑套件那边已经踩过，见 `adapters/qt/README.md`
的"线程模型（重要，不要改回去）"）。走套件时同理：`pi_plugin_get_view` 也在宿主 GUI
线程上被调到。

### 4.2 所有权与顺序：控件必须先死，模块才能卸载

`create_widget()` 返回的控件**归宿主**：宿主把它塞进布局、也负责销毁它。销毁必须在
`pi_plugin_host_session_unload()` **之前**完成，因为控件的信号槽函数体是**插件模块里的代码**；
模块一卸载，还活着的控件下次点击/重绘就跳进已释放内存。

```cpp
pi_plugin_qt_direct_destroy_widget(ifc, widget);     // 1) 先拆控件（插件模块仍映射）
pi_iunknown_release((IPiUnknown*)ifc);        // 2) 再放掉我们 QI 到的接口
pi_plugin_host_session_unload(session, slot);        // 3) 七步卸载序列（含模块卸载）
```

示例里的 `Host::TearDown()` 就是这个顺序，且幂等（交互模式的"Unload"按钮和自检模式
的定时器都调它）。这也是协议里为什么要有 `destroy_widget()` 槽位——直连插件没有套件
那种 teardown 钩子。

### 4.3 用能力门禁把"走错路的插件"挡在实例化之前

直连宿主**必须** `pi_plugin_host_session_require()` 自己的协议 IID。否则把"为套件写的" Qt
插件喂给它时，加载会成功（那个插件合法地声明了 `PI_PLUGIN_IID_VIEW_PROVIDER`），然后宿主 QI
不到协议、什么都不显示——又是一次静默失败，只是换了个地方发生。加上 require 之后：

```
load failed (hr=-8): plugin does not provide iid data1=0x9C3E71B5 required by this host
```

门禁在 descriptor 阶段执行，插件连实例都不会被创建。

### 4.4 直连插件不要链接套件

直连插件只需要 `piplugin` 与 Qt：

```cmake
target_link_libraries(my_qt_plugin PRIVATE piplugin Qt5::Widgets)   # 没有 piplugin_qt
```

理由不是"套件会炸"（它现在会复用宿主的 `QApplication`），而是**多一层无用的嵌入**：
直连时宿主直接持有 `QWidget*`，套件那一层原生子窗口、`on_idle` 驱动、resize 转发全部
用不上，白白多一个 DLL 依赖。

反过来，链接了套件却返回 `IPiPluginView` 的插件，在 Qt 宿主里也是能跑的（§1 的套件路线），
只是没必要。

## 5. 常见错误与症状

| 症状 | 原因 | 处置 |
|---|---|---|
| 插件界面完全不出现，trace 只有 `attach: enter` | 宿主持有 `QApplication`，套件版本过旧（没有复用分支，要建第二个） | 升级套件；当前版本会复用它 |
| 加载成功但 QI 不到协议，界面空白 | 宿主没 `require()`，或插件没声明 `PI_PLUGIN_CAP_PROVIDES` | §4.3 |
| 关闭窗口/卸载插件时崩溃 | 控件在模块卸载后仍活着（顺序错了） | §4.2：控件 → 接口 → unload |
| 控件出现但尺寸是 0 / 看不见 | 建出来就 `show()` 了，或没进布局 | 契约：无父、未 `show()`，由宿主的布局决定 |
| 控件里点击无响应 | 宿主事件循环没跑（`app.exec()` 没执行 / 被阻塞） | 直连不需要 `pi_plugin_on_idle()`，但需要宿主自己的循环在跑 |
| 卸载后再点插件控件崩溃 | 同上第 3 行；也可能是宿主把控件留在了布局里 | `delete` 后让 `QApplication` 处理完 DeferredDelete（示例用 `sendPostedEvents`） |

## 6. 怎么自查（无需人眼）

`examples/qt_host_direct` 的宿主内置 `--self-test`：加载 → QI → 收编 → **程序化点击
插件按钮** → 断言插件消息真的到达宿主 → 按顺序卸载 → 退出码判定。

```powershell
cd bin\Debug
.\pi_plugin_example_qt_direct_host.exe --self-test        # RESULT: PASS / exit 0
```

它同时是"走错路会怎样"的验证器：`--self-test pi_plugin_example_plugin_qt.dll` 应当失败在
能力门禁上（§4.3）。
