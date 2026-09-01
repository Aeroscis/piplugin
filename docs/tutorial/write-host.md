# 编写一个宿主（Write a Host）

宿主是加载插件的一方。本教程说明如何用框架 API 写一个宿主程序（GUI 或 headless）。
参照实现：`tests/test_host`（imgui 宿主）、`tests/test_host_qt`（Qt 宿主）、
`tests/test_headless_host`（无头宿主）。

## 1. 宿主最小流程

```c
#include "pipluginframework/pi_plugin.h"

/* 1) 创建宿主服务对象 */
IPiHostServices* host = NULL;
pi_host_services_create_default(&MessageProc, NULL,
                                /* GUI 宿主传嵌入窗口句柄；无头传 PI_INVALID_WINDOW */,
                                &host);

/* 2) 加载插件模块 */
PiPluginModule* module = pi_module_load("my_plugin.dll");
if (!module) { /* 查看 pi_module_get_load_error() */ }

/* 3) 取工厂 */
IPiPluginFactory* factory = NULL;
pi_module_get_factory(module, &factory);

/* 4) 能力门检查（可选但推荐，实例化前） */
const PiPluginDescriptor* desc = NULL;
pi_factory_get_descriptor(factory, &desc);
if (pi_descriptor_requires(desc, &PI_IID_HOST_UI)) {
    /* 我们是 headless 宿主：拒绝需要 GUI 的插件 */
    … return;
}

/* 5) 创建并初始化插件实例 */
PiGuid classGuid;
pi_factory_get_class_guid(factory, 0, &classGuid);
IPiPluginBase* plugin = NULL;
pi_factory_create_instance(factory, &classGuid, host, &plugin);
pi_plugin_initialize(plugin, host);

/* 6) GUI 宿主：获取并 attach 视图 */
IPiPluginView* view = NULL;
if (PI_SUCCEEDED(pi_plugin_get_view(plugin, &view)) && view) {
    pi_view_attach(view, (PiNativeWindow)embedContainerHandle);
    pi_view_set_visible(view, 1);
}

/* 7) 主循环：每帧驱动插件 + 转发尺寸变化 */
while (running) {
    …处理宿主自己的事件…
    if (view) pi_view_on_idle(view);            /* 每帧 pump 插件 */
    if (resized) pi_view_on_resize(view, w, h);
}

/* 8) 卸载：先释放插件，再卸载模块 */
if (view)  { pi_view_detach(view); pi_iunknown_release((IPiUnknown*)view); }
pi_plugin_terminate(plugin);
pi_iunknown_release((IPiUnknown*)plugin);
pi_iunknown_release((IPiUnknown*)factory);
pi_module_unload(module);
pi_iunknown_release((IPiUnknown*)host);
```

## 2. 消息回调

插件可随时通过 `pi_host_post_message(host, msg, wparam, lparam)` 向宿主发消息
（任何线程）。宿主在创建服务对象时注册回调：

```c
void MessageProc(void* user_data, uint32_t msg, uintptr_t wparam, intptr_t lparam)
{
    /* msg 0x80000000 以下为框架保留，以上为插件自定义 */
    printf("[plugin] msg=0x%04X wparam=%llu\n", msg, (unsigned long long)wparam);
}

pi_host_services_create_default(&MessageProc, /*user_data=*/NULL, window, &host);
```

宿主决定如何把消息 marshal 到自己的事件循环（测试宿主直接加锁写日志）。

## 3. GUI 宿主要点

### 3.1 提供 IPiHostUI

在 `pi_host_services_create_default` 的 `ui_parent_window` 参数传入**嵌入容器窗口**：

- imgui 宿主（`pi_test_host_imgui`）：创建一个子窗口 `g_embedContainer` 作为容器，
  把其 HWND 传给宿主服务对象。
- Qt 宿主（`pi_test_host_qt`）：`QWidget` + `Qt::WA_NativeWindow`，
  传 `container->winId()`。

### 3.2 驱动插件每帧

- imgui 宿主：主循环里 `PeekMessage` 循环之后调用 `pi_view_on_idle(view)`。
- Qt 宿主：`QTimer`（interval 0）在每次事件循环迭代触发 `pi_view_on_idle(view)`。

### 3.3 转发尺寸变化

- Win32：`WM_SIZE` 里调整容器并调用 `pi_view_on_resize(view, w, h)`。
- Qt：给容器安装 `eventFilter`，`QEvent::Resize` 时转发。

## 4. Headless 宿主要点

- `ui_parent_window` 传 `PI_INVALID_WINDOW` → 宿主**不暴露** `IPiHostUI`，
  插件的 `QueryInterface(PI_IID_HOST_UI)` 返回 `PI_E_NOINTERFACE` → 插件不建 UI。
- 实例化前做能力门：`pi_descriptor_requires(desc, &PI_IID_HOST_UI)` 为真 → 拒绝加载。
- 可以探测 `PI_IID_SERVICE`：headless 服务器宿主加载提供服务的插件并用
  `pi_service_start` / `pi_service_poll` / `pi_service_stop` 驱动（框架已提供接口，
  内置测试插件尚未实现 SERVICE，见 TODO）。

## 5. 便捷 API：pi_host_create_plugin

若你只需要"加载 + 创建 + 初始化一步到位"：

```c
PiPluginModule* module = NULL;
IPiPluginBase* plugin = NULL;
pi_host_create_plugin("my_plugin.dll", &classGuid, host, &plugin, &module);
/* 用完：
   pi_plugin_terminate/pi_iunknown_release(plugin)
   pi_module_unload(module)   */
```

> 注意 `out_module` 不能传 NULL（那样模块永远不会被卸载，故意泄漏以保证安全）；
> 请始终接收并管理模块的卸载。

## 6. 宿主清单（Checklist）

- [ ] 创建 `IPiHostServices`（GUI 传容器窗口 / headless 传 `PI_INVALID_WINDOW`）
- [ ] `pi_module_load` 失败时检查 `pi_module_get_load_error()`
- [ ] 实例化前做能力门检查（`pi_descriptor_requires`）
- [ ] `pi_get_view` 成功后 attach + set_visible
- [ ] 主循环每帧 `pi_on_idle`
- [ ] 尺寸变化转发 `pi_on_resize`
- [ ] 卸载顺序：detach view → release view → terminate/release plugin → release factory → unload module → release host