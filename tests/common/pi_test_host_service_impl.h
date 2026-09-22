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
#ifndef PI_TEST_HOST_SERVICE_IMPL_H
#define PI_TEST_HOST_SERVICE_IMPL_H

#include "pi_test_host_service.h"

typedef struct PiTestHostServiceImpl {
    PiRefCountedBase base;           /* must be FIRST member */
    const char*      name;
    uint32_t         name_calls;     /* 宿主侧计数：插件调了几次 name() */
    const uint32_t*  messages_seen;  /* 借用宿主自己的"收到多少条插件消息"计数 */
} PiTestHostServiceImpl;

static inline uint32_t PI_CALL PiTestHostServiceImpl_AddRef(void* self_ptr);
static inline uint32_t PI_CALL PiTestHostServiceImpl_Release(void* self_ptr);
static inline PiResult PI_CALL PiTestHostServiceImpl_Qi(void* self_ptr,
                                                        const PiGuid* iid, void** out);
static inline const char* PI_CALL PiTestHostServiceImpl_Name(void* self_ptr);
static inline uint32_t PI_CALL PiTestHostServiceImpl_MessagesSeen(void* self_ptr);

static const IPiTestHostServiceVtbl PI_TEST_HOST_SERVICE_VTBL = {
    { &PiTestHostServiceImpl_Qi,
      &PiTestHostServiceImpl_AddRef,
      &PiTestHostServiceImpl_Release },
    &PiTestHostServiceImpl_Name,
    &PiTestHostServiceImpl_MessagesSeen
};

/* 框架导出的 pi_refcounted_add_ref/release 直接填 vtbl 会在 C 里触发
 * warning C4232（取 dllimport 函数地址，不保证跨模块标识）—— 包一层薄封装，
 * 与本仓库其它实现（src/pi_plugin_host.c、单测）保持同一写法。 */
static inline uint32_t PI_CALL PiTestHostServiceImpl_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}

static inline uint32_t PI_CALL PiTestHostServiceImpl_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static inline PiResult PI_CALL PiTestHostServiceImpl_Qi(void* self_ptr,
                                                        const PiGuid* iid, void** out)
{
    PiTestHostServiceImpl* me = (PiTestHostServiceImpl*)self_ptr;
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_TEST_IID_HOST_SERVICE)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static inline const char* PI_CALL PiTestHostServiceImpl_Name(void* self_ptr)
{
    PiTestHostServiceImpl* me = (PiTestHostServiceImpl*)self_ptr;
    if (!me) return "";
    ++me->name_calls;
    return me->name ? me->name : "";
}

static inline uint32_t PI_CALL PiTestHostServiceImpl_MessagesSeen(void* self_ptr)
{
    PiTestHostServiceImpl* me = (PiTestHostServiceImpl*)self_ptr;
    if (!me || !me->messages_seen) return 0;
    return *me->messages_seen;
}

/* 初始化：宿主在创建 IPiHostServices 之前调用一次。`messages_seen` 指向宿主
 * 自己的消息计数器（借用指针），本实现只读它。 */
static inline void PiTestHostServiceImpl_Init(PiTestHostServiceImpl* self,
                                              const char* name,
                                              const uint32_t* messages_seen)
{
    if (!self) return;
    pi_refcounted_init(&self->base, (const IPiUnknownVtbl*)&PI_TEST_HOST_SERVICE_VTBL);
    self->name          = name;
    self->name_calls    = 0;
    self->messages_seen = messages_seen;
}

/* 交给 pi_host_services_create_ex() 的 extra_qi 钩子。
 *
 * 注意签名：PiHostExtraQiProc 与 PiHostMessageProc 一样是**普通 cdecl** 回调
 * typedef（不是 vtbl 槽位，故没有 PI_CALL）—— 与框架既有回调 typedef 一致。 */
static inline PiResult PiTestHostServiceImpl_ExtraQi(void* ctx, const PiGuid* iid, void** out)
{
    PiTestHostServiceImpl* me = (PiTestHostServiceImpl*)ctx;
    if (!out) return PI_E_INVALIDARG;
    if (me && pi_guid_equal(iid, &PI_TEST_IID_HOST_SERVICE)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

#endif /* PI_TEST_HOST_SERVICE_IMPL_H */
