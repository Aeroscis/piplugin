/*
 * piplugin tests - reference implementation of the app-defined host service
 * declared in pi_test_host_service.h (roadmap APP-01).
 *
 * Header-only and written in plain C so the C headless host and the C++ imgui
 * host share ONE implementation instead of each writing their own: the test
 * suite should not teach two different ways of exposing a host service.
 *
 * Lifetime: the host owns the object (static storage is fine - the refcount
 * starts at 1 and there is no destroy callback, so it is never freed). Every
 * reference a plugin obtains through QI is a normal refcounted reference that
 * the plugin releases.
 *
 * Threading: the counters are plain uint32_t. The test hosts call the service
 * from the thread the plugin queries on, and all test hosts drive their
 * plugins from a single thread; a real app exposing a service to plugins on
 * several threads would need atomics here.
 */
#ifndef PI_PLUGIN_TEST_HOST_SERVICE_IMPL_H
#define PI_PLUGIN_TEST_HOST_SERVICE_IMPL_H

#include "pi_test_host_service.h"

typedef struct PiPluginTestHostServiceImpl {
    PiRefCountedBase base;           /* must be FIRST member */
    const char*      name;
    uint32_t         name_calls;     /* 宿主侧计数：插件调了几次 name() */
    const uint32_t*  messages_seen;  /* 借用宿主自己的"收到多少条插件消息"计数 */
} PiPluginTestHostServiceImpl;

static inline uint32_t PI_CALL PiPluginTestHostServiceImpl_AddRef(void* self_ptr);
static inline uint32_t PI_CALL PiPluginTestHostServiceImpl_Release(void* self_ptr);
static inline PiResult PI_CALL PiPluginTestHostServiceImpl_Qi(void* self_ptr,
                                                        const PiGuid* iid, void** out);
static inline const char* PI_CALL PiPluginTestHostServiceImpl_Name(void* self_ptr);
static inline uint32_t PI_CALL PiPluginTestHostServiceImpl_MessagesSeen(void* self_ptr);

static const IPiPluginTestHostServiceVtbl PI_PLUGIN_TEST_HOST_SERVICE_VTBL = {
    { &PiPluginTestHostServiceImpl_Qi,
      &PiPluginTestHostServiceImpl_AddRef,
      &PiPluginTestHostServiceImpl_Release },
    &PiPluginTestHostServiceImpl_Name,
    &PiPluginTestHostServiceImpl_MessagesSeen
};

/* 框架导出的 pi_refcounted_add_ref/release 直接填 vtbl 会在 C 里触发
 * warning C4232（取 dllimport 函数地址，不保证跨模块标识）—— 包一层薄封装，
 * 与本仓库其它实现（src/pi_plugin_host.c、单测）保持同一写法。 */
static inline uint32_t PI_CALL PiPluginTestHostServiceImpl_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}

static inline uint32_t PI_CALL PiPluginTestHostServiceImpl_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static inline PiResult PI_CALL PiPluginTestHostServiceImpl_Qi(void* self_ptr,
                                                        const PiGuid* iid, void** out)
{
    PiPluginTestHostServiceImpl* me = (PiPluginTestHostServiceImpl*)self_ptr;
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_PLUGIN_TEST_IID_HOST_SERVICE)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static inline const char* PI_CALL PiPluginTestHostServiceImpl_Name(void* self_ptr)
{
    PiPluginTestHostServiceImpl* me = (PiPluginTestHostServiceImpl*)self_ptr;
    if (!me) return "";
    ++me->name_calls;
    return me->name ? me->name : "";
}

static inline uint32_t PI_CALL PiPluginTestHostServiceImpl_MessagesSeen(void* self_ptr)
{
    PiPluginTestHostServiceImpl* me = (PiPluginTestHostServiceImpl*)self_ptr;
    if (!me || !me->messages_seen) return 0;
    return *me->messages_seen;
}

/* 初始化：宿主在创建 IPiPluginHostServices 之前调用一次。`messages_seen` 指向宿主
 * 自己的消息计数器（借用指针），本实现只读它。 */
static inline void PiPluginTestHostServiceImpl_Init(PiPluginTestHostServiceImpl* self,
                                              const char* name,
                                              const uint32_t* messages_seen)
{
    if (!self) return;
    pi_refcounted_init(&self->base, (const IPiUnknownVtbl*)&PI_PLUGIN_TEST_HOST_SERVICE_VTBL);
    self->name          = name;
    self->name_calls    = 0;
    self->messages_seen = messages_seen;
}

/* 交给 pi_plugin_host_services_create_ex() 的 extra_qi 钩子。
 *
 * 注意签名：PiPluginHostExtraQiProc 与 PiPluginHostMessageProc 一样是**普通 cdecl** 回调
 * typedef（不是 vtbl 槽位，故没有 PI_CALL）—— 与框架既有回调 typedef 一致。 */
static inline PiResult PiPluginTestHostServiceImpl_ExtraQi(void* ctx, const PiGuid* iid, void** out)
{
    PiPluginTestHostServiceImpl* me = (PiPluginTestHostServiceImpl*)ctx;
    if (!out) return PI_E_INVALIDARG;
    if (me && pi_guid_equal(iid, &PI_PLUGIN_TEST_IID_HOST_SERVICE)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

#endif /* PI_PLUGIN_TEST_HOST_SERVICE_IMPL_H */
