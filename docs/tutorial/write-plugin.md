# 编写一个插件（Write a Plugin）

本文以 C++ 为例（参照 `tests/test_plugin`），介绍从零写一个 piplugin 插件所需的最小骨架。
插件 = 一个导出 `pi_plugin_entry` 的 DLL。

## 1. 最小骨架

一个插件由三部分组成：**工厂**（实现 `IPiPluginFactory`）、**插件对象**（实现 `IPiPluginBase`），以及**入口函数**。

### 1.1 入口函数

```cpp
// entry.cpp
#include "piplugin/pi_plugin.h"

extern "C" PI_PLUGIN_ENTRY_DECL   // 展开为 PI_PLUGIN_API PiResult pi_plugin_entry(IPiPluginFactory** out)
{
    if (!out_factory) return PI_E_INVALIDARG;
    MyPluginFactory* factory = new MyPluginFactory();
    if (!factory) return PI_E_OUTOFMEMORY;
    *out_factory = (IPiPluginFactory*)&factory->m_base;
    return PI_OK;
}
```

### 1.2 工厂（IPiPluginFactory）

```cpp
class MyPluginFactory {
public:
    PiRefCountedBase m_base;                  // 必须第一个成员
    static const IPiPluginFactoryVtbl s_factory_vtbl;

    MyPluginFactory() {
        pi_refcounted_init_with_destroy(&m_base,
            (const IPiUnknownVtbl*)&s_factory_vtbl,
            &pi_plugin_cpp_destroy<MyPluginFactory>);   // 归零时 delete this

        // 描述符先清零再填：它是"会追加可选字段"的结构（如 0.3 加的 properties），
        // 而这个对象在堆上，不清零的话那些字段就是垃圾值 —— 宿主读到垃圾
        // property_count 会去遍历垃圾 properties，然后崩在宿主自己里。
        pi_plugin_descriptor_init(&m_descriptor);

        m_descriptor.name = "My Plugin";
        m_descriptor.vendor = "Me";
        m_descriptor.version = "1.0.0";
        m_descriptor.category = "Demo";
        m_descriptor.api_version = PI_PLUGIN_API_VERSION;

        // 能力声明（LV2 风格）：提供视图，可选使用宿主 GUI
        m_capabilities[0] = { PI_PLUGIN_IID_PLUGIN_VIEW, PI_PLUGIN_CAP_PROVIDES };
        m_capabilities[1] = { PI_PLUGIN_IID_HOST_UI,     PI_PLUGIN_CAP_OPTIONAL };
        m_descriptor.capabilities = m_capabilities;
        m_descriptor.capability_count = 2;
    }

    // ---- vtable 槽位（静态函数，接受 this_ptr）----
    static uint32_t PI_CALL AddRef(void* s) { return pi_refcounted_add_ref(s); }
    static uint32_t PI_CALL Release(void* s){ return pi_refcounted_release(s); }
    static PiResult PI_CALL Qi(void* s, const PiGuid* iid, void** out) {
        if (!out) return PI_E_INVALIDARG;
        MyPluginFactory* me = (MyPluginFactory*)s;
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY)) {
            *out = me;
            me->m_base.unk.lpVtbl->pi_add_ref(s);
            return PI_OK;
        }
        *out = NULL; return PI_E_NOINTERFACE;
    }
    static const PiPluginDescriptor* PI_CALL GetDescriptor(void* s) {
        return &((MyPluginFactory*)s)->m_descriptor;
    }
    static uint32_t PI_CALL GetClassCount(void*) { return 1; }
    static PiResult PI_CALL GetClassGuid(void* s, uint32_t i, PiGuid* g) {
        (void)s; if (i != 0 || !g) return PI_E_INVALIDARG;
        *g = MY_CLASS_GUID; return PI_OK;
    }
    static PiResult PI_CALL CreateInstance(void* s, const PiGuid* guid,
                                           IPiPluginHostServices* host, IPiPluginBase** out) {
        (void)s;
        if (!guid || !out) return PI_E_INVALIDARG;
        if (!pi_guid_equal(guid, &MY_CLASS_GUID)) return PI_E_NOINTERFACE;
        MyPlugin* p = new MyPlugin();
        if (!p) return PI_E_OUTOFMEMORY;
        PiResult hr = p->Initialize(host);       // 或延迟到 pi_plugin_initialize
        if (PI_FAILED(hr)) { delete p; return hr; }
        *out = (IPiPluginBase*)&p->m_base;
        return PI_OK;
    }
};
```

> **`Initialize` 必须幂等**：框架的便捷加载 `pi_plugin_host_create_plugin()` 与宿主 kit L0
> 都在 `create_instance` 之后**再调一次** `pi_plugin_initialize()`（这是 1.2 生命周期里
> 写着的一步）。所以像上面那样在 `CreateInstance` 里就地初始化的插件会被初始化
> 两次 —— 第二次把同一个宿主指针再 add-ref 一遍，并且把第一次 QI 到的可选接口
> 包装直接覆盖掉（那一份引用就泄漏了）。加一行 `if (m_host) return PI_OK;` 即可。

### 1.3 插件对象（IPiPluginBase）

```cpp
class MyPlugin {
public:
    PiRefCountedBase m_base;                    // 必须第一个成员
    static const IPiPluginBaseVtbl s_base_vtbl;
    IPiPluginHostServices* m_host = NULL;
    IPiPluginHostUI*       m_hostUI = NULL;

    MyPlugin() {
        pi_refcounted_init_with_destroy(&m_base,
            (const IPiUnknownVtbl*)&s_base_vtbl, &pi_plugin_cpp_destroy<MyPlugin>);
    }
    ~MyPlugin() {                               // 释放 AddRef 过的宿主指针
        if (m_hostUI) pi_iunknown_release((IPiUnknown*)m_hostUI);
        if (m_host)   pi_iunknown_release((IPiUnknown*)m_host);
    }

    PiResult Initialize(IPiPluginHostServices* host) {
        if (m_host) return PI_OK;               // 幂等：宿主会再调一次 pi_plugin_initialize
        if (!host) return PI_OK;
        m_host = host;
        pi_iunknown_add_ref((IPiUnknown*)host);
        // 探测 GUI 宿主（可选能力）
        IPiPluginHostUI* ui = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host,
                        &PI_PLUGIN_IID_HOST_UI, (void**)&ui)))
            m_hostUI = ui;
        return PI_OK;
    }

    // ---- vtable 槽位 ----
    static PiResult PI_CALL Qi_Base(void* s, const PiGuid* iid, void** out) {
        if (!out) return PI_E_INVALIDARG;
        MyPlugin* me = (MyPlugin*)s;
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE)) {
            *out = me; me->m_base.unk.lpVtbl->pi_add_ref(s); return PI_OK;
        }
        *out = NULL; return PI_E_NOINTERFACE;
    }
    static PiResult PI_CALL Init(void* s, IPiPluginHostServices* host) {
        return ((MyPlugin*)s)->Initialize(host);
    }
    static PiResult PI_CALL Term(void* s) { (void)s; return PI_OK; }
    static PiResult PI_CALL GetView(void* s, IPiPluginView** out) {
        if (!out) return PI_E_INVALIDARG;
        MyPlugin* me = (MyPlugin*)s;
        if (!me->m_hostUI) { *out = NULL; return PI_E_NOINTERFACE; }  // 无头宿主不给视图
        // ... 通过 UI 适配器创建视图（见 adapters.md）
        return PI_E_NOINTERFACE;
    }
};
```

### 1.4 vtable 与 GUID

```cpp
static const PiGuid MY_CLASS_GUID =
    PI_GUID(0x7F83A000, 0x5C4D, 0x4E2A, 0x91, 0xD3, 0x8A, 0xFC, 0x2E, 0xB1, 0x44, 0x00);

const IPiPluginFactoryVtbl MyPluginFactory::s_factory_vtbl = {
    { &MyPluginFactory::Qi, &MyPluginFactory::AddRef, &MyPluginFactory::Release },
    &MyPluginFactory::GetDescriptor,
    &MyPluginFactory::GetClassCount,
    &MyPluginFactory::GetClassGuid,
    &MyPluginFactory::CreateInstance
};

const IPiPluginBaseVtbl MyPlugin::s_base_vtbl = {
    { &MyPlugin::Qi_Base, &pi_refcounted_add_ref, &pi_refcounted_release },
    &MyPlugin::Init, &MyPlugin::Term, &MyPlugin::GetView
};
```

## 2. CMake 最小配置

```cmake
cmake_minimum_required(VERSION 3.23)

set(TARGET_NAME my_plugin)
add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME} PRIVATE entry.cpp my_plugin.cpp my_plugin.h)
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)

# 若用到 UI 适配器：
# target_link_libraries(${TARGET_NAME} PRIVATE piplugin_imgui)  # 或 _qt
```

在你的工程树里，把这个目录 `add_subdirectory` 进去即可（测试工程参照 `tests/test_plugin*`）。

## 3. 关键约定

| 约定 | 说明 |
|---|---|
| 导出名 | `pi_plugin_entry`（`PI_PLUGIN_ENTRY_NAME`） |
| 对象布局 | `PiRefCountedBase` 必须是对象**第一个成员**（vtable 指针在偏移 0） |
| 引用计数 | 用 `pi_refcounted_*` 标准实现；C++ 对象 destroy 用 `pi_plugin_cpp_destroy<T>` |
| `QueryInterface` | 必须支持对应 IID 与 `PI_IID_UNKNOWN`，且返回时已 AddRef |
| 宿主指针 | 保存宿主对象时要 AddRef；析构时 Release |
| 无头宿主 | `m_hostUI == NULL` → 不创建视图，`pi_plugin_get_view` 返回 `PI_E_NOINTERFACE` |

## 4. 能力声明技巧

- **GUI 插件**：`PI_PLUGIN_IID_PLUGIN_VIEW (PROVIDES)` + `PI_PLUGIN_IID_HOST_UI (OPTIONAL)`。
- **纯服务插件（任务服务器）**：`PI_PLUGIN_IID_SERVICE (PROVIDES)`，不依赖任何宿主能力。
- **硬性 GUI 需求**：`PI_PLUGIN_IID_HOST_UI (REQUIRED)` —— headless 宿主会在实例化前直接拒绝。

宿主侧（如 `pi_plugin_test_host.imgui`）会在 `pi_plugin_factory_create_instance` **之前**调用
`pi_plugin_descriptor_requires(desc, &PI_PLUGIN_IID_HOST_UI)` 做能力门检查，请务必如实声明。

### 4.1 描述性元数据：properties（APP-04）

能力（capabilities）回答"这个插件在框架词汇里能做什么"；描述性事实应该走
descriptor 的键值对，而不是滥用一个 GUID：

```c
static PiPluginProperty s_props[2];
...
s_props[0].key = "com.example.ui.toolkit";   s_props[0].value = "qt5";
s_props[1].key = "com.example.file_formats"; s_props[1].value = "png,jpg";
s_desc.properties     = s_props;
s_desc.property_count = 2;
```

宿主侧按键取值：

```c
const char* toolkit = pi_plugin_descriptor_find_property(desc, "com.example.ui.toolkit");
if (toolkit) printf("this plugin's UI is built with %s\n", toolkit);
```

约定：

- 键值都是 UTF-8、NUL 结尾；`pi.` 前缀是**框架保留区**，你自己用 `com.example.*`
  这类自有前缀；
- 键按字节比较（大小写敏感），重复键取第一个；没有该属性返回 NULL；
- 0.2 编译的插件结构体里没有这两个字段，`pi_plugin_descriptor_find_property()` 会按插件声明的
  `api_version` 判定布局，对老插件直接回答"没有属性"（不会读越界内存）。

## 5. 可选：C++ RAII 层（pi_cpp.h）

C++ 插件可以少写一半引用计数样板：

```cpp
#include "piplugin/pi_cpp.h"           /* C++ 糖，不包含在 pi_plugin.h 里 */

class MyPlugin {
    PiPluginPtr<IPiPluginHostServices> m_host;     /* 借用入参 -> add_ref，析构自动 release */
    PiPluginPtr<IPiPluginHostUI>       m_hostUI;   /* 宿主没有 UI 时就是空句柄 */
    ...
    PiResult Initialize(IPiPluginHostServices* host) {
        if (m_host) return PI_OK;                       /* 幂等，见 1.3 的说明 */
        m_host   = PiPluginPtr<IPiPluginHostServices>::add_ref(host);  /* 借用 -> 自己持有 */
        m_hostUI = m_host.qi_to<IPiPluginHostUI>();            /* 失败 = 空句柄 = headless */
        return PI_OK;
    }
    // 析构函数里一行 release 都不用写
};
```

- `PiPluginPtr<T>` **接管**一个已有引用（框架"返回接口指针"一律已 AddRef，见
  interfaces.md 2.3），所以每个 QI 调用点不需要再补一次 release；
  借用指针（descriptor / native window）**禁止**包进去；
- 拷贝被删除，需要第二份持有就写 `PiPluginPtr<T>::add_ref(p)`；
- `qi_to<T>()` 用 `PiPluginIidOf<T>` 里的框架 IID；自定义接口在自己的头文件里补一个
  特化，或者 `qi_to<T>(my_iid)` 显式给 IID；
- 宿主侧的模块加载用 `PiPluginUniqueModule`（RAII `pi_plugin_module_unload`）；
- 完整语义与理由见头文件注释，测试见 `tests/unit_cpp`（含 Debug CRT 泄漏判定）。

## 6. 可选：事件（IPiPluginEventSink / IPiPluginHostEvents，API 0.4）

插件有两种参与事件的方式，各自独立、都能缺席：

**① 收：实现 `IPiPluginEventSink`**（宿主按地址投递给你）

```c
static PiResult PI_CALL Sink_Deliver(void* self_ptr, const PiPluginEvent* event)
{
    MyPlugin* me = ((MySink*)self_ptr)->owner;
    if (event->topic && strcmp(event->topic, "com.example.host.welcome") == 0) {
        const char* greeting = NULL;                       /* 读键值负载 */
        for (uint32_t i = 0; i < event->payload_count; ++i)
            if (strcmp(event->payload[i].key, "greeting") == 0) greeting = event->payload[i].value;
        (void)greeting;
        return PI_OK;                                      /* 已处理 */
    }
    return PI_E_NOTIMPL;      /* 不关心这个 topic —— 这不是错误，宿主会跳过 */
}
```

在 descriptor 里声明 `PI_PLUGIN_IID_EVENT_SINK` 为 `PI_PLUGIN_CAP_PROVIDES`，
并像 `IPiPluginService` 那样在 `pi_query_interface` 里交出这个接口
（不同接口要有自己的 vtbl 槽位，参照 `tests/test_plugin_service` 与
`tests/test_plugin_events` 的 wrapper 写法）。

**② 发/订阅：QI 宿主的 `IPiPluginHostEvents`**

```c
/* initialize 里 */
if (PI_SUCCEEDED(pi_plugin_host_events_query(host, &me->host_events))) {
    PiPluginEvent ev = { 0 };
    ev.type = PI_PLUGIN_EVENT_NOTIFY; ev.topic = "com.example.plugin.ready";
    pi_plugin_host_events_publish(me->host_events, &ev);          /* 任意线程可发 */

    /* owner = 自己的实例指针：卸载时宿主按它兜底退订，忘记退订也不会回调 */
    pi_plugin_host_events_subscribe(me->host_events, "com.example.host.broadcast",
                             (void*)me, &OnBroadcast, me, &me->subscription);
}
```

要点：

- **任何返回码都不是宿主的错误源**：`PI_E_NOTIMPL` / `PI_FAIL` 都只是你的表态；
- sink 与订阅回调都在**宿主主线程**上跑（= `pi_plugin_initialize()` 那条线程），
  所以可以直接碰 UI/句柄，**不需要锁**；但不要阻塞太久；
- 事件与负载字符串**只在本次调用期间有效**（借用），要留就自己拷贝；
- 事件是**尽力而为**的：可能丢、可能合并，不要拿它当可靠投递；
- 主题是**字面名字**（`pi.` 前缀留给框架），框架不定义通配语法；
- 只想"通知宿主一声"、内容很少？`pi_plugin_host_post_message()` 仍然够用（见 §1.3）。