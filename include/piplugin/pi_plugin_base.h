/*
 * piplugin - IPiPluginBase
 *
 * Base interface for every plugin instance. Lifecycle only:
 * initialize / terminate.
 *
 * A plugin's GUI is NOT part of this interface. Views are an optional
 * capability the host discovers by QueryInterface(PI_PLUGIN_IID_VIEW_PROVIDER)
 * and are handed out by IPiPluginViewProvider (pi_plugin_view.h). That split has
 * two consequences worth stating up front:
 *
 *   - a headless plugin never sees the word "view": it simply does not expose
 *     the provider, and the host's query misses. Before 0.6 it had to implement
 *     pi_plugin_get_view and return PI_E_NOINTERFACE from it, which is the one
 *     place in this framework where "I do not have this capability" was not
 *     expressed by a failing query;
 *   - "how many views, and which ones" belongs to the view interface, so a
 *     plugin with one view and a plugin with ten answer the framework through
 *     the same shape.
 */
#ifndef PI_PLUGIN_BASE_H
#define PI_PLUGIN_BASE_H

#include <pibase/pi_base.h>

#include "pi_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * IPiPluginBaseVtbl
 * -------------------------------------------------------------------------- */
typedef struct IPiPluginBaseVtbl {
    /* Inherited from IPiUnknown */
    IPiUnknownVtbl base;

    /* Initialize the plugin with the host services object.
     * Called once after instantiation. The plugin may QueryInterface the
     * host for optional capabilities such as IPiPluginHostUI (GUI hosts only). */
    PiResult(PI_CALL* pi_plugin_initialize)(void* this_ptr, IPiPluginHostServices* host);

    /* Terminate the plugin.
     *
     * Called once, before destruction, after every view this plugin handed out
     * has been detached and released. A plugin that started threads or owns
     * top-level windows must have joined those threads and destroyed those
     * windows before returning: the module is unmapped right after this
     * returns, and any code still running from it is a crash, not a warning. */
    PiResult(PI_CALL* pi_plugin_terminate)(void* this_ptr);
} IPiPluginBaseVtbl;

typedef struct IPiPluginBase {
    IPiPluginBaseVtbl const* lpVtbl;
} IPiPluginBase;

/* --------------------------------------------------------------------------
 * Inline helpers
 * -------------------------------------------------------------------------- */
static inline PiResult pi_plugin_initialize(IPiPluginBase*         self,
                                            IPiPluginHostServices* host)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_initialize)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_initialize((void*)self, host);
}

static inline PiResult pi_plugin_terminate(IPiPluginBase* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_terminate)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_terminate((void*)self);
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_BASE_H */
