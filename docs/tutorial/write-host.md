# 编写一个宿主（Write a Host）

宿主是加载插件的一方。本教程说明如何用框架 API 写一个宿主程序（GUI 或 headless）。
参照实现：`tests/test_host`（imgui 宿主）、`tests/test_host_qt`（Qt 宿主）、
`tests/test_headless_host`（无头宿主）。

## 1. 宿主最小流程

```c
#include "piplugin/pi_plugin.h"

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
  `pi_service_start` / `pi_service_poll` / `pi_service_stop` 驱动。参照实现是
  `tests/test_plugin_service`（纯 C 服务插件）与 `tests/test_headless_host`
  （把 start / poll / 状态 / stop 幂等 / 卸载序列再 stop 逐条断言，ctest 用例
  `headless_host_service_lifecycle`）。要点：
  - `start()` 的必填选项缺失时返回 `PI_E_MISSINGCAPABILITY`（这是接口文档的承诺，
    不是 `PI_FAIL`）；
  - `poll()` 由**宿主的主循环**驱动（对应 GUI 插件的 `pi_on_idle`）；服务未运行时
    应当明确失败，而不是假装在工作；
  - `stop()` 幂等：卸载序列会再调一次，插件必须能承受；
  - 纯服务插件不必实现 `IPiPluginView`：`pi_get_view` 返回 `PI_E_NOINTERFACE` 即可。

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

## 6. 少写机制代码：宿主 kit L0（piplugin_host）

§1 的 1)~8) 是每个宿主都要重写一遍的**机制**代码（加载 / 能力门禁 / 实例化 / 七步卸载），
抄错一次顺序就是卸载崩溃。宿主 kit 的 L0 层把它收拢成一个会话对象：

```c
#include "pi_host_session.h"

PiPluginHostSession* session = NULL;
pi_host_session_create(host, &session);
pi_host_session_set_logger(session, OnSessionLog, NULL);  /* 步骤日志去哪由宿主决定 */

/* 本宿主生态要求插件必须 PROVIDES 的能力（特化协议门禁，可多次调用） */
pi_host_session_require(session, &MY_APP_PROTOCOL_IID);

uint32_t slot = PI_HOST_SESSION_INVALID_SLOT;
if (PI_FAILED(pi_host_session_load(session, "my_plugin.dll", &slot))) {
    printf("%s\n", pi_host_session_last_error(session));  /* 含拒绝理由 */
    return;
}

/* 容器由宿主自己创建并摆位；kit 只接收它 */
pi_host_session_attach_view(session, slot, (PiNativeWindow)myContainer, /*set_visible=*/1);

while (running) {
    …处理宿主自己的事件…
    pi_host_session_drive_idle(session);                  /* 每帧 pump；时机归宿主 */
    pi_view_on_resize(pi_host_session_get_view(session, slot), w, h);
}

pi_host_session_unload(session, slot);   /* 七步序列，顺序由 kit 保证 */
pi_host_session_destroy(session);
```

要点：

- kit **不创建窗口、不持计时器、不决定布局**：容器是谁、在哪、多大、几个、可否见，全归宿主；
- `pi_host_session_get_*` 返回的都是**借用**指针（禁止 release），所有权在 session；
- 需要"实例化前按 descriptor 过滤"时用 `pi_host_session_inspect()` +
  `pi_host_session_instantiate()`（`load()` 就是这两步合起来）；
- 门禁是**双向**的：插件 `PI_CAP_REQUIRED` 的能力宿主给不出就拒绝；宿主
  `pi_host_session_require()` 声明的能力插件没 `PROVIDES` 也拒绝；
- 三层纪律与边界见 `host_kits/README.md`；
- 参照实现：`tests/test_host`（imgui）、`tests/test_host_qt`（Qt）、
  `tests/test_headless_host`（headless，演示 inspect/instantiate 的实例化前门禁）。

## 7. 少写粘合代码：宿主 kit L1（嵌入胶水）

容器仍然由你自己创建和摆位；L1 只负责"让它成为一个正确的 embed host"。

**Qt 宿主**（`host_kits/qt/`，`PiPluginEmbedArea`）：

```cpp
#include "pi_host_embed_area.h"

g_embedArea = new PiPluginEmbedArea(this);   /* 你创建、你放进布局、你决定样式 */
g_embedArea->setStyleSheet("background-color: #26262a;");   /* 视觉决策归你，QSS 全穿透 */
layout->addWidget(g_embedArea, 1);

/* 插件加载成功后 */
g_embedArea->attach(session, slot);          /* 原生窗口 + attach + resize 转发，一步到位 */

/* 你自己的帧时钟里 */
g_embedArea->driveIdle();                    /* 或 setAutoIdleEnabled(true) 交给内部 QTimer(0) */

/* 卸载前 */
g_embedArea->detachBinding();
pi_host_session_unload(session, slot);
```

它只做 attach / resize 转发 / idle 驱动，不设样式、不画背景、不占布局。绑定的是
`(session, slot)` 而不是裸 view 指针，所以插件卸载后它自动拿到 NULL，不会留下悬垂指针。

**imgui + D3D11 宿主**（`host_kits/dx11/`）：

```cpp
#include "pi_host_dx11.h"

/* 顶层窗口 OR 上它（WS_CLIPCHILDREN），否则每次 Present 都会擦掉插件的像素 */
HWND hwnd = CreateWindowW(cls, title, WS_OVERLAPPEDWINDOW | pi_host_dx11_top_level_style(), ...);

PiHostDx11Desc desc = {};
desc.background[0] = 0.15f; desc.background[1] = 0.15f;   /* 传你的清屏色 */
desc.background[2] = 0.15f; desc.background[3] = 1.0f;
desc.log = &YourLogger;                      /* 创建/降级/resize 日志去哪由你决定 */
pi_host_dx11_create((PiNativeWindow)hwnd, &desc, &g_dx);

/* 容器：矩形你说了算 */
g_embedContainer = (HWND)pi_host_dx11_create_embed_container((PiNativeWindow)hwnd, 410, 0, w, h);

/* 每帧：kit 给你 device/context/render target，画什么由你决定 */
ImGui_ImplDX11_Init(pi_host_dx11_device(g_dx), pi_host_dx11_context(g_dx));
...
pi_host_dx11_prepare_size(g_dx, clientW, clientH);   /* WM_SIZE 里转发；策略在 kit 内 */
pi_host_dx11_present(g_dx, 1);
```

这一层固化的是**创建参数与 resize 策略**（flip model 的必要性、`DXGI_SCALING_NONE` 及其降级、
"缓冲只增不减"、帧延迟等待对象、背景色、`ResizeBuffers` 必须传回创建时的 flags），
不是"画什么"。

## 8. 把自己的服务暴露给插件（通道 B：宿主自定义服务）

框架只保证两个宿主 IID：`IPiHostServices` 与（GUI 宿主才有的）`IPiHostUI`。
app 要给插件自己的服务（配置中心、日志、任务队列、许可证……）时，用
`pi_host_services_create_ex()` 装一个 **extra-QI 钩子**：框架 IID 之外的
`QueryInterface` 全部转给你，插件侧**不需要任何新 API** —— 它只是对自己拿到的
那个宿主对象 QI 一次。

```c
#include "piplugin/pi_plugin.h"

/* 你自己的接口：IID 必须是随机 128 位 UUID（见 interfaces.md 5.1），
 * 不要用框架保留区（data1 < 0x80000000）里的小编号。 */
static const PiGuid MY_SERVICE_IID =
    PI_GUID(0x3B7E14C9, 0x2A5D, 0x4F31, 0x8E, 0x77, 0x51, 0xC2, 0x9A, 0x0B, 0x6D, 0x44);

/* 钩子：契约与 QueryInterface 完全相同 —— 认领时返回 PI_OK 并给出
 * **已 add-ref** 的接口指针；不认领返回 PI_E_NOINTERFACE 并把 *out 置 NULL；
 * 其它失败码会被原样上抛给插件（例如 PI_E_OUTOFMEMORY）。 */
static PiResult MyExtraQi(void* ctx, const PiGuid* iid, void** out)
{
    MyService* svc = (MyService*)ctx;
    if (pi_guid_equal(iid, &MY_SERVICE_IID)) {
        *out = &svc->base;                       /* 已 add-ref 的对象 */
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

IPiHostServices* host = NULL;
pi_host_services_create_ex(&MessageProc, /*user_data=*/NULL,
                           embedWindow,                  /* headless 传 PI_INVALID_WINDOW */
                           &MyExtraQi, &myService,       /* 传 NULL 等价于 create_default */
                           &host);
```

插件侧（它就是普通的 QI，宿主没提供就优雅降级）：

```c
IMyService* svc = NULL;
if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host, &MY_SERVICE_IID, (void**)&svc))) {
    pi_my_service_do_something(svc);
    pi_iunknown_release((IPiUnknown*)svc);        /* QI 返回 add-ref 过的 */
} else {
    /* 宿主没有这个服务：照常运行。声明为 PI_CAP_OPTIONAL 就是这个意思。 */
}
```

要点：

- **插件侧零新增 API**：这正是它相对 §6 的 `pi_host_session_require()`（通道 A，
  宿主消费插件定义的接口）的镜像；
- 钩子可能被插件的任意线程调用（插件可以从自己的线程 QI），要线程安全；
- 有窗口时的 `IPiHostUI`、以及 `IPiHostServices` 本身由框架先答掉，不会转到钩子；
  **headless 的 `IPiHostUI` 算未命中**，所以 app 也可以借此提供自己的 UI 服务；
- 参照实现：`tests/test_headless_host` 与 `tests/test_host` 都通过钩子暴露了一个
  app 自定义服务，两个测试插件 QI 它并调用（`tests/common/pi_test_host_service.h`），
  ctest 用例 `app_defined_host_service_*` 断言这条链路真的跑通。

## 9. 可选：C++ RAII 层（pi_cpp.h）

C++ 宿主可以少写引用计数样板：

```cpp
#include "piplugin/pi_cpp.h"        /* C++ 糖，不包含在 pi_plugin.h 里 */

static PiPtr<IPiHostServices> g_hostServices;   /* 析构即 release */

/* 创建：put() 给出参地址，句柄接管 create_default 返回的那一份引用 */
pi_host_services_create_default(&MessageProc, nullptr, window, g_hostServices.put());
pi_host_session_create(g_hostServices.get(), &session);

/* 退出路径：不再手写 release */
g_hostServices.reset();
```

再往上，`include/piplugin/pi_cpp.h` 还提供：

- `PiUniqueModule`：RAII `pi_module_load` / `pi_module_unload`（`PiUniqueModule::load(path, m)`
  + `module.factory()`），声明顺序即加载顺序时，C++ 的逆序析构正好等于七步卸载序列；
- `PiPtr<T>::qi_to<U>()`：按 `PiIidOf<U>` 的框架 IID 做一次 QI，失败返回空句柄；
- `pi_cpp_destroy<T>`：`PiRefCountedBase` 需要的 C++ 析构 thunk。

`PiPtr<T>` **接管**已有引用（框架返回的接口指针一律已 AddRef），借用指针
（descriptor / native window）禁止包进去；拷贝被删除，要第二份持有就写
`PiPtr<T>::add_ref(p)`。参照实现：`tests/test_host_qt`（宿主服务对象）、
`tests/unit_cpp`（语义 + Debug CRT 泄漏判定）。

## 10. 事件：让插件收得到、也发得出（通道 C）

两条投递路径，都建立在可选接口上（宿主没提供/插件没实现都照常跑）：

| 想要 | 用什么 | 谁决定收件人 |
|---|---|---|
| 宿主通知**某个**插件 | 插件实现 `IPiEventSink`，宿主 `pi_host_session_deliver_event()` | 宿主（按地址） |
| 按主题一对多 / 插件之间间接协作 | 宿主提供 `IPiHostEvents`，各方 `subscribe` | 订阅关系（按 topic） |

最快接入方式（用本仓库自带的可选路由糖，见 `host_kits/events/`）：

```c
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"
#include "pi_event_router.h"

static PiEventRouter* g_router;

/* 1) 先建路由器，再用它当宿主对象的 extra_qi 钩子（通道 B）：插件 QI 就能拿到 */
pi_event_router_create(&g_router);
pi_host_services_create_ex(&MessageProc, NULL, window,
                           &pi_event_router_extra_qi, g_router, &services);
pi_host_session_create(services, &session);          /* session 也会 QI 到它 */

/* 2) 宿主自己订阅（owner = NULL 表示"宿主自己"） */
IPiHostEvents* events = pi_event_router_host_events(g_router);
uint32_t h = 0;
pi_host_events_subscribe(events, "com.example.plugin.ready", NULL, OnReady, ctx, &h);

/* 3) 加载 + 投递（按地址；插件没有 sink 时返回 PI_E_NOINTERFACE，跳过即可） */
pi_host_session_load(session, "my_plugin.dll", &slot);
if (pi_host_session_has_event_sink(session, slot)) {
    PiPluginProperty p = { "greeting", "hello" };
    PiEvent ev = { 0 };
    ev.type = PI_EVENT_REQUEST; ev.topic = "com.example.host.welcome";
    ev.payload = &p; ev.payload_count = 1;
    pi_host_session_deliver_event(session, slot, &ev);   /* 宿主主线程 */
}

/* 4) 主循环里泵一次：把插件发布的事件分发给订阅者（时机归宿主） */
pi_event_router_pump(g_router);
```

规则（细则见 `docs/design/events.md` 与 `interfaces.md` 2.7/2.8）：

- `publish` 任意线程可调；**投递与订阅回调都在宿主主线程**，所以插件 sink 不需要锁；
- 事件是**尽力而为**的：可丢、可合并；`pi_event_router_stats()` 里的 `dropped_full`
  就是"丢了几个"的可观测证据；
- 订阅带 `owner`（插件传自己的实例指针）：插件**忘记退订也不会**在卸载后被回调，
  session 的卸载序列会按 owner 清干净 —— 这条由 kit 保证，宿主不用手写；
- topic 是**字面名字**（框架不定义通配），`pi.` 前缀留给框架。

## 11. 宿主清单（Checklist）

- [ ] 创建 `IPiHostServices`（GUI 传容器窗口 / headless 传 `PI_INVALID_WINDOW`）
- [ ] 要给插件自己的服务时用 `pi_host_services_create_ex()` 装 extra-QI 钩子（§8）
- [ ] `pi_module_load` 失败时检查 `pi_module_get_load_error()`
- [ ] 实例化前做能力门检查（`pi_descriptor_requires`）
- [ ] `pi_get_view` 成功后 attach + set_visible
- [ ] 主循环每帧 `pi_on_idle`
- [ ] 尺寸变化转发 `pi_on_resize`
- [ ] 卸载顺序：detach view → release view → terminate/release plugin → release factory → unload module → release host
- [ ] 需要事件时：建路由器并当 extra_qi 钩子挂上，主循环里 `pi_event_router_pump()`（§10）
- [ ] C++ 宿主：宿主服务对象与模块用 `PiPtr` / `PiUniqueModule` 持有，省掉手写 release（§9）