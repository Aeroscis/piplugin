# pipluginframework_qt — Qt UI 适配器套件

这是 pipluginframework 的第一个 **UI 适配器套件（adapter kit）**：它把"Qt 兼容层"
从插件代码中抽离出来，封装成一个可复用的静态库。任何 Qt 写的插件链接它之后，
就获得了在**任意宿主**（imgui、wxWidgets、裸 Win32……）窗口内运行 Qt 界面的能力，
而宿主完全不需要知道 Qt 的存在。

## 套件内部做了什么

| 职责 | 实现 |
|---|---|
| Qt 事件循环 | 进程级共享的 **PiQtRuntime**：一个后台线程跑唯一的 `QApplication::exec()`，按引用计数启停 |
| 嵌入宿主窗口 | `IPiPluginView::pi_attach()` 时在 Qt 线程内 `SetParent` 把控件 HWND 挂进宿主容器（Linux/macOS 的 XEmbed/NSView 为 TODO） |
| 宿主驱动空闲 | `pi_on_idle()` 只做 dispatcher `wakeUp()`，绝不跨线程 `processEvents()` |
| 尺寸/可见性 | `pi_on_resize()` / `pi_set_visible()` 通过 queued invocation 自动 marshal 到 Qt 线程 |
| 生命周期 | 控件销毁、`user_data` 的 retain/release、运行时线程退出全部自动处理；`pi_detach()` 异步，`pi_release()`（引用归零）**同步阻塞**到 Qt 线程清理完毕并 join 运行时线程后才返回——保证宿主随后 `FreeLibrary` 安全 |

## 插件作者的使用方式

```cpp
#include "pi_qt_view.h"

// 1. 写一个控件工厂（在 Qt 运行时线程被调用）
static QWidget* MakeUi(void* user) {
    MyPlugin* me = (MyPlugin*)user;
    QWidget* w = new QWidget();
    // ... 构建 UI，信号槽里可以用 me 发消息给宿主 ...
    return w;
}

// 2. 在 IPiPluginBase::pi_get_view 里把工厂交给套件
PiResult PI_CALL MyPlugin::GetView(void* self, IPiPluginView** out) {
    MyPlugin* me = (MyPlugin*)self;
    if (!me->m_hostUI) { *out = NULL; return PI_E_NOINTERFACE; } // 无头宿主
    PiQtViewDesc desc = {};
    desc.create_widget = &MakeUi;
    desc.user_data     = me;
    desc.retain        = &MyPlugin::Retain;    // 可选：让套件在控件存活期间保活插件
    desc.release       = &MyPlugin::Release;
    return pi_qt_view_create(&desc, out);
}
```

CMake：

```cmake
target_link_libraries(my_plugin PRIVATE pipluginframework pipluginframework_qt)
```

## 边界与限制

- **面向"非 Qt 宿主"**：套件会创建进程里唯一的 `QApplication`。如果宿主自己就是
  Qt 程序，不要用这个套件——应该让插件控件直接进宿主自己的 Qt 事件循环。
- **静态库 + 单插件进程**：当前以 STATIC 库形式链接进每个插件 DLL；如果一个进程
  加载多个各自链接了本套件的 Qt 插件 DLL，会出现多个 `QApplication` 冲突。多 Qt
  插件场景请把套件编译为 SHARED 并让所有插件共用（TODO）。
- 后续可以按同样的模式增加 `pipluginframework_gtk`、`pipluginframework_webview`
  等套件，插件按需挑选。
