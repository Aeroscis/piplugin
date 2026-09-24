/*
 * piplugin - IPiPluginView
 *
 * GUI view interface that allows a plugin to render inside the host window.
 * The host embeds the plugin's native window handle, and can pump the
 * plugin's event loop via pi_plugin_on_idle().
 *
 * Design rationale for cross-framework GUI compatibility:
 *   1. Plugin creates its own native window (HWND/NSView/X11 Window)
 *   2. Plugin returns the native handle — host embeds it as a child
 *   3. Plugin runs its event loop in its own thread OR
 *      the host calls pi_plugin_on_idle() every frame to pump plugin events
 *   4. pi_plugin_on_resize() allows the host to notify the plugin of size changes
 *
 * This lets the plugin use Qt, GTK, raw Win32, or any other GUI toolkit
 * independently of what the host uses (imgui, wxWidgets, etc.).
 */
#ifndef PI_PLUGIN_VIEW_H
#define PI_PLUGIN_VIEW_H

#include "pi_plugin_types.h"
#include <pibase/pi_base.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IPiPluginViewVtbl {
    IPiUnknownVtbl base;

    PiResult (PI_CALL *pi_plugin_attach)(void* this_ptr, PiNativeWindow parent_window);

    PiResult (PI_CALL *pi_plugin_detach)(void* this_ptr);

    PiNativeWindow (PI_CALL *pi_plugin_get_native_window)(void* this_ptr);

    PiResult (PI_CALL *pi_plugin_on_resize)(void* this_ptr, int32_t width, int32_t height);

    PiResult (PI_CALL *pi_plugin_on_idle)(void* this_ptr);

    PiResult (PI_CALL *pi_plugin_get_preferred_size)(void* this_ptr,
                                               int32_t* width, int32_t* height);

    PiResult (PI_CALL *pi_plugin_set_visible)(void* this_ptr, int32_t visible);
} IPiPluginViewVtbl;

typedef struct IPiPluginView {
    const IPiPluginViewVtbl* lpVtbl;
} IPiPluginView;

static inline PiResult pi_plugin_view_attach(IPiPluginView* self, PiNativeWindow parent) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_attach) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_plugin_attach((void*)self, parent);
}

static inline PiResult pi_plugin_view_detach(IPiPluginView* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_detach) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_plugin_detach((void*)self);
}

static inline PiNativeWindow pi_plugin_view_get_native_window(IPiPluginView* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_native_window)
        return PI_INVALID_WINDOW;
    return self->lpVtbl->pi_plugin_get_native_window((void*)self);
}

static inline PiResult pi_plugin_view_on_resize(IPiPluginView* self, int32_t w, int32_t h) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_on_resize) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_plugin_on_resize((void*)self, w, h);
}

static inline PiResult pi_plugin_view_on_idle(IPiPluginView* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_on_idle) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_plugin_on_idle((void*)self);
}

static inline PiResult pi_plugin_view_get_preferred_size(IPiPluginView* self,
                                                   int32_t* w, int32_t* h) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_preferred_size)
        return PI_E_INVALIDARG;
    return self->lpVtbl->pi_plugin_get_preferred_size((void*)self, w, h);
}

static inline PiResult pi_plugin_view_set_visible(IPiPluginView* self, int32_t visible) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_set_visible) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_plugin_set_visible((void*)self, visible);
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_VIEW_H */
