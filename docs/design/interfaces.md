# 接口参考（Interface Reference）

> 本文档描述框架公开的全部接口、帮助函数与数据类型。
> 完整定义见 `include/piplugin/*.h`；`pi_plugin.h` 是 C 总入口，包含全部 C 头文件
> （C++ 语法糖 `pi_cpp.h` 是可选头，见 §7，不在总入口里）。

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
| `PI_E_VERSIONMISMATCH` | -9 | 插件与宿主的 `api_version` 不兼容 |

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
    uint32_t    api_version; /* PI_PLUGIN_API_VERSION；宿主据此做兼容门禁（见 1.5） */

    const PiPluginCapability* capabilities;
    uint32_t                  capability_count;

    /* API 0.3 追加（APP-04）：自由元数据，可 NULL */
    const PiPluginProperty*   properties;
    uint32_t                  property_count;
} PiPluginDescriptor;

typedef struct PiPluginProperty { const char* key; const char* value; } PiPluginProperty;
```

配套函数：

- `pi_plugin_descriptor_init(desc)` → **先把整个描述符清零**（可选字段一律"不存在"），再填字段
- `pi_plugin_descriptor_find_capability(desc, iid)` → 匹配的 `PiPluginCapability*` 或 NULL
- `pi_plugin_descriptor_provides(desc, iid)` → 是否提供该能力
- `pi_plugin_descriptor_requires(desc, iid)` → 是否必需该能力
- `pi_plugin_descriptor_find_property(desc, key)` → 属性的值，或 NULL（`properties` 为 NULL /
  `key` 为 NULL / 未声明该 key 都返回 NULL；重复 key 取第一个）

> **描述符必须清零后再填**（`pi_plugin_descriptor_init()` 或静态存储）。
> 它是**会追加字段**的结构（`properties` 就是 0.3 追加的），自动/动态存储的描述符默认是
> 未初始化内存（Debug 下是 `0xCDCDCDCD`）——宿主读到垃圾 `property_count` 就会去遍历
> 垃圾 `properties`，然后在**宿主自己**里崩，插件作者极难定位。
> 静态/全局描述符由语言保证清零，无需额外调用。

**properties 是自由元数据**：描述性事实（UI 工具包、支持的文件格式、主页、许可证……）
不该硬塞进 capabilities。键值都是 UTF-8、NUL 结尾；`pi.` 前缀保留给框架，app / 插件
用自有前缀（如 `com.example.thing`）；键按字节比较（大小写敏感）。

**追加字段 = 布局变化，读方要判版本**：`properties` 是 0.3 才追加的，0.2 编译出来的
模块结构体更短。版本门禁会拒绝"比宿主新"的插件，但**接受更老的**（同 major），所以
读追加字段前必须用插件自己声明的 `api_version` 判布局 —— `pi_plugin_descriptor_find_property()`
内部就是这么做的（`minor < 3` 直接当"没有属性"）。以后再加字段，沿用同一手法。

### 1.5 api_version 协商策略

`api_version` 是**插件编译时所用框架 API 的版本**，编码为 `major << 16 | minor`
（用 `PI_PLUGIN_API_VERSION_MAJOR` / `PI_PLUGIN_API_VERSION_MINOR` / `PI_PLUGIN_API_VERSION_MAKE` 读写）。

**取值规则：`PI_PLUGIN_API_VERSION` 的 `major.minor` 与发布版本始终一致**。
当前 **API 0.4 = 发布 0.4.0**：0.3 来自 APP-04（给 descriptor 追加 `properties`，
二进制布局变化），0.4 来自 APP-06（新增事件接口 `IPiPluginEventSink` / `IPiPluginHostEvents`，纯新增）。
三处版本（`CMakeLists.txt` 的 `project(VERSION)`、`conanfile.py` 的 `version`、
这里的 `major.minor`）必须一起改 —— 单测里的版本 tripwire 会拦住忘记同步的人。
1.0 是"ABI 冻结承诺"的时刻：在那之前每个 `x` 版本都可以改 ABI，
所以 pre-1.0 的插件应随宿主一起升级；1.0 之后 major 只在真正破坏 ABI 时才动，
minor 递增表示"只新增接口"。

| 情况 | 判定 |
|---|---|
| major 不同 | **不兼容** —— vtbl 布局可能已变，宿主拒绝加载 |
| major 相同、插件 minor ≤ 宿主 minor | 兼容，放行 |
| major 相同、插件 minor > 宿主 minor | **拒绝** —— 插件可能用到宿主还没有的接口 |

判定入口：`pi_plugin_api_version_compatible(host_version, plugin_version)`（核心库导出，
返回非 0 表示可以加载）。

**谁迁就谁：插件迁就宿主。** 宿主是自己进程的主人，不会为了某个插件升级框架；
插件应尽量按较低的 API 版本编译，被拒时提示用户升级宿主。

宿主侧的门禁在宿主 kit L0 里实现（`pi_plugin_host_session_load()` / `inspect()` 内部），
位置在 `pi_plugin_factory_create_instance()` **之前**，因此不兼容的插件连实例都不会被创建。
被拒时返回 `PI_E_VERSIONMISMATCH`，可读原因在 `pi_plugin_host_session_last_error()`，
形如：

```
plugin api_version 0x00020000 (major 2, minor 0) is incompatible with host
0x00010000 (major 1, minor 0); the plugin must not be newer than the host
```

不用宿主 kit 的宿主，自己做一次同样的检查即可（同样必须在实例化之前）：

```c
const PiPluginDescriptor* desc = NULL;
pi_plugin_factory_get_descriptor(factory, &desc);
if (desc && !pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION, desc->api_version))
    return;   /* 拒绝：版本不兼容 */
```

> 这里的 `api_version` 只管**框架 API**；app 自己定义的接口怎么演进版本，见 5.7。

### 1.6 插件入口点

```c
typedef PiResult (*PiPluginEntryProc)(IPiPluginFactory** out_factory);
#define PI_PLUGIN_ENTRY_NAME  "pi_plugin_entry"
#define PI_PLUGIN_ENTRY_EXPORT      /* 插件侧：dllexport / visibility("default") */
#define PI_PLUGIN_ENTRY_DECL  PI_PLUGIN_ENTRY_EXPORT PiResult pi_plugin_entry(IPiPluginFactory** out_factory)
```

每个插件 DLL 必须导出 `pi_plugin_entry`。定义时用 `PI_PLUGIN_ENTRY_DECL`：

```c
PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory) return PI_E_INVALIDARG;
    *out_factory = (IPiPluginFactory*)&MyFactory;   /* 工厂引用计数需 ≥ 1 */
    return PI_OK;
}
```

> **不要用 `PI_PLUGIN_API` 去定义入口**：它在插件侧展开成 `dllimport`，用在定义上会
> 编译失败（"definition of dllimport function not allowed"）。`PI_PLUGIN_API` 是
> **框架侧**的导出宏；插件侧用 `PI_PLUGIN_ENTRY_EXPORT`。

## 2. 接口与帮助函数

### 2.1 IPiUnknown（基础层 `<pibase/pi_base.h>`）

> 根接口及其 IID `PI_IID_UNKNOWN` 属**家族根词汇**，由基础层 pibase 提供，
> 不再是本库的头文件。本库中 `pi_` 裸前缀的标识符都是这个来源。

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

### 2.2 IPiPluginHostServices / IPiPluginHostUI（pi_plugin_host_services.h）

**IPiPluginHostServices**（宿主总是提供）：

```c
void* (PI_CALL *pi_plugin_host_alloc)(void* this_ptr, size_t size);        /* 线程安全 */
void  (PI_CALL *pi_plugin_host_free)(void* this_ptr, void* ptr);           /* NULL 安全 */
void  (PI_CALL *pi_plugin_host_post_message)(void* this_ptr, uint32_t msg,
                                      uintptr_t wparam, intptr_t lparam);
```

帮助：`pi_plugin_host_alloc` / `pi_plugin_host_free` / `pi_plugin_host_post_message`。

**IPiPluginHostUI**（GUI 宿主可选提供）：

```c
PiNativeWindow (PI_CALL *pi_plugin_host_get_parent_window)(void* this_ptr);
uint64_t       (PI_CALL *pi_plugin_host_ui_thread_id)(void* this_ptr);
```

帮助：`pi_plugin_host_ui_get_parent_window` / `pi_plugin_host_ui_thread_id`。

**宿主默认实现**：

```c
PiResult pi_plugin_host_services_create_default(
    PiPluginHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,          /* PI_INVALID_WINDOW = headless */
    IPiPluginHostServices** out_services);
void pi_plugin_host_default_set_ui_window(IPiPluginHostServices* services, PiNativeWindow window);
```

> **运行时切换嵌入窗口（W-02）**：`pi_plugin_host_default_set_ui_window()` 改的是宿主
> 报告的活值（插件已经拿到的 `IPiPluginHostUI` 指针立刻读到新容器），但**不会**自己
> 搬动插件的 view —— 容器是宿主的自由。宿主把插件界面换到另一个容器上的完整
> 动作是三件事，顺序不能反：
>
> ```c
> pi_plugin_view_detach(view);                                   /* 1. 同步销毁旧容器里的控件 */
> pi_plugin_host_default_set_ui_window(services, new_container);  /* 2. 让 IPiPluginHostUI 报告新容器 */
> pi_plugin_view_attach(view, new_container);                     /* 3. 在新容器里重建 */
> pi_plugin_view_set_visible(view, 1);
> ```
>
> 第 1 步是同步的：`pi_plugin_view_detach()` 返回时旧的原生窗口必须已经没了（不然新旧
> 窗口会抢同一个容器的绘制区域）。回归用例：ctest `container_switch_runtime`
> （imgui 插件）与 `container_switch_runtime_qt`（Qt 插件）—— 覆盖 A→B→A 切换、
> detach 同步性、切换后的尺寸往返与卸载。

**可组合宿主服务（APP-01，通道 B）**：默认对象只认框架的 IID，app 无法把
自己的服务递给插件。`create_ex` 多一个 extra-QI 钩子，框架 IID 之外的
`QueryInterface` 全部转交宿主：

```c
/* 与 IPiUnknown::pi_query_interface 同契约：认领 -> PI_OK + 已 add-ref 的指针；
 * 不认领 -> PI_E_NOINTERFACE + *out = NULL；其它失败码原样上抛。 */
typedef PiResult (*PiPluginHostExtraQiProc)(void* ctx, const PiGuid* iid, void** out);

PiResult pi_plugin_host_services_create_ex(
    PiPluginHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,
    PiPluginHostExtraQiProc extra_qi, void* extra_qi_ctx,   /* 传 NULL = 等价于 create_default */
    IPiPluginHostServices** out_services);
```

插件侧不需要任何新 API —— 它只是对自己拿到的宿主对象做一次普通 QI；宿主没有
该服务就返回 `PI_E_NOINTERFACE`，插件照常运行。`IPiPluginHostServices` 与（有窗口时的）
`IPiPluginHostUI` 由框架先答掉，不会转给钩子；headless 的 `IPiPluginHostUI` 算未命中，钩子
仍可认领。示例与完整契约见 `docs/tutorial/write-host.md` §8。

### 2.3 IPiPluginFactory（pi_plugin_factory.h）

```c
const PiPluginDescriptor* (PI_CALL *pi_plugin_get_descriptor)(void* this_ptr);
uint32_t (PI_CALL *pi_plugin_get_class_count)(void* this_ptr);
PiResult (PI_CALL *pi_plugin_get_class_guid)(void* this_ptr, uint32_t index, PiGuid* guid);
PiResult (PI_CALL *pi_plugin_create_instance)(void* this_ptr, const PiGuid* guid,
                                       IPiPluginHostServices* host, IPiPluginBase** out);
```

帮助：`pi_plugin_factory_get_descriptor` / `pi_plugin_factory_get_class_count` /
`pi_plugin_factory_get_class_guid` / `pi_plugin_factory_create_instance`。

### 2.4 IPiPluginBase（pi_plugin_base.h）

```c
PiResult (PI_CALL *pi_plugin_initialize)(void* this_ptr, IPiPluginHostServices* host);
PiResult (PI_CALL *pi_plugin_terminate)(void* this_ptr);
PiResult (PI_CALL *pi_plugin_get_view)(void* this_ptr, IPiPluginView** out);  /* 无 GUI → PI_E_NOINTERFACE */
```

帮助：`pi_plugin_initialize` / `pi_plugin_terminate` / `pi_plugin_get_view`。

### 2.5 IPiPluginView（pi_plugin_view.h）

```c
PiResult        (PI_CALL *pi_plugin_attach)(void* this_ptr, PiNativeWindow parent_window);
PiResult        (PI_CALL *pi_plugin_detach)(void* this_ptr);
PiNativeWindow  (PI_CALL *pi_plugin_get_native_window)(void* this_ptr);
PiResult        (PI_CALL *pi_plugin_on_resize)(void* this_ptr, int32_t width, int32_t height);
PiResult        (PI_CALL *pi_plugin_on_idle)(void* this_ptr);
PiResult        (PI_CALL *pi_plugin_get_preferred_size)(void* this_ptr, int32_t* width, int32_t* height);
PiResult        (PI_CALL *pi_plugin_set_visible)(void* this_ptr, int32_t visible);
```

帮助：`pi_plugin_view_attach` / `pi_plugin_view_detach` / `pi_plugin_view_get_native_window` /
`pi_plugin_view_on_resize` / `pi_plugin_view_on_idle` / `pi_plugin_view_get_preferred_size` / `pi_plugin_view_set_visible`。

### 2.6 IPiPluginService（pi_plugin_service.h）

headless / 服务端插件的可选接口：

```c
PiResult (PI_CALL *pi_plugin_service_start)(void* this_ptr,
                                     const PiPluginServiceOption* options, uint32_t option_count);
PiResult (PI_CALL *pi_plugin_service_stop)(void* this_ptr);              /* 幂等 */
PiResult (PI_CALL *pi_plugin_service_poll)(void* this_ptr);              /* 宿主主循环驱动 */
PiResult (PI_CALL *pi_plugin_service_get_status)(void* this_ptr, int32_t* out_status);
```

状态码：`PI_PLUGIN_SERVICE_STOPPED(0)` / `PI_PLUGIN_SERVICE_STARTING(1)` / `PI_PLUGIN_SERVICE_RUNNING(2)` / `PI_PLUGIN_SERVICE_ERROR(3)`。

配置项：`PiPluginServiceOption { const char* key; const char* value; }`（UTF-8）。

帮助：`pi_plugin_service_start` / `pi_plugin_service_stop` / `pi_plugin_service_poll` / `pi_plugin_service_get_status`。

协议细节（由 `tests/test_plugin_service` + `headless_host_service_lifecycle`
逐条断言，照抄那个插件就是一份可用的落地示例）：

- `pi_plugin_service_start`：必填选项缺失返回 `PI_E_MISSINGCAPABILITY`（不是 `PI_FAIL`）；
  重复 start 一个已在运行的服务应幂等成功；
- `pi_plugin_service_stop`：**任何时候都可调用**（没 start 过、已经停过都返回 `PI_OK`），
  卸载序列会再调一次；
- `pi_plugin_service_poll`：只在运行时返回 `PI_OK`；已停时返回 `PI_FAIL`，不要假装在工作；
- `pi_plugin_service_get_status`：`out_status == NULL` 返回 `PI_E_INVALIDARG`；
- 纯服务插件不必实现 `IPiPluginView`，`pi_plugin_get_view` 返回 `PI_E_NOINTERFACE`。

### 2.7 IPiPluginEventSink（pi_plugin_events.h，API 0.4）

**插件可选实现**的事件接收口。宿主在实例化后 QI 一次，命中就按槽位持有，投递用
`pi_plugin_host_session_deliver_event()`：

```c
typedef struct PiPluginEvent {
    uint32_t                 type;          /* PI_PLUGIN_EVENT_NOTIFY / REQUEST / 自定义 */
    const char*              topic;         /* UTF-8；`pi.` 保留给框架 */
    const PiPluginProperty*  payload;       /* 可 NULL；键值对，借用 */
    uint32_t                 payload_count;
    const PiGuid*            origin;        /* 宿主填入（发布者）；宿主自己发布为 NULL */
} PiPluginEvent;

PiResult (PI_CALL *pi_plugin_event_deliver)(void* this_ptr, const PiPluginEvent* event);
```

帮助：`pi_plugin_event_deliver`（NULL 安全：无 sink 时返回 `PI_E_NOINTERFACE`）。

契约要点：

- **只在宿主主/owner 线程被调用**（就是调用 `pi_plugin_initialize()` 的那条线程），
  因此插件 sink 里**不需要锁**，可以直接碰 UI/句柄；
- 只在 `pi_plugin_initialize()` 之后、`pi_plugin_terminate()` 之前投递；
- 返回 `PI_OK`（已处理）/ `PI_E_NOTIMPL`（不关心该 topic）/ `PI_FAIL`（处理出错）；
  **任何返回码都不是宿主的错误条件** —— 事件是尽力而为，宿主不得重试或报错；
- `event` 及其字符串**只在本次调用期间有效**，不得保存、不得阻塞过久。

### 2.8 IPiPluginHostEvents（pi_plugin_events.h，API 0.4）

**宿主可选提供**的事件路由口：插件对它 QI（`pi_plugin_host_events_query(host, &events)`），
然后发布/订阅。宿主通常用 `pi_plugin_host_services_create_ex()` 的 extra_qi 钩子把它挂上（见 2.2）。

```c
PiResult (PI_CALL *pi_plugin_host_events_publish)(void* this_ptr, const PiPluginEvent* event);
PiResult (PI_CALL *pi_plugin_host_events_subscribe)(void* this_ptr, const char* topic,
                                             void* owner, PiPluginEventCallback cb,
                                             void* user_data, uint32_t* out_subscription);
PiResult (PI_CALL *pi_plugin_host_events_unsubscribe)(void* this_ptr, uint32_t subscription);
PiResult (PI_CALL *pi_plugin_host_events_drop_owner)(void* this_ptr, void* owner);
```

帮助：`pi_plugin_host_events_publish` / `_subscribe` / `_unsubscribe` / `_drop_owner` /
`pi_plugin_host_events_query`。

| 规则 | 内容 |
|---|---|
| 线程 | `publish` **任意线程**可调（宿主 marshal）；`subscribe` / `unsubscribe` / `drop_owner` 与**回调**都在宿主主线程 |
| topic | UTF-8、字节精确、大小写敏感；`pi.` 保留给框架；app 用自有前缀；**框架不定义通配语法**；上限 `PI_PLUGIN_EVENT_TOPIC_MAX` |
| owner | 生命周期令牌：插件传自己的**实例指针**（与 `pi_plugin_qt_view_shutdown_owner` 同一约定）；`NULL` = 宿主自己，永不自动摘除 |
| 退订 | 显式 `unsubscribe` 可以；**忘记也没关系** —— 宿主 kit 在每个槽位卸载时调用 `drop_owner`，保证卸载后回调绝不再触发 |
| 尽力而为 | 可合并、可丢弃；`PI_OK` ≠ 已送达；丢弃必须可观测（路由器暴露 `dropped_full`） |
| 重入 | 在回调里 `publish` 合法，但由**下一次 pump** 投递，绝不递归 |

参照实现与验收：`host_kits/events/`（可选路由器 `PiPluginEventRouter`）、
`tests/test_host_events`（ctest `events_two_way_loop`）、
设计取舍见 `docs/design/events.md`。

## 3. 宿主管理 API（pi_plugin_host.h）

```c
PiPluginModule* pi_plugin_module_load(const char* path);                  /* 失败返回 NULL */
void            pi_plugin_module_unload(PiPluginModule* module);
const char*     pi_plugin_module_get_load_error(void);                    /* 上次失败的描述（**线程局部**） */
PiResult        pi_plugin_module_get_load_error_r(char* buf, size_t size); /* 拷进调用方缓冲（W-01） */

PiResult pi_plugin_module_get_factory(PiPluginModule* module, IPiPluginFactory** out_factory);

/* 便捷加载：load + get_factory + create_instance + initialize 一步完成 */
PiResult pi_plugin_host_create_plugin(const char* dll_path,
                               const PiGuid* class_guid,
                               IPiPluginHostServices* host,
                               IPiPluginBase** out_plugin,
                               PiPluginModule** out_module);      /* out_module 可 NULL（见下） */
```

> **加载错误串是线程局部的（W-01）**：并发宿主里每个线程各读各的那一条，不会
> 互相覆盖（旧实现是进程级 static，多线程下只剩"最后写入的那条"）。
> 两个入口：`pi_plugin_module_get_load_error()` 返回本线程当前那条（下次同线程 load 前
> 有效，永不为 NULL，"no error" 表示上次成功）；`pi_plugin_module_get_load_error_r()`
> 把它拷贝进调用方缓冲 —— 想**留存**失败原因时用它（拷贝不受后续 load 影响）。
> 缓冲为 NULL / size 为 0 返回 `PI_E_INVALIDARG`，放不下就截断（仍 NUL 结尾）。

> **注意**：DLL 卸载时机。模块必须存活到所有插件实例释放之后。`pi_plugin_host_create_plugin`
> 中若调用者传 `out_module == NULL`，为安全起见模块**故意不卸载**（接受泄漏），
> 因为卸载会使插件代码失效。请始终通过 `out_module` 取得模块并在释放插件后
> 调用 `pi_plugin_module_unload`。

## 4. UI 适配器套件 API

### 4.1 pi_imgui_view.h（imgui 套件）

插件作者只需提供回调：

```c
typedef void (*PiPluginImGuiInitProc)(void* user_data);    /* 可选：context 建立后调一次 */
typedef void (*PiPluginImGuiDrawProc)(void* user_data);    /* 必填：每帧绘制 */
typedef void (*PiPluginImGuiRetainProc)(void* user_data);  /* 可选：attach 时保活 user_data */
typedef void (*PiPluginImGuiReleaseProc)(void* user_data); /* 可选：detach 时释放 */

typedef struct PiPluginImGuiViewDesc {
    PiPluginImGuiInitProc    init;
    PiPluginImGuiDrawProc    draw;
    PiPluginImGuiRetainProc  retain;
    PiPluginImGuiReleaseProc release;
    void*              user_data;
} PiPluginImGuiViewDesc;

PiResult pi_plugin_imgui_view_create(const PiPluginImGuiViewDesc* desc, IPiPluginView** out_view);
```

契约：`pi_plugin_attach` / `pi_plugin_on_idle` / `pi_plugin_on_resize` / `pi_plugin_detach` 必须在**宿主 GUI 线程**调用。

### 4.2 pi_qt_view.h（Qt 套件）

```c
typedef QWidget* (*PiPluginQtCreateWidgetProc)(void* user_data);         /* 必填：Qt 线程调用 */
typedef void (*PiPluginQtDestroyWidgetProc)(void* user_data, QWidget* widget); /* 可选 */
typedef void (*PiPluginQtRetainProc)(void* user_data);                   /* 可选 */
typedef void (*PiPluginQtReleaseProc)(void* user_data);                  /* 可选 */

typedef struct PiPluginQtViewDesc {
    PiPluginQtCreateWidgetProc  create_widget;
    PiPluginQtDestroyWidgetProc destroy_widget;
    PiPluginQtRetainProc        retain;
    PiPluginQtReleaseProc       release;
    void*                 user_data;
} PiPluginQtViewDesc;

PiResult pi_plugin_qt_view_create(const PiPluginQtViewDesc* desc, IPiPluginView** out_view);
QWidget* pi_plugin_qt_view_widget(IPiPluginView* view);                       /* Qt 线程独占访问 */
void pi_plugin_qt_view_post(IPiPluginView* view, void (*fn)(void* user), void* user);
```

> **`pi_plugin_qt_view_post`（W-04）**：任意线程可调，回调**一定在宿主 GUI 线程上执行**
> —— 在 GUI 线程上调用就是内联执行；从别的线程调用则异步排队，由宿主下一次
> `pi_plugin_on_idle() -> processEvents()` 取出来执行（**不阻塞调用方**，也不引入第二条
> Qt 线程）。队列是尽力而为：视图 detach / 析构之后尚未执行的调用被丢弃，第一次
> attach 之前没有可 marshal 的 GUI 线程时同样丢弃（`PI_PLUGIN_QT_VIEW_TRACE=1` 有记录）。
> 回归用例：ctest `qt_view_post_from_worker_thread`（插件子线程调用，断言回调落在
> 宿主 GUI 线程上；改回内联执行时该用例稳定失败）。

## 5. 定义你自己的接口（特化协议，通道 A）

框架刻意只提供**机制**：任何 GUID 标识的接口都能被 QI、能力声明与宿主门禁处理。
因此 app 作者不需要 fork 框架，就能实现"**我生态内所有插件必须符合 XXX**"这类特化协议：

- 插件侧：实现你的接口，在 descriptor 里声明 `PI_PLUGIN_CAP_PROVIDES`；
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
伪 GUID（如 `0x00000021`）混进生态造成误读。`data1 = 0x00000000/01/02/03/10/11/20/30/31`
这九个已被占用，不要重复使用（完整表见 `architecture.md` §3，以那张表为准）。

> IID 一旦随 PUBLIC 版本发布就是 ABI 的一部分：**只能新增接口，不能修改已发布的 vtbl**。

**结果码同理**：你的协议方法返回 `PiResult` 时，自定义错误码不要占用框架的
`-1..-9`，取值 `<= -100`；若需要表达"已受理、稍后送达"的成功语义，用正值结果码
而不是负数。规则本体在 `include/piplugin/pi_plugin_types.h` 的 result codes 段。

### 5.2 第一步：app 定义协议（`my_app_protocol.h`）

```c
#include "piplugin/pi_plugin.h"

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
    IPiPluginHostServices* host;     /* add-ref'd */
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
        pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE) ||
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
    s_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;

    s_desc.name = "My Plugin";
    s_desc.vendor = "me";
    s_desc.version = "1.0.0";
    s_desc.category = "worker";
    s_desc.api_version = PI_PLUGIN_API_VERSION;
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
pi_plugin_host_session_create(host, &session);

/* 声明本 app 生态要求的能力：不满足的插件在 create_instance 之前就被拒绝 */
pi_plugin_host_session_require(session, &MY_APP_PROTOCOL_IID);

uint32_t slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
if (PI_FAILED(pi_plugin_host_session_load(session, "my_plugin.dll", &slot))) {
    /* 例如：plugin does not provide iid data1=0x9F3C1D42 required by this host */
    printf("%s\n", pi_plugin_host_session_last_error(session));
    return;
}

IPiPluginBase* plugin = pi_plugin_host_session_get_plugin(session, slot);   /* 借用 */
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
pi_plugin_factory_get_descriptor(factory, &desc);
if (!pi_plugin_descriptor_provides(desc, &MY_APP_PROTOCOL_IID))
    return;   /* 拒绝：本生态要求插件实现该接口 */
```

### 5.5 反向通道：宿主把自己的服务提供给插件

**已落地（APP-01）**。`PiPluginDefaultHost` 仍然只实现框架的自己三个 IID，但
`pi_plugin_host_services_create_ex()` 允许宿主再挂一个 extra-QI 钩子：框架 IID 之外的
`QueryInterface` 全部转交给宿主，由宿主决定认领哪些 app 自定义 IID。

```c
static const PiGuid MY_SERVICE_IID = /* 随机 128 位 UUID，见 5.1 */;

/* 与 QueryInterface 同契约：认领时 *out 必须是**已 add-ref** 的接口指针 */
static PiResult MyExtraQi(void* ctx, const PiGuid* iid, void** out)
{
    if (pi_guid_equal(iid, &MY_SERVICE_IID)) {
        *out = &((MyService*)ctx)->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

pi_plugin_host_services_create_ex(&MessageProc, NULL, window,
                           &MyExtraQi, &myService, &host);   /* NULL = 行为同 create_default */
```

插件侧**没有新 API**：对它 `pi_plugin_initialize()` 收到的那个宿主对象做一次普通的
`pi_query_interface(host, &MY_SERVICE_IID, &out)` 即可；宿主没提供就得到
`PI_E_NOINTERFACE`，插件按 `PI_PLUGIN_CAP_OPTIONAL` 的约定照常运行。于是通道 B 与通道 A
对称：一个是"宿主消费插件定义的接口"，一个是"插件消费宿主定义的服务"。

框架 API 的版本协商见 1.5；app 自定义接口的版本演进方式见 5.7。

### 5.6 三条禁令

1. **不改已发布的 vtbl**——要扩展就新增 IID 与接口（COM 规则）；
2. **vtbl 里不放 C++ 类型、不抛异常**——必须纯 C ABI，调用约定一律 `PI_CALL`，
   否则 Rust/C#/Python 的 FFI 承诺立刻失效；
3. **QI 必须返回 AddRef 过的指针**——宿主（以及框架的 session）一定会 release。

### 5.7 你的协议怎么演进版本

框架的 `api_version` 只管**框架 API 本身**，不覆盖 app 自定义的接口。两条规则：

1. **只增不改**：要给协议加方法，就新增一个 IID + 新接口。老插件不实现新接口，
   宿主 QI 失败即降级 —— 已发布的 vtbl 永远不动（COM 规则）。
2. **表达"我的协议是第几版"**，二选一：
   - 给协议再定义一个 IID 变体（如 `MY_APP_PROTOCOL_V2_IID`），宿主按自己支持的
     版本依次 QI；
   - 或用 descriptor 的自由 metadata 通道声明协议版本号（**已落地**，APP-04 的
     `properties` + `pi_plugin_descriptor_find_property()`，见 1.4）：插件写
     `myapp.protocol.version = "2"`，宿主按自己支持的版本决定要不要实例化。

宿主侧推荐做法：把"本生态要求哪几个协议 IID"全部交给 `pi_plugin_host_session_require()`
（见 5.4），不满足的插件在实例化之前就被拒绝。

### 5.8 通道 C：消息还是事件？

三条通道各有其位，别混用：

| 手段 | 方向 | 结构 | 何时用 |
|---|---|---|---|
| `pi_plugin_host_post_message(msg, w, l)` | 插件 → 宿主 | 三个整数 | 最原始的拉杆：少量、临时的信号；宿主自己解释编码 |
| `IPiPluginEventSink` + `pi_plugin_host_session_deliver_event()` | 宿主 → **指定**插件 | `PiPluginEvent`（type + topic + 键值） | 宿主知道该通知谁（"你的配置变了"）；**按地址投递** |
| `IPiPluginHostEvents`（publish / subscribe） | 双向，按 **topic** 扇出 | 同上 | 一对多、或"我不想关心谁在听"；插件之间通过宿主间接协作 |

选型要点：

- 目标明确、只有一个接收者 → sink；"谁关心谁来订阅" → topic；
- 事件是**尽力而为**的：不要用它传必须可靠送达的东西；可靠语义要另设计（现在没有）；
- 不要用 `pi.` 前缀发自己的 topic（框架保留区），也不要指望通配订阅（框架不定义语法）；
- 事件负载是键值字符串，天生可序列化 —— 这是为 `FUT-05`（跨进程插件）留的路。

## 6. 线程模型总结

| 组件 | 线程 |
|---|---|
| 宿主主循环 / `pi_plugin_on_idle` / `pi_plugin_on_resize` | 宿主 GUI 线程 |
| imgui 套件全部调用 | 宿主 GUI 线程（无内部线程） |
| Qt 套件全部调用（`create_widget` / 控件操作 / 生命周期） | 宿主 GUI 线程（**没有**私有后台线程，见 `adapters/qt/README.md`） |
| Qt 套件 `pi_plugin_qt_view_post` | 任意线程可调；回调在宿主 GUI 线程上执行（同线程内联，跨线程异步排队，W-04） |
| `pi_plugin_host_post_message` | 任意插件线程可调（宿主负责 marshal） |
| `pi_plugin_host_events_publish` | 任意线程可调（宿主 marshal 到自己的主线程） |
| `pi_plugin_event_deliver` / 事件订阅回调 | 宿主主/owner 线程（= 调用 `pi_plugin_initialize()` 的那条）；插件侧无需加锁 |
| `pi_plugin_host_events_subscribe` / `_unsubscribe` / `_drop_owner` | 宿主主线程 |
| 引用计数 | 原子操作，任何线程安全 |
| `pi_plugin_module_load` 的失败原因（`pi_plugin_module_get_load_error*`） | 线程局部（W-01）：每个线程读回**自己**那次 load 的结果 |

## 7. C++ RAII 层（可选，pi_cpp.h）

`pi_cpp.h` 是给 C++ 宿主/插件作者的语法糖，**不属于 C ABI**：它没有导出符号、
没有新增调用约定，只是包装既有 inline 帮助函数的模板。它**不**被 `pi_plugin.h`
包含（C 总入口保持纯 C），要用就显式 `#include "piplugin/pi_cpp.h"`。

| 名字 | 作用 |
|---|---|
| `PiPluginPtr<T>` | 持有接口指针的 RAII 句柄：析构 release、移动语义、拷贝被删除 |
| `PiPluginPtr<T>::add_ref(p)` | 给**借用**指针加一份引用，返回句柄 |
| `PiPluginPtr<T>::qi_to<U>()` | 按 `PiPluginIidOf<U>` 的 IID 做 QI；失败返回空句柄（不抛异常） |
| `PiPluginPtr<T>::put()` | 交给 C API 写出的出参地址（COM 的 receive 语义） |
| `PiPluginPtr<T>::detach()` | 交回引用，调用方负责 release |
| `PiPluginIidOf<T>` | 接口类型 → 框架 IID 的映射；自定义接口在自己头文件里补特化 |
| `PiPluginUniqueModule` | RAII `pi_plugin_module_unload`；`load(path, m)` + `factory()` |
| `pi_plugin_cpp_destroy<T>` | `pi_refcounted_init_with_destroy` 用的析构 thunk（跑 C++ 析构） |

**所有权语义（与 2.3 的冻结约定一一对应）**：`PiPluginPtr<T>` 的构造函数**接管**
（adopt）一个已有引用，不额外 AddRef —— 因为框架"返回接口指针"一律已经
AddRef 过（QI / factory / create_instance / get_view / create_default）。反过来
（构造函数再 AddRef）会让每个调用点都要补一次手工 release，那正是这层要消灭的
错误。借用指针（descriptor、native window）**禁止**包进 `PiPluginPtr`。

用法示例见 `docs/tutorial/write-plugin.md` §5 与 `docs/tutorial/write-host.md` §9；
行为与泄漏断言见 `tests/unit_cpp`（MSVC Debug 下用 `_CrtDumpMemoryLeaks()` 判定
"有没有泄漏"，因此"无泄漏"是一个可断言的退出码，而不是一句保证）。