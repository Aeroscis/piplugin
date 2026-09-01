/*
 * pipluginframework - Host-side implementation
 *
 * Handles loading plugin DLLs, extracting factories, managing lifecycle,
 * and provides the default IPiHostServices implementation that hosts can
 * hand to plugins.
 */
#include "pipluginframework/pi_plugin_host.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if PI_PLATFORM_WINDOWS
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

/* --------------------------------------------------------------------------
 * Load error diagnostics
 * -------------------------------------------------------------------------- */
static char g_load_error[256] = "no error";

PI_EXPORT const char* pi_module_get_load_error(void)
{
    return g_load_error;
}

/* --------------------------------------------------------------------------
 * PiPluginModule — internal struct
 * -------------------------------------------------------------------------- */
struct PiPluginModule {
#if PI_PLATFORM_WINDOWS
    HMODULE            handle;
#else
    void*              handle;
#endif
    IPiPluginFactory*  factory;   /* factory lives as long as the module */
    char*              path;
};

/* --------------------------------------------------------------------------
 * pi_module_load
 * -------------------------------------------------------------------------- */
PI_EXPORT PiPluginModule* pi_module_load(const char* path)
{
    if (!path) { snprintf(g_load_error, sizeof(g_load_error), "null path"); return NULL; }

#if PI_PLATFORM_WINDOWS
    HMODULE handle = LoadLibraryA(path);
    if (!handle) {
        snprintf(g_load_error, sizeof(g_load_error),
                 "LoadLibraryA(\"%s\") failed (err=%lu)", path, (unsigned long)GetLastError());
        return NULL;
    }
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(g_load_error, sizeof(g_load_error),
                 "dlopen(\"%s\") failed: %s", path, dlerror());
        return NULL;
    }
#endif

    /* Resolve the entry point */
#if PI_PLATFORM_WINDOWS
    PiPluginEntryProc entry = (PiPluginEntryProc)GetProcAddress(handle, PI_PLUGIN_ENTRY_NAME);
#else
    PiPluginEntryProc entry = (PiPluginEntryProc)dlsym(handle, PI_PLUGIN_ENTRY_NAME);
#endif
    if (!entry) {
        snprintf(g_load_error, sizeof(g_load_error),
                 "\"%s\" does not export %s", path, PI_PLUGIN_ENTRY_NAME);
#if PI_PLATFORM_WINDOWS
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return NULL;
    }

    IPiPluginFactory* factory = NULL;
    PiResult hr = entry(&factory);
    if (PI_FAILED(hr) || !factory) {
        snprintf(g_load_error, sizeof(g_load_error),
                 "%s entry point failed (hr=%d)", path, (int)hr);
#if PI_PLATFORM_WINDOWS
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return NULL;
    }

    PiPluginModule* module = (PiPluginModule*)malloc(sizeof(PiPluginModule));
    if (!module) {
        snprintf(g_load_error, sizeof(g_load_error), "out of memory");
#if PI_PLATFORM_WINDOWS
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return NULL;
    }
    snprintf(g_load_error, sizeof(g_load_error), "no error");

    module->handle  = handle;
    module->factory = factory;
#if PI_PLATFORM_WINDOWS
    module->path    = _strdup(path);
#else
    module->path    = strdup(path);
#endif
    return module;
}

/* --------------------------------------------------------------------------
 * pi_module_unload
 * -------------------------------------------------------------------------- */
PI_EXPORT void pi_module_unload(PiPluginModule* module)
{
    if (!module) return;

    /* Release our reference to the factory. Note: all plugin instances
     * created from this factory must already be released, otherwise their
     * vtables point into unmapped memory after FreeLibrary. */
    if (module->factory) {
        pi_iunknown_release((IPiUnknown*)module->factory);
    }

#if PI_PLATFORM_WINDOWS
    FreeLibrary(module->handle);
#else
    dlclose(module->handle);
#endif

    free(module->path);
    free(module);
}

/* --------------------------------------------------------------------------
 * pi_module_get_factory
 * -------------------------------------------------------------------------- */
PI_EXPORT PiResult pi_module_get_factory(PiPluginModule* module,
                                          IPiPluginFactory** out_factory)
{
    if (!module || !out_factory) return PI_E_INVALIDARG;
    *out_factory = module->factory;
    if (*out_factory) {
        pi_iunknown_add_ref((IPiUnknown*)(*out_factory));
    }
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Default IPiHostServices implementation
 *
 * One C object exposes both IPiHostServices and (optionally) IPiHostUI.
 * Layout: PiRefCountedBase first, then the state. `this_ptr` in every
 * vtable slot points at the object start, so we cast to the full struct.
 * -------------------------------------------------------------------------- */
typedef struct PiDefaultHost {
    PiRefCountedBase     base;           /* must be FIRST member          */
    PiHostMessageProc    post_message;
    void*                user_data;
    volatile PiNativeWindow ui_window;   /* PI_INVALID_WINDOW = headless  */
    uint64_t             ui_thread_id;
} PiDefaultHost;

/* Separate IPiHostUI view over a PiDefaultHost. COM identity rules say an
 * interface pointer needs its own vtbl slot, so we hand out this small
 * wrapper whose lifetime pins the owner. */
typedef struct PiDefaultHostUI {
    PiRefCountedBase     base;           /* must be FIRST member          */
    PiDefaultHost*       owner;          /* add-ref'd                     */
} PiDefaultHostUI;

static PiResult PI_CALL pi_default_host_qi(void* self_ptr, const PiGuid* iid, void** out);
static uint32_t PI_CALL pi_default_host_add_ref(void* self_ptr);
static uint32_t PI_CALL pi_default_host_release(void* self_ptr);
static void* PI_CALL pi_default_host_alloc(void* self_ptr, size_t size);
static void PI_CALL pi_default_host_free(void* self_ptr, void* ptr);
static void PI_CALL pi_default_host_post(void* self_ptr, uint32_t msg,
                                          uintptr_t wparam, intptr_t lparam);
static PiNativeWindow PI_CALL pi_default_host_parent_window(void* self_ptr);
static uint64_t PI_CALL pi_default_host_thread_id(void* self_ptr);

static PiResult PI_CALL pi_default_host_ui_qi(void* self_ptr, const PiGuid* iid, void** out);
static uint32_t PI_CALL pi_default_host_ui_add_ref(void* self_ptr);
static uint32_t PI_CALL pi_default_host_ui_release(void* self_ptr);
static void pi_default_host_ui_destroy(void* self_ptr);

static const IPiHostServicesVtbl s_default_host_services_vtbl = {
    { &pi_default_host_qi, &pi_default_host_add_ref, &pi_default_host_release },
    &pi_default_host_alloc,
    &pi_default_host_free,
    &pi_default_host_post
};

static const IPiHostUIVtbl s_default_host_ui_vtbl = {
    { &pi_default_host_ui_qi, &pi_default_host_ui_add_ref, &pi_default_host_ui_release },
    &pi_default_host_parent_window,
    &pi_default_host_thread_id
};

static PiResult PI_CALL pi_default_host_qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    PiDefaultHost* me = (PiDefaultHost*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_IID_HOST_SERVICES)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    /* UI capability is only exposed when a window was set */
    if (pi_guid_equal(iid, &PI_IID_HOST_UI) && me->ui_window != PI_INVALID_WINDOW) {
        PiDefaultHostUI* ui = (PiDefaultHostUI*)calloc(1, sizeof(PiDefaultHostUI));
        if (!ui) return PI_E_OUTOFMEMORY;
        pi_refcounted_init_with_destroy(&ui->base,
                                        (const IPiUnknownVtbl*)&s_default_host_ui_vtbl,
                                        &pi_default_host_ui_destroy);
        ui->owner = me;
        pi_iunknown_add_ref((IPiUnknown*)&me->base);
        *out = &ui->base;
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiResult PI_CALL pi_default_host_ui_qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    PiDefaultHostUI* me = (PiDefaultHostUI*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_IID_HOST_UI)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static uint32_t PI_CALL pi_default_host_ui_add_ref(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}

static uint32_t PI_CALL pi_default_host_ui_release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static void pi_default_host_ui_destroy(void* self_ptr)
{
    PiDefaultHostUI* me = (PiDefaultHostUI*)self_ptr;
    if (me->owner)
        pi_iunknown_release((IPiUnknown*)&me->owner->base);
    free(me);
}

static uint32_t PI_CALL pi_default_host_add_ref(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}

static uint32_t PI_CALL pi_default_host_release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static void* PI_CALL pi_default_host_alloc(void* self_ptr, size_t size)
{
    (void)self_ptr;
    return malloc(size);
}

static void PI_CALL pi_default_host_free(void* self_ptr, void* ptr)
{
    (void)self_ptr;
    free(ptr);
}

static void PI_CALL pi_default_host_post(void* self_ptr, uint32_t msg,
                                          uintptr_t wparam, intptr_t lparam)
{
    PiDefaultHost* me = (PiDefaultHost*)self_ptr;
    if (me->post_message)
        me->post_message(me->user_data, msg, wparam, lparam);
}

static PiNativeWindow PI_CALL pi_default_host_parent_window(void* self_ptr)
{
    PiDefaultHost* me = (PiDefaultHost*)self_ptr;
    return me->ui_window;
}

static uint64_t PI_CALL pi_default_host_thread_id(void* self_ptr)
{
    PiDefaultHost* me = (PiDefaultHost*)self_ptr;
    return me->ui_thread_id;
}

static void pi_default_host_destroy(void* self_ptr)
{
    free(self_ptr);
}

PI_EXPORT PiResult pi_host_services_create_default(
    PiHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,
    IPiHostServices** out_services)
{
    if (!out_services) return PI_E_INVALIDARG;
    *out_services = NULL;

    PiDefaultHost* host = (PiDefaultHost*)calloc(1, sizeof(PiDefaultHost));
    if (!host) return PI_E_OUTOFMEMORY;

    pi_refcounted_init_with_destroy(&host->base,
                                    (const IPiUnknownVtbl*)&s_default_host_services_vtbl,
                                    &pi_default_host_destroy);
    host->post_message = post_message;
    host->user_data    = user_data;
    host->ui_window    = ui_parent_window;
#if PI_PLATFORM_WINDOWS
    host->ui_thread_id = (uint64_t)GetCurrentThreadId();
#else
    host->ui_thread_id = (uint64_t)getpid();
#endif

    *out_services = (IPiHostServices*)&host->base;
    return PI_OK;
}

PI_EXPORT void pi_host_default_set_ui_window(IPiHostServices* services,
                                              PiNativeWindow window)
{
    /* The vtbl pointer at offset 0 is the services vtbl for this default
     * object; recover the state behind it. */
    PiDefaultHost* host = (PiDefaultHost*)services;
    if (!host || host->base.unk.lpVtbl != (const IPiUnknownVtbl*)&s_default_host_services_vtbl)
        return; /* not a default host services object */
    host->ui_window = window;
}

/* --------------------------------------------------------------------------
 * pi_host_create_plugin — convenience loader
 * -------------------------------------------------------------------------- */
PI_EXPORT PiResult pi_host_create_plugin(const char* dll_path,
                                          const PiGuid* class_guid,
                                          IPiHostServices* host,
                                          IPiPluginBase** out_plugin,
                                          PiPluginModule** out_module)
{
    if (!dll_path || !class_guid || !out_plugin) return PI_E_INVALIDARG;

    PiPluginModule* module = pi_module_load(dll_path);
    if (!module) return PI_E_NOTFOUND;

    IPiPluginFactory* factory = NULL;
    PiResult hr = pi_module_get_factory(module, &factory);
    if (PI_FAILED(hr)) {
        pi_module_unload(module);
        return hr;
    }

    hr = pi_factory_create_instance(factory, class_guid, host, out_plugin);
    pi_iunknown_release((IPiUnknown*)factory);

    if (PI_FAILED(hr)) {
        pi_module_unload(module);
        return hr;
    }

    hr = pi_plugin_initialize(*out_plugin, host);
    if (PI_FAILED(hr)) {
        pi_iunknown_release((IPiUnknown*)*out_plugin);
        *out_plugin = NULL;
        pi_module_unload(module);
        return hr;
    }

    /* The module must stay loaded while the plugin instance is alive;
     * hand it to the caller if they asked for it, otherwise we cannot
     * track the plugin's lifetime here — keep it loaded (leaked on
     * purpose) to keep the demo safe. */
    if (out_module) {
        *out_module = module;
    } else {
        /* Intentionally not unloading: unloading here would invalidate the
         * plugin's code. Callers who pass NULL accept the leak. */
    }
    return PI_OK;
}
