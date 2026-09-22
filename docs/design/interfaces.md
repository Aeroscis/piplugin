# 接口参考（Interface Reference）

> 本文档描述框架公开的全部接口、帮助函数与数据类型。
> 完整定义见 `include/pipluginframework/*.h`；`pi_plugin.h` 是总入口，包含所有头文件。

## 1. 数据类型（pi_plugin_types.h）

### 1.1 PiGuid — 128 位 GUID（COM 兼容布局）

```c
typedef struct PiGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} PiGuid;
```

构造宏：`PI_GUID(l, w1, w2, b1..b8)`。比较函数：`pi_guid_equal(a, b) -> int`。

### 1.2 PiResult — 结果码

| 宏 | 值 | 含义 |
|---|---|---|
| `PI_OK` | 0 | 成功 |
| `PI_FAIL` | -1 | 通用失败 |
| `PI_E_NOINTERFACE` | -2 | 不支持该接口 |
| `PI_E_INVALIDARG` | -3 | 非法参数 |
| `PI_E_OUTOFMEMORY` | -4 | 内存不足 |
| `PI_E_NOTIMPL` | -5 | 未实现 |
| `PI_E_UNEXPECTED` | -6 | 意外错误 |
| `PI_E_NOTFOUND` | -7 | 未找到 |
| `PI_E_MISSINGCAPABILITY` | -8 | 宿主缺少必需能力 |

辅助宏：`PI_SUCCEEDED(r)`（≥0）、`PI_FAILED(r)`（<0）。

### 1.3 PiNativeWindow — 原生窗口句柄

Windows/macOS 为 `void*`，Linux 为 `unsigned long`。
`PI_INVALID_WINDOW` 表示无窗口；`PI_IS_VALID_WINDOW(h)` 判断有效性。

### 1.4 PiPluginDescriptor — 插件描述符

```c
typedef struct PiPluginDescriptor {
    const char* name;        /* 人类可读名称 */
    const char* vendor;      /* 厂商 */
    const char* version;     /* 语义化版本 "1.0.0" */
    const char* category;    /* 分类 */
    uint32_t    api_version; /* PI_API_VERSION (0x00010000) */

    const PiPluginCapability* capabilities;
    uint32_t                  capability_count;
} PiPluginDescriptor;
```

配套函数：

- `pi_descriptor_find_capability(desc, iid)` → 匹配的 `PiPluginCapability*` 或 NULL
- `pi_descriptor_provides(desc, iid)` → 是否提供该能力
- `pi_descriptor_requires(desc, iid)` → 是否必需该能力

### 1.5 插件入口点

```c
typedef PiResult (*PiPluginEntryProc)(IPiPluginFactory** out_factory);
#define PI_PLUGIN_ENTRY_NAME "pi_plugin_entry"
#define PI_PLUGIN_ENTRY_DECL PI_EXPORT PiResult pi_plugin_entry(IPiPluginFactory** out_factory)
```

每个插件 DLL 必须导出 `pi_plugin_entry`。

## 2. 接口与帮助函数

### 2.1 IPiUnknown（pi_plugin_unknown.h）

根接口，vtable 为纯 C 函数指针结构：

```c
typedef struct IPiUnknownVtbl {
    PiResult (PI_CALL *pi_query_interface)(void* this_ptr, const PiGuid* iid, void** out);
    uint32_t (PI_CALL *pi_add_ref)(void* this_ptr);
    uint32_t (PI_CALL *pi_release)(void* this_ptr);
} IPiUnknownVtbl;
```

帮助函数（NULL 安全包装）：

- `pi_iunknown_query_interface(self, iid, &out)`
- `pi_iunknown_add_ref(self)`
- `pi_iunknown_release(self)`

**引用计数基类**（供插件作者复用，必须作为对象第一个成员）：

```c
typedef struct PiRefCountedBase {
    IPiUnknown          unk;
    volatile uint32_t   ref_count;
    PiDestroyProc       destroy;
} PiRefCountedBase;
```

- `pi_refcounted_init(base, vtbl)` — 初始化（destroy=NULL）
- `pi_refcounted_init_with_destroy(base, vtbl, destroy)` — 带销毁回调
- `pi_refcounted_add_ref(this_ptr)` / `pi_refcounted_release(this_ptr)` — 标准实现（可放进 vtable）

### 2.2 IPiHostServices / IPiHostUI（pi_plugin_host_services.h）

**IPiHostServices**（宿主总是提供）：

```c
void* (PI_CALL *pi_host_alloc)(void* this_ptr, size_t size);        /* 线程安全 */
void  (PI_CALL *pi_host_free)(void* this_ptr, void* ptr);           /* NULL 安全 */
void  (PI_CALL *pi_host_post_message)(void* this_ptr, uint32_t msg,
                                      uintptr_t wparam, intptr_t lparam);
```

帮助：`pi_host_alloc` / `pi_host_free` / `pi_host_post_message`。

**IPiHostUI**（GUI 宿主可选提供）：

```c
PiNativeWindow (PI_CALL *pi_host_get_parent_window)(void* this_ptr);
uint64_t       (PI_CALL *pi_host_ui_thread_id)(void* this_ptr);
```

帮助：`pi_host_ui_get_parent_window` / `pi_host_ui_thread_id`。

**宿主默认实现**：

```c
PiResult pi_host_services_create_default(
    PiHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,          /* PI_INVALID_WINDOW = headless */
    IPiHostServices** out_services);
void pi_host_default_set_ui_window(IPiHostServices* services, PiNativeWindow window);
```

### 2.3 IPiPluginFactory（pi_plugin_factory.h）

```c
const PiPluginDescriptor* (PI_CALL *pi_get_descriptor)(void* this_ptr);
uint32_t (PI_CALL *pi_get_class_count)(void* this_ptr);
PiResult (PI_CALL *pi_get_class_guid)(void* this_ptr, uint32_t index, PiGuid* guid);
PiResult (PI_CALL *pi_create_instance)(void* this_ptr, const PiGuid* guid,
                                       IPiHostServices* host, IPiPluginBase** out);
```

帮助：`pi_factory_get_descriptor` / `pi_factory_get_class_count` /
`pi_factory_get_class_guid` / `pi_factory_create_instance`。

### 2.4 IPiPluginBase（pi_plugin_base.h）

```c
PiResult (PI_CALL *pi_initialize)(void* this_ptr, IPiHostServices* host);
PiResult (PI_CALL *pi_terminate)(void* this_ptr);
PiResult (PI_CALL *pi_get_view)(void* this_ptr, IPiPluginView** out);  /* 无 GUI → PI_E_NOINTERFACE */
```

帮助：`pi_plugin_initialize` / `pi_plugin_terminate` / `pi_plugin_get_view`。

### 2.5 IPiPluginView（pi_plugin_view.h）

```c
PiResult        (PI_CALL *pi_attach)(void* this_ptr, PiNativeWindow parent_window);
PiResult        (PI_CALL *pi_detach)(void* this_ptr);
PiNativeWindow  (PI_CALL *pi_get_native_window)(void* this_ptr);
PiResult        (PI_CALL *pi_on_resize)(void* this_ptr, int32_t width, int32_t height);
PiResult        (PI_CALL *pi_on_idle)(void* this_ptr);
PiResult        (PI_CALL *pi_get_preferred_size)(void* this_ptr, int32_t* width, int32_t* height);
PiResult        (PI_CALL *pi_set_visible)(void* this_ptr, int32_t visible);
```

帮助：`pi_view_attach` / `pi_view_detach` / `pi_view_get_native_window` /
`pi_view_on_resize` / `pi_view_on_idle` / `pi_view_get_preferred_size` / `pi_view_set_visible`。

### 2.6 IPiService（pi_plugin_service.h）

headless / 服务端插件的可选接口：

```c
PiResult (PI_CALL *pi_service_start)(void* this_ptr,
                                     const PiServiceOption* options, uint32_t option_count);
PiResult (PI_CALL *pi_service_stop)(void* this_ptr);              /* 幂等 */
PiResult (PI_CALL *pi_service_poll)(void* this_ptr);              /* 宿主主循环驱动 */
PiResult (PI_CALL *pi_service_get_status)(void* this_ptr, int32_t* out_status);
```

状态码：`PI_SERVICE_STOPPED(0)` / `PI_SERVICE_STARTING(1)` / `PI_SERVICE_RUNNING(2)` / `PI_SERVICE_ERROR(3)`。

配置项：`PiServiceOption { const char* key; const char* value; }`（UTF-8）。

帮助：`pi_service_start` / `pi_service_stop` / `pi_service_poll` / `pi_service_get_status`。

## 3. 宿主管理 API（pi_plugin_host.h）

```c
PiPluginModule* pi_module_load(const char* path);                  /* 失败返回 NULL */
void            pi_module_unload(PiPluginModule* module);
const char*     pi_module_get_load_error(void);                    /* 上次失败的描述 */

PiResult pi_module_get_factory(PiPluginModule* module, IPiPluginFactory** out_factory);

/* 便捷加载：load + get_factory + create_instance + initialize 一步完成 */
PiResult pi_host_create_plugin(const char* dll_path,
                               const PiGuid* class_guid,
                               IPiHostServices* host,
                               IPiPluginBase** out_plugin,
                               PiPluginModule** out_module);      /* out_module 可 NULL（见下） */
```

> **注意**：DLL 卸载时机。模块必须存活到所有插件实例释放之后。`pi_host_create_plugin`
> 中若调用者传 `out_module == NULL`，为安全起见模块**故意不卸载**（接受泄漏），
> 因为卸载会使插件代码失效。请始终通过 `out_module` 取得模块并在释放插件后
> 调用 `pi_module_unload`。

## 4. UI 适配器套件 API

### 4.1 pi_imgui_view.h（imgui 套件）

插件作者只需提供回调：

```c
typedef void (*PiImGuiInitProc)(void* user_data);    /* 可选：context 建立后调一次 */
typedef void (*PiImGuiDrawProc)(void* user_data);    /* 必填：每帧绘制 */
typedef void (*PiImGuiRetainProc)(void* user_data);  /* 可选：attach 时保活 user_data */
typedef void (*PiImGuiReleaseProc)(void* user_data); /* 可选：detach 时释放 */

typedef struct PiImGuiViewDesc {
    PiImGuiInitProc    init;
    PiImGuiDrawProc    draw;
    PiImGuiRetainProc  retain;
    PiImGuiReleaseProc release;
    void*              user_data;
} PiImGuiViewDesc;

PiResult pi_imgui_view_create(const PiImGuiViewDesc* desc, IPiPluginView** out_view);
```

契约：`pi_attach` / `pi_on_idle` / `pi_on_resize` / `pi_detach` 必须在**宿主 GUI 线程**调用。

### 4.2 pi_qt_view.h（Qt 套件）

```c
typedef QWidget* (*PiQtCreateWidgetProc)(void* user_data);         /* 必填：Qt 线程调用 */
typedef void (*PiQtDestroyWidgetProc)(void* user_data, QWidget* widget); /* 可选 */
typedef void (*PiQtRetainProc)(void* user_data);                   /* 可选 */
typedef void (*PiQtReleaseProc)(void* user_data);                  /* 可选 */

typedef struct PiQtViewDesc {
    PiQtCreateWidgetProc  create_widget;
    PiQtDestroyWidgetProc destroy_widget;
    PiQtRetainProc        retain;
    PiQtReleaseProc       release;
    void*                 user_data;
} PiQtViewDesc;

PiResult pi_qt_view_create(const PiQtViewDesc* desc, IPiPluginView** out_view);
QWidget* pi_qt_view_widget(IPiPluginView* view);                       /* Qt 线程独占访问 */
void pi_qt_view_post(IPiPluginView* view, void (*fn)(void* user), void* user);
```

## 5. 定义你自己的接口（特化协议，通道 A）

框架刻意只提供**机制**：任何 GUID 标识的接口都能被 QI、能力声明与宿主门禁处理。
因此 app 作者不需要 fork 框架，就能实现"**我生态内所有插件必须符合 XXX**"这类特化协议：

- 插件侧：实现你的接口，在 descriptor 里声明 `PI_CAP_PROVIDES`；
- 宿主侧：声明"本生态要求该接口"，不满足的插件在**实例化之前**被拒绝。

### 5.1 GUID 分配规则（必读）

| 用途 | 谁分配 | 形式 |
|---|---|---|
| 框架接口 IID | 框架集中分配（`src/pi_plugin_unknown.c`） | `data1 < 0x80000000`，如 `0x00000003` |
| app / 第三方接口 IID | **你自己** | 随机生成的完整 128 位 UUID |
| 插件 class GUID | 插件作者 | 随机生成的完整 128 位 UUID |

生成一次即可，此后**永不改动**：

```bash
uuidgen                                            # Linux/macOS/WSL
python -c "import uuid; print(uuid.uuid4())"       # 任意平台
```

为什么随机就够：判定是 `pi_guid_equal` 对**完整 128 位**比较，随机值的碰撞概率可忽略。
框架保留 `data1 < 0x80000000` 只是把**编号风格**留给自己，避免"看起来像框架接口"的
伪 GUID（如 `0x00000021`）混进生态造成误读。`data1 = 0x00000000/01/02/03/10/11/20`
这七个已被占用，不要重复使用。

> IID 一旦随 PUBLIC 版本发布就是 ABI 的一部分：**只能新增接口，不能修改已发布的 vtbl**。

### 5.2 第一步：app 定义协议（`my_app_protocol.h`）

```c
#include "pipluginframework/pi_plugin.h"

/* 随机生成一次，此后永不改动（PI_GUID 展开成花括号初始化器，故定义为变量） */
static const PiGuid MY_APP_PROTOCOL_IID =
    PI_GUID(0x9F3C1D42, 0x7B08, 0x4E55, 0xA1, 0x6C, 0x0D, 0xF2, 0x88, 0x37, 0x51, 0xBE);

/* vtbl 的第一项必须是 IPiUnknownVtbl；方法一律 PI_CALL + 纯 C ABI 类型 */
typedef struct IMyAppProtocolVtbl {
    IPiUnknownVtbl base;

    PiResult (PI_CALL *pi_my_do_work)(void* this_ptr, const char* job, int32_t* out_result);
} IMyAppProtocolVtbl;

typedef struct IMyAppProtocol {
    const IMyAppProtocolVtbl* lpVtbl;
} IMyAppProtocol;

static inline PiResult pi_my_do_work(IMyAppProtocol* self, const char* job, int32_t* out)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_my_do_work) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_my_do_work((void*)self, job, out);
}
```

> 若你的编译器对头文件里未使用的 `static const` 报警，改为在一个 `.c` 里定义、
> 头里 `extern const PiGuid MY_APP_PROTOCOL_IID;` 即可。

### 5.3 第二步：插件实现它

```c
typedef struct MyPlugin {
    PiRefCountedBase base;     /* 必须是第一个数据成员 */
    IPiHostServices* host;     /* add-ref'd */
} MyPlugin;

static PiResult PI_CALL MyPlugin_DoWork(void* self_ptr, const char* job, int32_t* out)
{
    (void)self_ptr;
    if (!job || !out) return PI_E_INVALIDARG;
    *out = 42;                 /* 真正的工作 */
    return PI_OK;
}

/* QI：命中自己的 IID 就返回接口指针并 AddRef —— 返回的通常就是同一个对象 */
static PiResult PI_CALL MyPlugin_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    MyPlugin* me = (MyPlugin*)self_ptr;
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_IID_PLUGIN_BASE) ||
        pi_guid_equal(iid, &MY_APP_PROTOCOL_IID)) {
        *out = me;
        pi_refcounted_add_ref(self_ptr);       /* 宿主一定会 release，必须 AddRef */
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

/* vtbl：base 三项（用框架提供的标准实现）+ 你的方法 */
static const IMyAppProtocolVtbl s_protocol_vtbl = {
    { &MyPlugin_Qi, &pi_refcounted_add_ref, &pi_refcounted_release },
    &MyPlugin_DoWork
};

/* descriptor 里声明 PROVIDES —— 宿主据此在实例化前门禁 */
static PiPluginCapability s_caps[1];
static PiPluginDescriptor s_desc;

static void DeclareCapabilities(void)
{
    s_caps[0].iid   = MY_APP_PROTOCOL_IID;
    s_caps[0].flags = PI_CAP_PROVIDES;

    s_desc.name = "My Plugin";
    s_desc.vendor = "me";
    s_desc.version = "1.0.0";
    s_desc.category = "worker";
    s_desc.api_version = PI_API_VERSION;
    s_desc.capabilities = s_caps;
    s_desc.capability_count = 1;
}
```

> **C 语言的一个小坑**：把框架导出的 `pi_refcounted_add_ref` / `pi_refcounted_release`
> 直接填进 vtbl，在 **C** 里会触发 `warning C4232`（取 dllimport 函数地址，不保证跨模块标识）。
> 这是本框架填充 vtbl 的固有模式，C++ 编译不报。若你的工程把警告当错误，二选一：
> ① 局部 `#pragma warning(disable:4232)`；② 在插件里写一对薄封装
> `MyPlugin_AddRef/Release`（内部转调框架函数）再填进 vtbl，取到的就是本模块内的函数地址。
>
> （上面两段代码已用 `cl /W4 /utf-8 /std:c11` 与 `/std:c++17` 各编译通过。）

### 5.4 第三步：宿主消费它

用宿主 kit（推荐，见 `docs/tutorial/write-host.md`）：

```c
#include "my_app_protocol.h"
#include "pi_host_session.h"

PiPluginHostSession* session = NULL;
pi_host_session_create(host, &session);

/* 声明本 app 生态要求的能力：不满足的插件在 create_instance 之前就被拒绝 */
pi_host_session_require(session, &MY_APP_PROTOCOL_IID);

uint32_t slot = PI_HOST_SESSION_INVALID_SLOT;
if (PI_FAILED(pi_host_session_load(session, "my_plugin.dll", &slot))) {
    /* 例如：plugin does not provide iid data1=0x9F3C1D42 required by this host */
    printf("%s\n", pi_host_session_last_error(session));
    return;
}

IPiPluginBase* plugin = pi_host_session_get_plugin(session, slot);   /* 借用 */
IMyAppProtocol* proto = NULL;
if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)plugin,
                                             &MY_APP_PROTOCOL_IID, (void**)&proto))) {
    int32_t result = 0;
    pi_my_do_work(proto, "job-1", &result);
    pi_iunknown_release((IPiUnknown*)proto);    /* QI 返回的是 add-ref 过的 */
}
```

不用宿主 kit 的宿主，门禁自己做一次即可（必须在 `create_instance` 之前）：

```c
const PiPluginDescriptor* desc = NULL;
pi_factory_get_descriptor(factory, &desc);
if (!pi_descriptor_provides(desc, &MY_APP_PROTOCOL_IID))
    return;   /* 拒绝：本生态要求插件实现该接口 */
```

### 5.5 目前不能做的两件事

- **反向通道（宿主把自己的自定义服务提供给插件）**：`PiDefaultHost` 封闭，只认
  框架内建的三个 IID，插件无法 QI 到 app 自定义的宿主服务。这是 roadmap 的 APP-01
  （可组合宿主服务 `pi_host_services_create_ex`），尚未落地。
- **自定义接口的版本协商**：`descriptor->api_version` 的运行时检查属 roadmap BLK-03，
  尚未落地；在它完成前，接口演进请用"新增 IID + 新增接口"的方式，不要改已有 vtbl。

### 5.6 三条禁令

1. **不改已发布的 vtbl**——要扩展就新增 IID 与接口（COM 规则）；
2. **vtbl 里不放 C++ 类型、不抛异常**——必须纯 C ABI，调用约定一律 `PI_CALL`，
   否则 Rust/C#/Python 的 FFI 承诺立刻失效；
3. **QI 必须返回 AddRef 过的指针**——宿主（以及框架的 session）一定会 release。

## 6. 线程模型总结

| 组件 | 线程 |
|---|---|
| 宿主主循环 / `pi_on_idle` / `pi_on_resize` | 宿主 GUI 线程 |
| imgui 套件全部调用 | 宿主 GUI 线程（无内部线程） |
| Qt 套件 `create_widget` / 控件操作 | Qt 运行时线程（私有后台线程） |
| Qt 套件 `pi_qt_view_post` | 任意线程可调，marshal 到 Qt 线程 |
| `pi_host_post_message` | 任意插件线程可调（宿主负责 marshal） |
| 引用计数 | 原子操作，任何线程安全 |