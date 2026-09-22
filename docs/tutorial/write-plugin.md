# 编写一个插件（Write a Plugin）

本文以 C++ 为例（参照 `tests/test_plugin`），介绍从零写一个 piplugin 插件所需的最小骨架。
插件 = 一个导出 `pi_plugin_entry` 的 DLL。

## 1. 最小骨架

一个插件由三部分组成：**工厂**（实现 `IPiPluginFactory`）、**插件对象**（实现 `IPiPluginBase`），以及**入口函数**。

### 1.1 入口函数

```cpp
// entry.cpp
#include "piplugin/pi_plugin.h"

extern "C" PI_PLUGIN_ENTRY_DECL   // 展开为 PI_EXPORT PiResult pi_plugin_entry(IPiPluginFactory** out)
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
            &pi_cpp_destroy<MyPluginFactory>);   // 归零时 delete this

        m_descriptor.name = "My Plugin";
        m_descriptor.vendor = "Me";
        m_descriptor.version = "1.0.0";
        m_descriptor.category = "Demo";
        m_descriptor.api_version = PIPLUGIN_API_VERSION;

        // 能力声明（LV2 风格）：提供视图，可选使用宿主 GUI
        m_capabilities[0] = { PI_IID_PLUGIN_VIEW, PI_CAP_PROVIDES };
        m_capabilities[1] = { PI_IID_HOST_UI,     PI_CAP_OPTIONAL };
        m_descriptor.capabilities = m_capabilities;
        m_descriptor.capability_count = 2;
    }

    // ---- vtable 槽位（静态函数，接受 this_ptr）----
    static uint32_t PI_CALL AddRef(void* s) { return pi_refcounted_add_ref(s); }
    static uint32_t PI_CALL Release(void* s){ return pi_refcounted_release(s); }
    static PiResult PI_CALL Qi(void* s, const PiGuid* iid, void** out) {
        if (!out) return PI_E_INVALIDARG;
        MyPluginFactory* me = (MyPluginFactory*)s;
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_FACTORY)) {
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
                                           IPiHostServices* host, IPiPluginBase** out) {
        (void)s;
        if (!guid || !out) return PI_E_INVALIDARG;
        if (!pi_guid_equal(guid, &MY_CLASS_GUID)) return PI_E_NOINTERFACE;
        MyPlugin* p = new MyPlugin();
        if (!p) return PI_E_OUTOFMEMORY;
        PiResult hr = p->Initialize(host);       // 或延迟到 pi_initialize
        if (PI_FAILED(hr)) { delete p; return hr; }
        *out = (IPiPluginBase*)&p->m_base;
        return PI_OK;
    }
};
```

### 1.3 插件对象（IPiPluginBase）

```cpp
class MyPlugin {
public:
    PiRefCountedBase m_base;                    // 必须第一个成员
    static const IPiPluginBaseVtbl s_base_vtbl;
    IPiHostServices* m_host = NULL;
    IPiHostUI*       m_hostUI = NULL;

    MyPlugin() {
        pi_refcounted_init_with_destroy(&m_base,
            (const IPiUnknownVtbl*)&s_base_vtbl, &pi_cpp_destroy<MyPlugin>);
    }
    ~MyPlugin() {                               // 释放 AddRef 过的宿主指针
        if (m_hostUI) pi_iunknown_release((IPiUnknown*)m_hostUI);
        if (m_host)   pi_iunknown_release((IPiUnknown*)m_host);
    }

    PiResult Initialize(IPiHostServices* host) {
        if (!host) return PI_OK;
        m_host = host;
        pi_iunknown_add_ref((IPiUnknown*)host);
        // 探测 GUI 宿主（可选能力）
        IPiHostUI* ui = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host,
                        &PI_IID_HOST_UI, (void**)&ui)))
            m_hostUI = ui;
        return PI_OK;
    }

    // ---- vtable 槽位 ----
    static PiResult PI_CALL Qi_Base(void* s, const PiGuid* iid, void** out) {
        if (!out) return PI_E_INVALIDARG;
        MyPlugin* me = (MyPlugin*)s;
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_BASE)) {
            *out = me; me->m_base.unk.lpVtbl->pi_add_ref(s); return PI_OK;
        }
        *out = NULL; return PI_E_NOINTERFACE;
    }
    static PiResult PI_CALL Init(void* s, IPiHostServices* host) {
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
| 引用计数 | 用 `pi_refcounted_*` 标准实现；C++ 对象 destroy 用 `pi_cpp_destroy<T>` |
| `QueryInterface` | 必须支持对应 IID 与 `PI_IID_UNKNOWN`，且返回时已 AddRef |
| 宿主指针 | 保存宿主对象时要 AddRef；析构时 Release |
| 无头宿主 | `m_hostUI == NULL` → 不创建视图，`pi_get_view` 返回 `PI_E_NOINTERFACE` |

## 4. 能力声明技巧

- **GUI 插件**：`PI_IID_PLUGIN_VIEW (PROVIDES)` + `PI_IID_HOST_UI (OPTIONAL)`。
- **纯服务插件（任务服务器）**：`PI_IID_SERVICE (PROVIDES)`，不依赖任何宿主能力。
- **硬性 GUI 需求**：`PI_IID_HOST_UI (REQUIRED)` —— headless 宿主会在实例化前直接拒绝。

宿主侧（如 `pi_test_host.imgui`）会在 `pi_factory_create_instance` **之前**调用
`pi_descriptor_requires(desc, &PI_IID_HOST_UI)` 做能力门检查，请务必如实声明。