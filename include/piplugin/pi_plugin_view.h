/*
 * piplugin - IPiPluginView / IPiPluginViewProvider
 *
 * GUI view interface that allows a plugin to render inside a host window.
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
 *
 * Two interfaces, one capability:
 *
 *   IPiPluginViewProvider - implemented by the PLUGIN. The host queries it
 *                           (PI_PLUGIN_IID_VIEW_PROVIDER), enumerates the views
 *                           that exist right now, and takes one by its id. A
 *                           plugin without a GUI simply does not expose it.
 *   IPiPluginView         - one view, handed out by the provider. The host
 *                           attaches it to a window it created, pumps it, and
 *                           releases it before termination.
 *
 * The set of views is dynamic (one per document, say). A host that opens one
 * window per view keeps a map from view id to window and reconciles it against
 * each enumeration; see the provider block below for why that is a getter and
 * not an event.
 */
#ifndef PI_PLUGIN_VIEW_H
#define PI_PLUGIN_VIEW_H

#include <pibase/pi_base.h>

#include "pi_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * IPiPluginView
 * -------------------------------------------------------------------------- */
typedef struct IPiPluginViewVtbl {
    IPiUnknownVtbl base;

    PiResult(PI_CALL* pi_plugin_attach)(void* this_ptr, PiNativeWindow parent_window);

    PiResult(PI_CALL* pi_plugin_detach)(void* this_ptr);

    PiNativeWindow(PI_CALL* pi_plugin_get_native_window)(void* this_ptr);

    PiResult(PI_CALL* pi_plugin_on_resize)(void* this_ptr, int32_t width, int32_t height);

    PiResult(PI_CALL* pi_plugin_on_idle)(void* this_ptr);

    PiResult(PI_CALL* pi_plugin_get_preferred_size)(void*    this_ptr,
                                                    int32_t* width, int32_t* height);

    PiResult(PI_CALL* pi_plugin_set_visible)(void* this_ptr, int32_t visible);

    /* Human-readable title for this view, so a host that puts each view in its
     * own window can name them without guessing ("Note 1", "Note 2").
     *
     * Returns PI_OK and an INTERNAL string — the plugin keeps ownership, the
     * host must copy it and must not free it; the pointer stays valid until the
     * view is detached. Returns PI_E_NOTIMPL when this view has no title of its
     * own, which is not an error: the host then falls back to its own naming.
     *
     * UTF-8, NUL-terminated. Called on the host GUI thread, after attach. */
    PiResult(PI_CALL* pi_plugin_view_get_title)(void* this_ptr, char const** out_title);
} IPiPluginViewVtbl;

typedef struct IPiPluginView {
    IPiPluginViewVtbl const* lpVtbl;
} IPiPluginView;

static inline PiResult pi_plugin_view_attach(IPiPluginView* self, PiNativeWindow parent)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_attach)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_attach((void*)self, parent);
}

static inline PiResult pi_plugin_view_detach(IPiPluginView* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_detach)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_detach((void*)self);
}

static inline PiNativeWindow pi_plugin_view_get_native_window(IPiPluginView* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_native_window)
    {
        return PI_INVALID_WINDOW;
    }
    return self->lpVtbl->pi_plugin_get_native_window((void*)self);
}

static inline PiResult pi_plugin_view_on_resize(IPiPluginView* self, int32_t w, int32_t h)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_on_resize)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_on_resize((void*)self, w, h);
}

static inline PiResult pi_plugin_view_on_idle(IPiPluginView* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_on_idle)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_on_idle((void*)self);
}

static inline PiResult pi_plugin_view_get_preferred_size(IPiPluginView* self,
                                                         int32_t* w, int32_t* h)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_preferred_size)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_get_preferred_size((void*)self, w, h);
}

static inline PiResult pi_plugin_view_set_visible(IPiPluginView* self, int32_t visible)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_set_visible)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_set_visible((void*)self, visible);
}

static inline PiResult pi_plugin_view_get_title(IPiPluginView* self, char const** out_title)
{
    if (!out_title)
    {
        return PI_E_INVALIDARG;
    }
    *out_title = NULL;
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_view_get_title)
    {
        return PI_E_NOINTERFACE;
    }
    return self->lpVtbl->pi_plugin_view_get_title((void*)self, out_title);
}

/* --------------------------------------------------------------------------
 * IPiPluginViewProvider
 *
 * The identity of a view is a string the PLUGIN chooses, not an index. Indices
 * are only a cursor through the current enumeration, and the set itself is
 * dynamic: a plugin that opens one view per document gains a view when the user
 * creates a document and loses one when they close it.
 *
 * Why a stable id instead of an index: a host keeps one window per view. If the
 * plugin deleted view 1 out of 3 and everything shifted down, every window would
 * be showing someone else's content. With an id, the host maps id -> window and
 * a removal is just "this id is gone".
 *
 * How the host learns about a change: it re-reads pi_plugin_views(). That call
 * is a borrowed-array getter with no allocation, so a host can call it on every
 * frame it already pumps, which is what the framework expects. There is no
 * change event on purpose — events in this framework are best effort and may be
 * dropped (pi_plugin_events.h), and a dropped "the list changed" event would be
 * a window that never appears. A cheap getter cannot be dropped.
 * -------------------------------------------------------------------------- */

/* Longest view title or id the framework will copy, including the terminator.
 * Longer strings are truncated by the host, not rejected. */
#define PI_PLUGIN_VIEW_TITLE_MAX 256u
#define PI_PLUGIN_VIEW_ID_MAX    PI_PLUGIN_VIEW_TITLE_MAX

typedef struct PiPluginViewInfo {
    /* Stable identity, UTF-8, NUL-terminated, non-empty, unique among the views
     * that currently exist. A single-panel plugin passes "main".
     *
     * Borrowed from the plugin: valid until the plugin's next pi_plugin_views()
     * call, its terminate, or a change to that view's id — whichever comes
     * first. The host must copy it if it wants to keep it. */
    char const* id;
} PiPluginViewInfo;

typedef struct IPiPluginViewProviderVtbl {
    IPiUnknownVtbl base;

    /* Enumerate the views that exist right now.
     *
     * *out_views receives a BORROWED array owned by the plugin; the host must
     * not free it, and must not hold it across calls that could change it.
     * *out_count is at least 1 — a plugin with no GUI does not expose this
     * interface at all, so "provider that exists" and "has no views" do not have
     * to be the same state.
     *
     * The count and the ids may change from one call to the next. The host is
     * expected to call this each frame it pumps (see the note above) and to
     * reconcile: ids it has not seen get a window, windows whose id vanished are
     * destroyed. Order within the array is a presentation hint only (the host
     * may use it for window cascade placement); it is not identity.
     *
     * Host main thread, after pi_plugin_initialize. */
    PiResult(PI_CALL* pi_plugin_views)(void* this_ptr, PiPluginViewInfo const** out_views,
                                       uint32_t* out_count);

    /* Take the view with this id.
     *
     * On success *out_view holds a reference the caller must release, and the
     * view is NOT attached yet — the host calls pi_plugin_attach() on it
     * afterwards. Asking twice for the same id may return the same object with
     * an extra reference or a freshly built one; the plugin decides, but every
     * returned pointer must be independently releasable.
     *
     * PI_E_NOTFOUND means no view with that id exists (it was closed, or the id
     * was never listed). PI_E_INVALIDARG is a NULL/empty id or out_view.
     *
     * Host main thread. */
    PiResult(PI_CALL* pi_plugin_get_view)(void* this_ptr, char const* view_id,
                                          IPiPluginView** out_view);
} IPiPluginViewProviderVtbl;

typedef struct IPiPluginViewProvider {
    IPiPluginViewProviderVtbl const* lpVtbl;
} IPiPluginViewProvider;

static inline PiResult pi_plugin_views(IPiPluginViewProvider* self,
                                       PiPluginViewInfo const** out_views, uint32_t* out_count)
{
    if (!out_views || !out_count)
    {
        return PI_E_INVALIDARG;
    }
    *out_views = NULL;
    *out_count = 0;
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_views)
    {
        return PI_E_NOINTERFACE;
    }
    return self->lpVtbl->pi_plugin_views((void*)self, out_views, out_count);
}

static inline PiResult pi_plugin_get_view(IPiPluginViewProvider* self, char const* view_id,
                                          IPiPluginView** out_view)
{
    if (!out_view)
    {
        return PI_E_INVALIDARG;
    }
    *out_view = NULL;
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_get_view)
    {
        return PI_E_NOINTERFACE;
    }
    return self->lpVtbl->pi_plugin_get_view((void*)self, view_id, out_view);
}

/* Convenience: does this plugin expose a GUI? QI the provider and enumerate it.
 *
 * Returns PI_OK with an AddRef'd provider in *out_provider (release it) when the
 * plugin has at least one view; *out_count is that count at the time of the
 * call. Pass NULL for *out_provider if the caller only wants to know whether a
 * GUI exists.
 *
 * Returns PI_E_NOINTERFACE with both out-parameters cleared when the plugin has
 * no views; that is the normal headless case, not an error. */
static inline PiResult pi_plugin_query_view_provider(IPiPluginBase*          plugin,
                                                     IPiPluginViewProvider** out_provider,
                                                     uint32_t*               out_count)
{
    if (out_provider)
    {
        *out_provider = NULL;
    }
    if (out_count)
    {
        *out_count = 0;
    }
    if (!plugin || !out_count)
    {
        return PI_E_INVALIDARG;
    }

    IPiPluginViewProvider* provider = NULL;
    if (PI_FAILED(pi_iunknown_query_interface((IPiUnknown*)plugin, &PI_PLUGIN_IID_VIEW_PROVIDER,
                                              (void**)&provider)))
    {
        return PI_E_NOINTERFACE;
    }

    PiPluginViewInfo const* views = NULL;
    uint32_t count = 0;
    if (PI_FAILED(pi_plugin_views(provider, &views, &count)) || count == 0)
    {
        pi_iunknown_release((IPiUnknown*)provider);
        return PI_E_NOINTERFACE;
    }

    if (out_provider)
    {
        *out_provider = provider;
    }
    else
    {
        pi_iunknown_release((IPiUnknown*)provider);
    }
    *out_count = count;
    return PI_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_VIEW_H */
