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

/* 线程身份（pi_host_ui_thread_id）的平台实现所需：
 *   Linux 用内核线程 id（glibc 的 gettid() 到 2.30 才有，故直接走 syscall，
 *   任何 glibc 版本都能编译且不需要额外链接 pthread）；
 *   macOS 用 pthread_self 句柄（libSystem 必然提供，且对"是不是同一个线程"
 *   的判断已经足够）。 */
#if PI_PLATFORM_LINUX
#  include <unistd.h>
#  include <sys/syscall.h>
#elif PI_PLATFORM_MACOS
#  include <pthread.h>
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
static PiNativeWindow PI_CALL pi_default_host_ui_parent_window(void* self_ptr);
static uint64_t PI_CALL pi_default_host_ui_thread_id(void* self_ptr);

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
    &pi_default_host_ui_parent_window,
    &pi_default_host_ui_thread_id
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

/* 这两个槽位是从 **PiDefaultHostUI 包装对象**上调用的：IPiHostUI 的接口指针
 * 就是那个包装（它的 vtbl 是本表），所以必须先取回 owner 再读宿主状态。
 *
 * 历史缺陷（由 tests/unit 的单测发现）：这里曾把 self_ptr 直接当作
 * PiDefaultHost*，于是 ui_window / ui_thread_id 会按 PiDefaultHost 的偏移
 * (40 / 48) 去读一个只有 32 字节的 PiDefaultHostUI 分配 —— 越界读，且返回给
 * 插件的是垃圾句柄/垃圾线程 id。修法是走 owner；因为包装持有 owner 的引用，
 * 这里读到的是活值（pi_host_default_set_ui_window 之后立刻生效）。 */
static PiNativeWindow PI_CALL pi_default_host_ui_parent_window(void* self_ptr)
{
    PiDefaultHostUI* me = (PiDefaultHostUI*)self_ptr;
    if (!me->owner) return PI_INVALID_WINDOW;
    return me->owner->ui_window;
}

static uint64_t PI_CALL pi_default_host_ui_thread_id(void* self_ptr)
{
    PiDefaultHostUI* me = (PiDefaultHostUI*)self_ptr;
    if (!me->owner) return 0;
    return me->owner->ui_thread_id;
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
    /* 契约见 pi_plugin_host_services.h：非 0、UI 线程存活期间稳定、只用于
     * "是不是同一个线程"的比较。
     *
     * BLK-08 修复：非 Windows 分支曾经返回 getpid() —— 那是**进程 id**，
     * 插件拿它做线程判断必然出错。 */
#if PI_PLATFORM_WINDOWS
    host->ui_thread_id = (uint64_t)GetCurrentThreadId();
#elif PI_PLATFORM_LINUX
    host->ui_thread_id = (uint64_t)syscall(SYS_gettid);
#elif PI_PLATFORM_MACOS
    host->ui_thread_id = (uint64_t)(uintptr_t)pthread_self();
#else
    host->ui_thread_id = 0;
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

    /* 终审约定（BLK-08）：失败时 out 参数一律为 NULL，调用方不必自带预置。
     * 之前这里什么都不写，失败后调用方读到的会是它自己的旧值。 */
    *out_plugin = NULL;
    if (out_module) *out_module = NULL;

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
