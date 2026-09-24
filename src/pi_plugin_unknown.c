#include <pibase/pi_base.h>
#include "piplugin/pi_plugin_types.h"
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Interface GUIDs (defined here once).
 *
 * PI_IID_UNKNOWN is deliberately absent: it identifies the root interface, so
 * the base layer defines it next to IPiUnknown.
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_PLUGIN_FACTORY = PI_GUID(0x00000001, 0x0000, 0x0000,
                                             0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_PLUGIN_BASE = PI_GUID(0x00000002, 0x0000, 0x0000,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_PLUGIN_VIEW = PI_GUID(0x00000003, 0x0000, 0x0000,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
/* 0.2 additions: host services / UI capability / headless service */
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_HOST_SERVICES = PI_GUID(0x00000010, 0x0000, 0x0000,
                                            0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_HOST_UI = PI_GUID(0x00000011, 0x0000, 0x0000,
                                       0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_SERVICE = PI_GUID(0x00000020, 0x0000, 0x0000,
                                       0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
/* 0.4 additions: events (channel C). Interfaces only - no existing vtbl moved. */
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_EVENT_SINK = PI_GUID(0x00000030, 0x0000, 0x0000,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_HOST_EVENTS = PI_GUID(0x00000031, 0x0000, 0x0000,
                                           0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

/* --------------------------------------------------------------------------
 * API version negotiation
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API int pi_plugin_api_version_compatible(uint32_t host_version, uint32_t plugin_version)
{
    /* 策略见 pi_plugin_types.h：major 必须相同，且插件不得高于宿主。
     * 同 major 时 plugin_version <= host_version 等价于 minor 比较。 */
    if (PI_PLUGIN_API_VERSION_MAJOR(host_version) != PI_PLUGIN_API_VERSION_MAJOR(plugin_version)) return 0;
    return plugin_version <= host_version;
}

/* --------------------------------------------------------------------------
 * Capability helpers
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API const PiPluginCapability* pi_plugin_descriptor_find_capability(
    const PiPluginDescriptor* desc, const PiGuid* iid)
{
    if (!desc || !iid || !desc->capabilities) return NULL;
    for (uint32_t i = 0; i < desc->capability_count; ++i) {
        if (pi_guid_equal(&desc->capabilities[i].iid, iid))
            return &desc->capabilities[i];
    }
    return NULL;
}

PI_PLUGIN_API int pi_plugin_descriptor_provides(const PiPluginDescriptor* desc, const PiGuid* iid)
{
    const PiPluginCapability* cap = pi_plugin_descriptor_find_capability(desc, iid);
    return cap && (cap->flags & PI_PLUGIN_CAP_PROVIDES);
}

PI_PLUGIN_API int pi_plugin_descriptor_requires(const PiPluginDescriptor* desc, const PiGuid* iid)
{
    const PiPluginCapability* cap = pi_plugin_descriptor_find_capability(desc, iid);
    return cap && (cap->flags & PI_PLUGIN_CAP_REQUIRED);
}

/* --------------------------------------------------------------------------
 * Descriptor properties (roadmap APP-04)
 *
 * The fields are APPENDED to PiPluginDescriptor in API 0.3, so a module built
 * against 0.2 has a shorter struct: reading `properties` out of it would read
 * past the end of the object it was compiled with. The version gate rejects a
 * plugin that is NEWER than the host, but it accepts an OLDER one (same major),
 * so this function has to decide for itself. The plugin's own api_version is
 * the layout discriminator - it is at the same offset in every layout, and the
 * 0.x rule is that the ABI may move with each x release. Follow the same pattern
 * when appending another field.
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API const char* pi_plugin_descriptor_find_property(const PiPluginDescriptor* desc,
                                                  const char* key)
{
    uint32_t i;
    if (!desc || !key) return NULL;
    if (PI_PLUGIN_API_VERSION_MINOR(desc->api_version) < 3) return NULL;   /* pre-0.3 layout */
    if (!desc->properties) return NULL;
    for (i = 0; i < desc->property_count; ++i) {
        const PiPluginProperty* prop = &desc->properties[i];
        if (!prop->key) continue;
        if (strcmp(prop->key, key) == 0) return prop->value;   /* 第一个命中者胜出 */
    }
    return NULL;
}

