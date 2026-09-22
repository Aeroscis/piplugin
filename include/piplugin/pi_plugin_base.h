/*
 * piplugin - IPiPluginBase
 *
 * Base interface for every plugin instance.
 * Provides lifecycle management (initialize / terminate) and
 * the bridge to obtain a GUI view.
 */
#ifndef PI_PLUGIN_BASE_H
#define PI_PLUGIN_BASE_H

#include "pi_plugin_unknown.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward */
typedef struct IPiPluginView IPiPluginView;

/* --------------------------------------------------------------------------
 * IPiPluginBaseVtbl
 * -------------------------------------------------------------------------- */
typedef struct IPiPluginBaseVtbl {
    /* Inherited from IPiUnknown */
    IPiUnknownVtbl base;

    /* Initialize the plugin with the host services object.
     * Called once after instantiation. The plugin may QueryInterface the
     * host for optional capabilities such as IPiHostUI (GUI hosts only). */
    PiResult (PI_CALL *pi_initialize)(void* this_ptr, IPiHostServices* host);

    /* Terminate the plugin.
     * Called before destruction. Plugin must release resources. */
    PiResult (PI_CALL *pi_terminate)(void* this_ptr);

    /* Get the plugin's GUI view, if any.
     *   out  — receives IPiPluginView (caller must ->release())
     * Returns PI_OK on success, PI_E_NOINTERFACE if this plugin has no GUI. */
    PiResult (PI_CALL *pi_get_view)(void* this_ptr, IPiPluginView** out);
} IPiPluginBaseVtbl;

typedef struct IPiPluginBase {
    const IPiPluginBaseVtbl* lpVtbl;
} IPiPluginBase;

/* --------------------------------------------------------------------------
 * Inline helpers
 * -------------------------------------------------------------------------- */
static inline PiResult pi_plugin_initialize(IPiPluginBase* self,
                                             IPiHostServices* host) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_initialize) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_initialize((void*)self, host);
}

static inline PiResult pi_plugin_terminate(IPiPluginBase* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_terminate) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_terminate((void*)self);
}

static inline PiResult pi_plugin_get_view(IPiPluginBase* self, IPiPluginView** out) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_get_view) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_get_view((void*)self, out);
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_BASE_H */
