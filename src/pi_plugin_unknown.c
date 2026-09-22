#include "pipluginframework/pi_plugin_unknown.h"
#include <stdlib.h>
#include <string.h>

#if PI_PLATFORM_WINDOWS
#  include <windows.h>
#  define PI_INTERLOCKED_INC(p) InterlockedIncrement((volatile LONG*)(p))
#  define PI_INTERLOCKED_DEC(p) InterlockedDecrement((volatile LONG*)(p))
#else
#  define PI_INTERLOCKED_INC(p) __sync_add_and_fetch((volatile uint32_t*)(p), 1)
#  define PI_INTERLOCKED_DEC(p) __sync_sub_and_fetch((volatile uint32_t*)(p), 1)
#endif

/* --------------------------------------------------------------------------
 * Interface GUIDs (defined here once)
 * -------------------------------------------------------------------------- */
PI_EXPORT const PiGuid PI_IID_UNKNOWN = PI_GUID(0x00000000, 0x0000, 0x0000,
                                      0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_EXPORT const PiGuid PI_IID_PLUGIN_FACTORY = PI_GUID(0x00000001, 0x0000, 0x0000,
                                             0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_EXPORT const PiGuid PI_IID_PLUGIN_BASE = PI_GUID(0x00000002, 0x0000, 0x0000,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_EXPORT const PiGuid PI_IID_PLUGIN_VIEW = PI_GUID(0x00000003, 0x0000, 0x0000,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
/* 1.1.0 additions: host services / UI capability / headless service */
PI_EXPORT const PiGuid PI_IID_HOST_SERVICES = PI_GUID(0x00000010, 0x0000, 0x0000,
                                            0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_EXPORT const PiGuid PI_IID_HOST_UI = PI_GUID(0x00000011, 0x0000, 0x0000,
                                       0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_EXPORT const PiGuid PI_IID_SERVICE = PI_GUID(0x00000020, 0x0000, 0x0000,
                                       0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

/* --------------------------------------------------------------------------
 * pi_guid_equal
 * -------------------------------------------------------------------------- */
PI_EXPORT int pi_guid_equal(const PiGuid* a, const PiGuid* b)
{
    if (!a || !b) return 0;
    return a->data1 == b->data1
        && a->data2 == b->data2
        && a->data3 == b->data3
        && memcmp(a->data4, b->data4, 8) == 0;
}

/* --------------------------------------------------------------------------
 * API version negotiation
 * -------------------------------------------------------------------------- */
PI_EXPORT int pi_api_version_compatible(uint32_t host_version, uint32_t plugin_version)
{
    /* 策略见 pi_plugin_types.h：major 必须相同，且插件不得高于宿主。
     * 同 major 时 plugin_version <= host_version 等价于 minor 比较。 */
    if (PI_API_VERSION_MAJOR(host_version) != PI_API_VERSION_MAJOR(plugin_version)) return 0;
    return plugin_version <= host_version;
}

/* --------------------------------------------------------------------------
 * Capability helpers
 * -------------------------------------------------------------------------- */
PI_EXPORT const PiPluginCapability* pi_descriptor_find_capability(
    const PiPluginDescriptor* desc, const PiGuid* iid)
{
    if (!desc || !iid || !desc->capabilities) return NULL;
    for (uint32_t i = 0; i < desc->capability_count; ++i) {
        if (pi_guid_equal(&desc->capabilities[i].iid, iid))
            return &desc->capabilities[i];
    }
    return NULL;
}

PI_EXPORT int pi_descriptor_provides(const PiPluginDescriptor* desc, const PiGuid* iid)
{
    const PiPluginCapability* cap = pi_descriptor_find_capability(desc, iid);
    return cap && (cap->flags & PI_CAP_PROVIDES);
}

PI_EXPORT int pi_descriptor_requires(const PiPluginDescriptor* desc, const PiGuid* iid)
{
    const PiPluginCapability* cap = pi_descriptor_find_capability(desc, iid);
    return cap && (cap->flags & PI_CAP_REQUIRED);
}

/* --------------------------------------------------------------------------
 * Refcounted base
 * -------------------------------------------------------------------------- */
PI_EXPORT void pi_refcounted_init(PiRefCountedBase* base, const IPiUnknownVtbl* vtbl)
{
    pi_refcounted_init_with_destroy(base, vtbl, NULL);
}

PI_EXPORT void pi_refcounted_init_with_destroy(PiRefCountedBase* base,
                                                const IPiUnknownVtbl* vtbl,
                                                PiDestroyProc destroy)
{
    if (!base) return;
    base->unk.lpVtbl = vtbl;
    base->ref_count = 1;
    base->destroy   = destroy;
}

PI_EXPORT uint32_t PI_CALL pi_refcounted_add_ref(void* this_ptr)
{
    PiRefCountedBase* base = (PiRefCountedBase*)this_ptr;
    if (!base) return 0;
    return (uint32_t)PI_INTERLOCKED_INC(&base->ref_count);
}

PI_EXPORT uint32_t PI_CALL pi_refcounted_release(void* this_ptr)
{
    PiRefCountedBase* base = (PiRefCountedBase*)this_ptr;
    if (!base) return 0;
    uint32_t ref = (uint32_t)PI_INTERLOCKED_DEC(&base->ref_count);
    if (ref == 0) {
        /* Invoke the owner's destroy callback instead of free():
         * objects may be allocated with any allocator (C++ new, host
         * allocator, static storage). destroy may be NULL. */
        PiDestroyProc destroy = base->destroy;
        if (destroy) destroy(this_ptr);
    }
    return ref;
}
