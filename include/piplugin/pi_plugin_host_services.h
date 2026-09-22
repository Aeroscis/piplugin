/*
 * piplugin - IPiHostServices / IPiHostUI
 *
 * The host is handed to the plugin at initialization time as an
 * IPiHostServices COM-style object instead of a raw callback struct.
 *
 *   IPiHostServices  - core services every host (GUI or headless) provides:
 *                      memory allocation and message posting.
 *   IPiHostUI        - OPTIONAL capability, queried by the plugin via
 *                      QueryInterface(PI_IID_HOST_UI). Only GUI hosts
 *                      expose it. A headless host (e.g. a task server)
 *                      returns PI_E_NOINTERFACE, and plugins that declared
 *                      PI_IID_HOST_UI as PI_CAP_OPTIONAL keep running.
 *
 * This mirrors the LV2 feature model: the core stays GUI-agnostic, and
 * GUI support is discovered at runtime in both directions.
 */
#ifndef PI_PLUGIN_HOST_SERVICES_H
#define PI_PLUGIN_HOST_SERVICES_H

#include "pi_plugin_unknown.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * IPiHostServices - core host services (always available)
 * -------------------------------------------------------------------------- */
typedef struct IPiHostServicesVtbl {
    IPiUnknownVtbl base;

    /* Allocate memory through the host. Thread-safe. */
    void* (PI_CALL *pi_host_alloc)(void* this_ptr, size_t size);

    /* Free memory obtained from pi_host_alloc. NULL-safe. */
    void  (PI_CALL *pi_host_free)(void* this_ptr, void* ptr);

    /* Post a message to the host. Safe to call from any plugin thread;
     * the host decides how to marshal it to its own event loop. */
    void  (PI_CALL *pi_host_post_message)(void* this_ptr, uint32_t msg,
                                          uintptr_t wparam, intptr_t lparam);
} IPiHostServicesVtbl;

typedef struct IPiHostServices {
    const IPiHostServicesVtbl* lpVtbl;
} IPiHostServices;

/* Inline helpers */
static inline void* pi_host_alloc(IPiHostServices* self, size_t size) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_alloc) return NULL;
    return self->lpVtbl->pi_host_alloc((void*)self, size);
}

static inline void pi_host_free(IPiHostServices* self, void* ptr) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_free) return;
    self->lpVtbl->pi_host_free((void*)self, ptr);
}

static inline void pi_host_post_message(IPiHostServices* self, uint32_t msg,
                                        uintptr_t wparam, intptr_t lparam) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_post_message) return;
    self->lpVtbl->pi_host_post_message((void*)self, msg, wparam, lparam);
}

/* --------------------------------------------------------------------------
 * IPiHostUI - optional GUI host capability
 *
 * A plugin that has a GUI queries this interface on the host object.
 * If the query fails with PI_E_NOINTERFACE the host is headless and the
 * plugin must not create any UI.
 * -------------------------------------------------------------------------- */
typedef struct IPiHostUIVtbl {
    IPiUnknownVtbl base;

    /* Native window handle the plugin should embed into.
     * Returns PI_INVALID_WINDOW if the host currently has no window. */
    PiNativeWindow (PI_CALL *pi_host_get_parent_window)(void* this_ptr);

    /* The host's UI thread identity, so the plugin knows which thread owns the
     * event loop that drives pi_on_idle().
     *
     * 契约（BLK-08 终审）：
     *   - 非 0，且在 UI 线程存活期间稳定；
     *   - **只用于"是不是同一个线程"的比较**，不要假设它等于某个操作系统
     *     工具（任务管理器 / ps -T / top -H）显示的线程号；
     *   - 平台实现：Windows = GetCurrentThreadId()，Linux = gettid()（内核
     *     线程 id），macOS = pthread_self() 句柄转 64 位。
     *     Linux 上主线程的 tid 与进程 id 相同，这不是 bug。 */
    uint64_t (PI_CALL *pi_host_ui_thread_id)(void* this_ptr);
} IPiHostUIVtbl;

typedef struct IPiHostUI {
    const IPiHostUIVtbl* lpVtbl;
} IPiHostUI;

static inline PiNativeWindow pi_host_ui_get_parent_window(IPiHostUI* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_get_parent_window)
        return PI_INVALID_WINDOW;
    return self->lpVtbl->pi_host_get_parent_window((void*)self);
}

static inline uint64_t pi_host_ui_thread_id(IPiHostUI* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_ui_thread_id) return 0;
    return self->lpVtbl->pi_host_ui_thread_id((void*)self);
}

/* --------------------------------------------------------------------------
 * Host-side default implementation
 *
 * Creates a refcounted IPiHostServices object the host can hand to plugins.
 *   post_message          - callback invoked on pi_host_post_message
 *                           (may be NULL; messages are dropped)
 *   user_data             - opaque pointer passed to post_message
 *   ui_parent_window      - if not PI_INVALID_WINDOW, the object also
 *                           exposes IPiHostUI returning this window;
 *                           otherwise it stays headless.
 *   out_services          - receives the new object (refcount 1);
 *                           destroy with ->pi_release()
 *
 * A GUI host typically creates the object early, then calls
 * pi_host_default_set_ui_window() whenever the embed container changes.
 * -------------------------------------------------------------------------- */
typedef void (*PiHostMessageProc)(void* user_data, uint32_t msg,
                                  uintptr_t wparam, intptr_t lparam);

PI_EXPORT PiResult pi_host_services_create_default(
    PiHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,
    IPiHostServices** out_services);

/* Update / clear the UI parent window of a default host services object
 * created above. Passing PI_INVALID_WINDOW makes the host headless again. */
PI_EXPORT void pi_host_default_set_ui_window(IPiHostServices* services,
                                              PiNativeWindow window);

/* --------------------------------------------------------------------------
 * Composable host services (roadmap APP-01)
 *
 * The default host object answers the framework's own IIDs and nothing else,
 * so an app could not offer its plugins a service of its own: the reverse of
 * the plugin-defined-protocol channel. create_ex adds the missing half — the
 * app installs an "extra QI" hook, and every IID the framework object does
 * not recognise is forwarded to it.
 *
 * The plugin side needs no new API at all: it is an ordinary
 * QueryInterface() on the very same host object it was handed, so a plugin
 * that neither knows nor needs the app's service gets PI_E_NOINTERFACE and
 * carries on (declare the capability PI_CAP_OPTIONAL to say so in the
 * descriptor, exactly as PI_IID_HOST_UI does).
 *
 * The hook has the same contract as IPiUnknown::pi_query_interface:
 *   - found it:  return PI_OK and store an ADD-REF'd interface pointer in *out;
 *   - not mine:  return PI_E_NOINTERFACE and set *out = NULL; any other
 *                failure code is passed through to the plugin unchanged
 *                (so PI_E_OUTOFMEMORY from the app survives the trip);
 *   - *out is pre-set to NULL by the framework before the hook runs, and is
 *     forced back to NULL if the hook reports failure;
 *   - returning PI_OK without writing *out is treated as "not mine" rather
 *     than handing the plugin a NULL "success".
 * The hook may be called from whatever thread the plugin queries from, so it
 * must be thread-safe if plugins may query concurrently. IIDs this object
 * answers itself (IPiHostServices always, IPiHostUI while it has a window)
 * are answered first and never forwarded. A headless object's IPiHostUI is a
 * miss like any other, so an app may supply its own — a remote or proxied UI
 * service, for instance; the framework would rather let the app decide than
 * hard-code that headless means "no UI service of any kind".
 * -------------------------------------------------------------------------- */
typedef PiResult (*PiHostExtraQiProc)(void* ctx, const PiGuid* iid, void** out);

/* Same as pi_host_services_create_default(), plus `extra_qi`.
 *
 * Passing extra_qi == NULL is exactly equivalent to create_default(): the two
 * entry points share one implementation, and the object created is the very
 * same shape (pi_host_default_set_ui_window() and the IPiHostUI capability
 * behave identically on it).
 *
 * `extra_qi_ctx` is opaque; it is handed back to the hook and never touched
 * by the framework. The framework does not take ownership of anything the
 * hook returns beyond the reference it hands to the plugin. */
PI_EXPORT PiResult pi_host_services_create_ex(
    PiHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,
    PiHostExtraQiProc extra_qi, void* extra_qi_ctx,
    IPiHostServices** out_services);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_HOST_SERVICES_H */
