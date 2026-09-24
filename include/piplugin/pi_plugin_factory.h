/*
 * piplugin - IPiPluginFactory
 *
 * Every plugin DLL exports pi_plugin_entry(), which returns an
 * IPiPluginFactory. The host uses the factory to:
 *   1. Query plugin metadata (name, version, etc.)
 *   2. Enumerate supported plugin classes
 *   3. Instantiate plugin instances
 */
#ifndef PI_PLUGIN_FACTORY_H
#define PI_PLUGIN_FACTORY_H

#include <pibase/pi_base.h>

#include "pi_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * IPiPluginFactoryVtbl
 * -------------------------------------------------------------------------- */
typedef struct IPiPluginFactoryVtbl {
    /* Inherited from IPiUnknown */
    IPiUnknownVtbl base;

    /* Get the plugin descriptor. Caller does not free. */
    PiPluginDescriptor const*(PI_CALL* pi_plugin_get_descriptor)(void* this_ptr);

    /* Number of plugin classes this factory can create */
    uint32_t(PI_CALL* pi_plugin_get_class_count)(void* this_ptr);

    /* Get the class GUID at the given index.
     *   index — 0-based index
     *   guid  — receives the class GUID
     * Returns PI_OK or PI_E_INVALIDARG. */
    PiResult(PI_CALL* pi_plugin_get_class_guid)(void* this_ptr, uint32_t index, PiGuid* guid);

    /* Create an instance of the plugin class identified by guid.
     *   guid — class GUID (from pi_plugin_get_class_guid)
     *   host — host services object for the new instance (may be NULL)
     *   out — receives IPiPluginBase (caller must ->release())
     * Returns PI_OK on success. The host object is owned by the caller;
     * the plugin must AddRef it if it keeps the pointer. */
    PiResult(PI_CALL* pi_plugin_create_instance)(void* this_ptr, PiGuid const* guid,
                                                 IPiPluginHostServices* host,
                                                 IPiPluginBase**        out);
} IPiPluginFactoryVtbl;

typedef struct IPiPluginFactory {
    IPiPluginFactoryVtbl const* lpVtbl;
} IPiPluginFactory;

/* --------------------------------------------------------------------------
 * Inline helpers
 * -------------------------------------------------------------------------- */
static inline PiResult pi_plugin_factory_get_descriptor(IPiPluginFactory*          self,
                                                        PiPluginDescriptor const** desc)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_descriptor || !desc)
    {
        return PI_E_INVALIDARG;
    }
    *desc = self->lpVtbl->pi_plugin_get_descriptor((void*)self);
    return PI_OK;
}

static inline uint32_t pi_plugin_factory_get_class_count(IPiPluginFactory* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_class_count)
    {
        return 0;
    }
    return self->lpVtbl->pi_plugin_get_class_count((void*)self);
}

static inline PiResult pi_plugin_factory_get_class_guid(IPiPluginFactory* self,
                                                        uint32_t index, PiGuid* guid)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_class_guid)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_get_class_guid((void*)self, index, guid);
}

static inline PiResult pi_plugin_factory_create_instance(IPiPluginFactory*      self,
                                                         PiGuid const*          guid,
                                                         IPiPluginHostServices* host,
                                                         IPiPluginBase**        out)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_create_instance)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_create_instance((void*)self, guid, host, out);
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_FACTORY_H */
