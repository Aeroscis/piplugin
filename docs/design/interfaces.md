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

## 5. 线程模型总结

| 组件 | 线程 |
|---|---|
| 宿主主循环 / `pi_on_idle` / `pi_on_resize` | 宿主 GUI 线程 |
| imgui 套件全部调用 | 宿主 GUI 线程（无内部线程） |
| Qt 套件 `create_widget` / 控件操作 | Qt 运行时线程（私有后台线程） |
| Qt 套件 `pi_qt_view_post` | 任意线程可调，marshal 到 Qt 线程 |
| `pi_host_post_message` | 任意插件线程可调（宿主负责 marshal） |
| 引用计数 | 原子操作，任何线程安全 |