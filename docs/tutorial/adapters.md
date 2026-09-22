# 使用 UI 适配器套件（Adapters）

本文讲解两个 UI 适配器套件的用法：`piplugin_imgui`（立即模式 UI）与
`piplugin_qt`（窗口控件 UI）。两者的共同点：**插件作者只写纯 UI 逻辑**，
事件循环合并、窗口嵌入、线程 marshal、生命周期都由套件处理。

## 1. 选择哪个套件？

| 场景 | 选型 |
|---|---|
| 插件 UI 简单 / 想直接画控件、图表、调试面板 | **imgui 套件**（宿主 GUI 线程驱动，最简单） |
| 插件 UI 复杂 / 需要 Qt 控件库（表格、树、样式表） | **Qt 套件**（私有 Qt 事件循环线程） |
| 宿主本身就是 Qt 程序 | 不要用 Qt 套件——直接把控件放进宿主 Qt 事件循环（见限制） |
| 宿主自身也用 imgui | imgui 套件没问题（context 自动隔离） |

## 2. imgui 套件（piplugin_imgui）

### 2.1 用法

```cpp
#include "pi_imgui_view.h"
#include "imgui.h"

// 1. 每帧绘制回调（宿主 GUI 线程调用）
void DrawUi(void* user_data) {
    MyPlugin* me = (MyPlugin*)user_data;
    ImGui::Begin("My Panel");
    ImGui::Text("Hello from plugin!");
    static int v = 0;
    if (ImGui::SliderInt("Value", &v, 0, 100)) {
        pi_host_post_message(me->m_host, 0x1000, (uintptr_t)v, 0);
    }
    ImGui::End();
}

// 2. 可选：一次性初始化（context 建立后）
void SetupUi(void*) { ImGui::StyleColorsDark(); }

// 3. 在 IPiPluginBase::pi_get_view 中：
PiResult PI_CALL GetView(void* s, IPiPluginView** out) {
    if (!out) return PI_E_INVALIDARG;
    MyPlugin* me = (MyPlugin*)s;
    if (!me->m_hostUI) { *out = NULL; return PI_E_NOINTERFACE; }

    PiImGuiViewDesc desc = {};
    desc.init      = &SetupUi;     // 可选
    desc.draw      = &DrawUi;      // 必填
    desc.retain    = &Retain;      // 可选：attach 时 AddRef user_data
    desc.release   = &Release;     // 可选：detach 时 Release
    desc.user_data = me;
    return pi_imgui_view_create(&desc, out);
}
```

### 2.2 内部机制（了解即可）

- 套件在 `pi_attach` 时创建：子窗口（`WS_CHILD`，注册在插件模块）、D3D11 设备/交换链
  （硬件失败自动回退 WARP 软渲染）、独立 ImGui context + Win32/DX11 backend。
- `pi_on_idle()` 每调用一次渲染一帧（NewFrame → draw → Render → Present）。
- **context 隔离**：渲染期间把当前 ImGui context 切换为本套件自建的 context，
  调用结束恢复原 context —— 所以嵌进也用 imgui 的宿主不会互相污染。
- 契约：`pi_attach` / `pi_on_idle` / `pi_on_resize` / `pi_detach` 必须都在宿主 GUI 线程调用。

## 3. Qt 套件（piplugin_qt）

### 3.1 用法

```cpp
#include "pi_qt_view.h"

// 1. 控件工厂（Qt 运行时线程调用，返回无父 QWidget）
QWidget* MakeUi(void* user_data) {
    MyPlugin* me = (MyPlugin*)user_data;
    QWidget* w = new QWidget();
    QVBoxLayout* lay = new QVBoxLayout(w);
    QLabel* label = new QLabel("Qt Plugin UI", w);
    QSlider* slider = new QSlider(Qt::Horizontal, w);
    QObject::connect(slider, &QSlider::valueChanged, [me](int v) {
        if (me->m_host) pi_host_post_message(me->m_host, 0x1000, (uintptr_t)v, 0);
    });
    lay->addWidget(label);
    lay->addWidget(slider);
    return w;                       // 套件负责嵌入 + 尺寸
}

// 2. 在 pi_get_view 中：
PiResult PI_CALL GetView(void* s, IPiPluginView** out) {
    if (!out) return PI_E_INVALIDARG;
    MyPlugin* me = (MyPlugin*)s;
    if (!me->m_hostUI) { *out = NULL; return PI_E_NOINTERFACE; }

    PiQtViewDesc desc = {};
    desc.create_widget = &MakeUi;      // 必填
    desc.retain        = &MyPlugin::Retain;   // 可选：控件存活期间保活插件
    desc.release       = &MyPlugin::Release;
    desc.user_data     = me;
    return pi_qt_view_create(&desc, out);
}
```

### 3.2 手动 Qt 线程代码（进阶）

```cpp
QWidget* w = pi_qt_view_widget(view);      // 只能在 Qt 线程访问
pi_qt_view_post(view, &SomeFn, user);      // 任意线程可调，marshal 到 Qt 线程
```

### 3.3 内部机制

- **PiQtRuntime**：进程级共享。首个 view attach 时启动后台线程并创建唯一
  `QApplication`；最后一个 view detach 时异步 `quit()`。按用户数引用计数。
- **嵌入**：`pi_attach` 在 Qt 线程内 `SetParent` 把控件 HWND 挂进宿主容器（Windows）。
- **idle 驱动**：宿主的 `pi_on_idle()` 仅做 `wakeUp()` 唤醒 Qt dispatcher，
  绝不跨线程 `processEvents()`（那会触发跨线程 UB）。
- **同步释放**：`pi_release()` 引用归零后**同步阻塞**等待 Qt 线程清理完控件并 join
  运行时线程——保证宿主随后 `FreeLibrary`（卸载插件 DLL）安全。

## 4. CMake 链接

```cmake
# 插件 CMakeLists.txt
target_link_libraries(my_plugin PRIVATE piplugin)
target_link_libraries(my_plugin PRIVATE piplugin_imgui)  # 或 piplugin_qt
```

套件里 `piplugin_imgui` 是 STATIC（会把 imgui 一起带进来），
`piplugin_qt` 是 **SHARED**（进程里同一份套件状态，见注意事项表）。
找不到依赖时套件自检禁用（configure 有 STATUS/WARNING 提示）。
Qt 插件部署时要带上套件 DLL（本仓库构建会自动部署到 `bin/<CONFIG>/`）。

## 5. 注意事项与限制

| 事项 | 说明 |
|---|---|
| Qt 套件面向**非 Qt 宿主** | 宿主本身是 Qt 时不要用本套件 |
| 多 Qt 插件同进程 | ✅ 已支持：套件是 SHARED，所有 Qt 插件共用进程里唯一一个 `QApplication`；回归用例 `tests/test_host_multi`（ctest `multi_plugin_qt_in_one_process`）。拆控件用 `pi_qt_view_shutdown_owner(owner)`，不要用进程级的 `pi_qt_view_shutdown()` |
| Qt 套件 Linux/macOS 嵌入 | `SetParent` 仅 Windows；X11 XEmbed / NSView 嵌入未实现（TODO） |
| imgui 套件 | 仅 Windows（D3D11 backend），其他平台待移植 |
| 回调线程 | imgui 回调在宿主 GUI 线程；Qt 控件代码只能在 Qt 线程触碰 |