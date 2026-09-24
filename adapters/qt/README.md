# piplugin_qt — Qt UI 适配器套件

> 写自己的套件？先读 **[`docs/design/adapter-spec.md`](../../docs/design/adapter-spec.md)**：
> 它把适配器与框架/宿主之间的契约写成条款 + 自查清单，并附一个照它写出来的最小套件
> （`examples/minimal_kit_win32`，纯 C 零工具包，已通过官方一致性验收）。本文余下部分
> 讲 Qt 套件的具体做法。

这是 piplugin 的第一个 **UI 适配器套件（adapter kit）**：它把"Qt 兼容层"
从插件代码中抽离出来，封装成一个可复用的 **SHARED 库**（一套 0.2.0 时是静态库，
见文末"边界与限制"）。任何 Qt 写的插件链接它之后，就获得了在**任意宿主**
（imgui、wxWidgets、裸 Win32……）窗口内运行 Qt 界面的能力，而宿主完全不需要
知道 Qt 的存在。

## 套件内部做了什么

| 职责 | 实现 |
|---|---|
| Qt 事件循环 | 进程（进程内所有 Qt 插件共有的一份套件）唯一的 `QApplication`，**创建在宿主的 GUI 线程上**；宿主每帧调 `pi_plugin_on_idle()`，套件在其中调 `processEvents()` 给 Qt 分一小片时间 |
| 嵌入宿主窗口 | `pi_plugin_attach()` 时先用 Qt 的 `_q_embedded_native_parent_handle` 属性把宿主容器 HWND 告知 Qt，**让 Qt 自己**把控件窗口创建成容器的 `WS_CHILD`（不做"顶层窗口 SetParent"、不叠加标题栏/边框、不按屏幕坐标算位置）；Linux/macOS 的 XEmbed/NSView 为 TODO |
| 尺寸/可见性 | `pi_plugin_on_resize()` / `pi_plugin_set_visible()` 直接同步执行——本来就在同一个线程，无需 marshal |
| 跨线程回调 | `pi_plugin_qt_view_post(view, fn, user)` **任意线程可调**：在宿主 GUI 线程上调用就是内联执行，从别的线程调用则异步排队，由宿主下一次 `pi_plugin_on_idle()` 取出并在 GUI 线程上执行（不阻塞、不引入第二条线程） |
| 生命周期 | 控件销毁、`QApplication` 析构、`user_data` 的 retain/release 全部在宿主线程**同步**完成；`pi_plugin_detach()` 返回时控件已经没了，`pi_release()`（引用归零）返回时 `QApplication` 也已经析构完——此后宿主 `FreeLibrary` 绝对安全 |

## 线程模型（重要，不要改回去）

**所有 Qt 调用都在宿主的 GUI 线程上。**

原因有两个，任何一个踩了都会出致命问题：

1. Qt 要求 `QApplication` 所在线程就是 GUI 线程。在后台线程建 `QApplication` 时
   Qt 自己会打印 `WARNING: QApplication was not created in the main() thread.`，
   并在退出/卸载路径上踩到竞态。
2. 更致命的是 Win32 的父子窗口规则：**销毁或重挂子窗口会向父窗口同步发送
   `WM_PARENTNOTIFY`/`WM_DESTROY`**。插件的控件被 `SetParent` 到宿主容器后，
   它的父窗口归宿主线程所有。如果控件活在第二个线程，那个线程执行
   `DestroyWindow` 时就会阻塞在"等宿主线程派发消息"上，而宿主线程此刻正阻塞在
   "等 Qt 干完"上——直接死锁；宿主等不及去 `FreeLibrary` 时，Qt 还在往里调用，
   就是插件卸载崩溃。

早期版本正是"后台 QThread 跑 `QApplication::exec()` + queued invocation +
等控件销毁的信号量"，结果就是**卸载崩溃**和**detach 卡死**。不要改回去。

`pi_plugin_qt_view_post()` 的跨线程投递**不是**上面那个模型：它没有第二条线程、没有
任何等待，只是把调用排进队列并往宿主 GUI 线程投一个 posted event，由宿主本来
就要调的 `pi_plugin_on_idle()` 执行（见下表"跨线程回调"）。队列里的调用在视图 detach /
析构后被丢弃——插件不该在控件已经没了之后再被回调。回归用例：ctest
`qt_view_post_from_worker_thread`（tests/test_host_multi，插件子线程调用，断言
回调落在宿主 GUI 线程上）。

正确的（当前）做法：

```
宿主 GUI 线程                         Qt
──────────────────────────────────────────────────────────────
pi_plugin_view_attach()
  └─ pi_plugin_qt_view::attach()             QApplication 构造（本线程）
     ├─ create_widget()               QWidget 构造 + 布局
     ├─ 告知 Qt 宿主容器 HWND          Qt 把控件窗口建成容器的 WS_CHILD
     ├─ setGeometry(0,0,容器客户区)     子窗口坐标 = 父客户区坐标，无边框
     └─ show() + RedrawWindow()

每帧 pi_plugin_view_on_idle()
  └─ processEvents(AllEvents, 4ms)    Qt 定时器/绘制/输入

pi_plugin_view_detach()
  └─ delete widget                    Qt 控件析构（本线程，父窗口同线程 → 不会死锁）

pi_release(view)
  └─ delete QApplication              Qt 全局清理（本线程，模块仍映射 → 安全）
```

## 插件作者的使用方式

```cpp
#include "pi_qt_view.h"

// 1. 写一个控件工厂（在宿主 GUI 线程被调用）
static QWidget* MakeUi(void* user) {
    MyPlugin* me = (MyPlugin*)user;
    QWidget* w = new QWidget();
    // ... 构建 UI，信号槽里可以用 me 发消息给宿主 ...
    return w;
}

// 2. 在 IPiPluginBase::pi_plugin_get_view 里把工厂交给套件
PiResult PI_CALL MyPlugin::GetView(void* self, IPiPluginView** out) {
    MyPlugin* me = (MyPlugin*)self;
    if (!me->m_hostUI) { *out = NULL; return PI_E_NOINTERFACE; } // 无头宿主
    PiPluginQtViewDesc desc = {};
    desc.create_widget = &MakeUi;
    desc.user_data     = me;
    desc.retain        = &MyPlugin::Retain;    // 可选：让套件在控件存活期间保活插件
    desc.release       = &MyPlugin::Release;
    return pi_plugin_qt_view_create(&desc, out);
}

// 3. 插件的 pi_plugin_terminate() 里收尾（可选但强烈建议）
PiResult PI_CALL MyPlugin::Term(void* self) {
    MyPlugin* me = (MyPlugin*)self;
    pi_plugin_qt_view_shutdown_owner(me);   // 幂等：拆掉**本插件**的控件（见下）
    return PI_OK;
}
```

CMake：

```cmake
target_link_libraries(my_plugin PRIVATE piplugin piplugin_qt)
```

套件是 SHARED 库，所以**插件部署时要带上 `piplugin_qt<后缀>.dll`**（放在插件 DLL
旁边或 PATH 上）。本仓库的构建会把套件 DLL 与 Qt 运行时 DLL 一起部署到
`bin/<CONFIG>/`。链接方式不变：`target_link_libraries` 一样写。

**关于 `pi_plugin_qt_view_shutdown_owner(owner)`**（`owner` = 创建视图时
`PiPluginQtViewDesc::user_data`，通常就是插件实例）：QUIT 时只拆**这个 owner 的**控件。
套件是进程共享的，`pi_plugin_qt_view_shutdown()`（不带 owner）会拆掉进程里**所有** Qt
插件的界面 —— 只有在确定整个进程的 Qt 用量都归你时才用它。`QApplication` 属于进程，
由**最后一个**销毁的视图负责析构，与谁先退出无关。

**宿主侧只要遵守两条**：

1. 每帧调用一次 `pi_plugin_view_on_idle()`（就是事件循环的"心跳"）。
2. 卸载插件前先 `pi_plugin_view_detach()` + `pi_release()`；插件的 `pi_plugin_terminate()`
   里调了 `pi_plugin_qt_view_shutdown_owner()` 的话，即使忘了这两步也不会崩。

## 宿主窗口嵌入的实现（Windows，重要）

宿主的容器窗口是"别的框架的原生窗口"，Qt 这边没有对应的 `QWidget`，所以**不能**用
`QWidget::setParent()` 把控件嵌进去。反过来"把已经建好的顶层控件窗口用 `SetParent`
塞进去"也不行——Qt 仍然认为那是个顶层窗口，于是：

- 每次设置尺寸都会额外加上顶层边框（标题栏 + 可调边框）的余量；
- 位置按**屏幕坐标**计算，而 `SetParent` 之后的子窗口是按**父客户区坐标**解释的。

表现就是插件界面在宿主里整体偏移并超出可见范围（被容器裁掉）；并且"最小化再恢复"
时几何被重新读回，看起来又正常了，一拖动窗口立刻坏掉。

套件改用 Qt 自带的"外来父窗口"机制：在控件原生窗口**创建之前**，把容器 HWND 写进控件的
动态属性 `_q_embedded_native_parent_handle`（`QWidgetPrivate::createTLSysExtra()` 会把它
拷到该控件的 `QWindow` 上，`WindowCreationData::fromWindow()` 读出后判定"此窗口不是顶层
窗口，必须建成该 HWND 的 `WS_CHILD`"）。这正是 ActiveQt 和 Qt 官方 QtWinMigrate 使用的
机制。于是：

- 边框余量恒为 0，`resize()` 得到的就是请求的尺寸；
- 几何一律按父窗口客户区坐标处理（`MoveWindow`），Qt 的模型与窗口管理器的实际状态始终一致；
- 宿主改变容器大小时，套件只调 `QWidget::setGeometry()`，不再绕过 Qt 直接 `SetWindowPos`，
  因此不会出现"Qt 的认知与真实几何不一致"。

由此得到一条约束（见 `PiPluginQtViewDesc::create_widget`）：**返回的控件必须是全新的、尚未
`show()` / `winId()` 过的控件**。上面的属性只在原生窗口创建的那一刻被读取，控件若已经
自带原生窗口就来不及了；这种情况下套件会退回旧的 `SetParent` 兜底路径（`PI_PLUGIN_QT_VIEW_TRACE=1`
时日志里能看到 `falling back to SetParent`），效果不如正常路径。

## 调试

设环境变量 `PI_PLUGIN_QT_VIEW_TRACE=1`，套件会把每一步生命周期写到
`<exe 目录>/pi_plugin_qt_view.log`，带线程 id 和时间戳。排查"插件卸载崩"“界面不动”
这类问题时先看这个文件——每一步应该都发生在同一个线程 id 上。

## 边界与限制

- **面向"非 Qt 宿主"**：套件会创建进程里唯一的 `QApplication`，并把它放在
  宿主的主线程/消息循环上（非 Qt 宿主的 GUI 线程就是进程主线程，符合 Qt 要求）。
  如果宿主自己就是 Qt 程序，不要用这个套件——应该让插件控件直接进宿主自己的
  Qt 事件循环。
- **SHARED（APP-08）**：套件是 SHARED 库，进程里只有一份，因此**同一进程可以同时
  加载多个 Qt 插件 DLL**，它们共用一个 `QApplication`（Qt 只允许一个）。
  0.2.0 时套件是 STATIC：每个 Qt 插件 DLL 各带一份套件状态，第二个插件 attach 时
  会去建第二个 `QApplication`，Qt 直接断言
  `"there should be only one application object"`。回归用例：
  `tests/test_host_multi`（ctest `multi_plugin_qt_in_one_process`）——两个不同的 Qt
  插件模块同时加载、各自嵌进自己的容器、各自继续跑 Qt 定时器，然后一起卸载。
  **部署要求**：插件运行环境必须能找到套件 DLL（`bin/<CONFIG>/` 已自动部署）。
- **宿主不要阻塞自己的消息循环太久**：Qt 的定时器/输入靠宿主的 `pi_plugin_on_idle()`
  驱动，宿主卡住的时候插件界面也会卡住（这是正确行为，不是 bug）。
- 后续可以按同样的模式增加 `piplugin_gtk`、`piplugin_webview`
  等套件，插件按需挑选。

## 宿主窗口的渲染注意事项

嵌入的是**另一个模块的原生子窗口**。宿主如果是 GPU 渲染（D3D/OpenGL），要注意
自己画的那块区域不能盖掉子窗口：

- 顶层窗口加 `WS_CLIPCHILDREN`。
- D3D 交换链用**翻转模型**（`DXGI_SWAP_EFFECT_FLIP_DISCARD`）。老式的
  `DISCARD`/`SEQUENTIAL` 是"blit 模型"，`Present()` 会把后缓冲直接 blit 到整个
  客户区、压掉子窗口的内容——表现就是"插件区域一闪一闪/只在宿主不渲染的时候
  才看得见"。翻转模型由 DWM 合成，子窗口正常叠在上面。

参考实现见 `tests/test_host/pi_imgui_test_host.cpp`（`CreateDeviceD3D()` 与
`CreateWindowW()` 的 `WS_CLIPCHILDREN`）。
