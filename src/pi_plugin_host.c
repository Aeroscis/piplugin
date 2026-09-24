/*
 * piplugin - Host-side implementation
 *
 * Handles loading plugin DLLs, extracting factories, managing lifecycle,
 * and provides the default IPiPluginHostServices implementation that hosts can
 * hand to plugins.
 */
/* Linux/glibc 平台层可见性：本文件用到 strdup（POSIX 层）与 syscall（__USE_MISC 层），
 * 而项目用 -std=c11 编译，会定义 __STRICT_ANSI__，glibc 就把这两层声明挡在 feature
 * 门后（gcc 16 -std=c11 --pedantic-errors 实测：两者都是 implicit declaration）。
 * _DEFAULT_SOURCE 同时打开这两层，且必须出现在**任何**系统头之前；MSVC 不认识这个
 * 宏，定义它没有副作用。 */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "piplugin/pi_plugin_host.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if PI_PLATFORM_WINDOWS
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

/* 线程身份（pi_plugin_host_ui_thread_id）的平台实现所需：
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
 *
 * W-01：错误串是**线程局部**的。原先是进程级 static，多线程宿主里各线程
 * 互相覆盖 —— 并发加载失败时所有线程都只能读到"最后那一条"。
 * 语义不变（"最近一次 pi_plugin_module_load 的可读原因"），只是"下一次调用"
 * 现在按线程算；单线程行为逐字不变（tests/unit 有回归）。
 * -------------------------------------------------------------------------- */
#if PI_PLATFORM_WINDOWS
#  define PI_PLUGIN_LOAD_ERROR_TLS __declspec(thread)
#else
#  define PI_PLUGIN_LOAD_ERROR_TLS _Thread_local
#endif

#define PI_PLUGIN_LOAD_ERROR_MAX 256

static PI_PLUGIN_LOAD_ERROR_TLS char g_load_error[PI_PLUGIN_LOAD_ERROR_MAX] = "no error";

PI_PLUGIN_API const char* pi_plugin_module_get_load_error(void)
{
    return g_load_error;
}

PI_PLUGIN_API PiResult pi_plugin_module_get_load_error_r(char* buf, size_t size)
{
    if (!buf || size == 0) return PI_E_INVALIDARG;
    /* 拷贝本线程那一条；snprintf 保证 NUL 结尾（放不下就截断） */
    snprintf(buf, size, "%s", g_load_error);
    return PI_OK;
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
 * pi_plugin_module_load
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API PiPluginModule* pi_plugin_module_load(const char* path)
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
    /* POSIX 保证 dlsym 的 void* 可以当函数指针用，但 ISO C 禁止"对象指针 -> 函数
     * 指针"的直接转换，--pedantic-errors 下是错误（实测 gcc 16 -Wpedantic）。
     * 逐字节拷贝绕开这条转换规则，是 C 里取 dlsym 结果的标准写法。 */
    void* symbol = dlsym(handle, PI_PLUGIN_ENTRY_NAME);
    PiPluginEntryProc entry = NULL;
    memcpy(&entry, &symbol, sizeof(entry));
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
 * pi_plugin_module_unload
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API void pi_plugin_module_unload(PiPluginModule* module)
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
 * pi_plugin_module_get_factory
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API PiResult pi_plugin_module_get_factory(PiPluginModule* module,
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
 * Default IPiPluginHostServices implementation
 *
 * One C object exposes both IPiPluginHostServices and (optionally) IPiPluginHostUI.
 * Layout: PiRefCountedBase first, then the state. `this_ptr` in every
 * vtable slot points at the object start, so we cast to the full struct.
 * -------------------------------------------------------------------------- */
typedef struct PiPluginDefaultHost {
    PiRefCountedBase     base;           /* must be FIRST member          */
    PiPluginHostMessageProc    post_message;
    void*                user_data;
    volatile PiNativeWindow ui_window;   /* PI_INVALID_WINDOW = headless  */
    uint64_t             ui_thread_id;
    /* 可组合宿主服务（APP-01）：框架 IID 之外的 QI 一律转给宿主自己的钩子。
     * 两个都是 NULL 时行为与本钩子出现之前完全一致（create_ex(NULL) 与
     * create_default 共用同一条实现路径，见 pi_plugin_host_services_create_ex）。 */
    PiPluginHostExtraQiProc    extra_qi;
    void*                extra_qi_ctx;
} PiPluginDefaultHost;

/* Separate IPiPluginHostUI view over a PiPluginDefaultHost. COM identity rules say an
 * interface pointer needs its own vtbl slot, so we hand out this small
 * wrapper whose lifetime pins the owner. */
typedef struct PiPluginDefaultHostUI {
    PiRefCountedBase     base;           /* must be FIRST member          */
    PiPluginDefaultHost*       owner;          /* add-ref'd                     */
} PiPluginDefaultHostUI;

static PiResult PI_CALL pi_plugin_default_host_qi(void* self_ptr, const PiGuid* iid, void** out);
static uint32_t PI_CALL pi_plugin_default_host_add_ref(void* self_ptr);
static uint32_t PI_CALL pi_plugin_default_host_release(void* self_ptr);
static void* PI_CALL pi_plugin_default_host_alloc(void* self_ptr, size_t size);
static void PI_CALL pi_plugin_default_host_free(void* self_ptr, void* ptr);
static void PI_CALL pi_plugin_default_host_post(void* self_ptr, uint32_t msg,
                                          uintptr_t wparam, intptr_t lparam);
static PiNativeWindow PI_CALL pi_plugin_default_host_ui_parent_window(void* self_ptr);
static uint64_t PI_CALL pi_plugin_default_host_ui_thread_id(void* self_ptr);

static PiResult PI_CALL pi_plugin_default_host_ui_qi(void* self_ptr, const PiGuid* iid, void** out);
static uint32_t PI_CALL pi_plugin_default_host_ui_add_ref(void* self_ptr);
static uint32_t PI_CALL pi_plugin_default_host_ui_release(void* self_ptr);
static void pi_plugin_default_host_ui_destroy(void* self_ptr);

static const IPiPluginHostServicesVtbl s_default_host_services_vtbl = {
    { &pi_plugin_default_host_qi, &pi_plugin_default_host_add_ref, &pi_plugin_default_host_release },
    &pi_plugin_default_host_alloc,
    &pi_plugin_default_host_free,
    &pi_plugin_default_host_post
};

static const IPiPluginHostUIVtbl s_default_host_ui_vtbl = {
    { &pi_plugin_default_host_ui_qi, &pi_plugin_default_host_ui_add_ref, &pi_plugin_default_host_ui_release },
    &pi_plugin_default_host_ui_parent_window,
    &pi_plugin_default_host_ui_thread_id
};

static PiResult PI_CALL pi_plugin_default_host_qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    PiPluginDefaultHost* me = (PiPluginDefaultHost*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_PLUGIN_IID_HOST_SERVICES)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    /* UI capability is only exposed when a window was set */
    if (pi_guid_equal(iid, &PI_PLUGIN_IID_HOST_UI) && me->ui_window != PI_INVALID_WINDOW) {
        PiPluginDefaultHostUI* ui = (PiPluginDefaultHostUI*)calloc(1, sizeof(PiPluginDefaultHostUI));
        if (!ui) return PI_E_OUTOFMEMORY;
        pi_refcounted_init_with_destroy(&ui->base,
                                        (const IPiUnknownVtbl*)&s_default_host_ui_vtbl,
                                        &pi_plugin_default_host_ui_destroy);
        ui->owner = me;
        pi_iunknown_add_ref((IPiUnknown*)&me->base);
        *out = &ui->base;
        return PI_OK;
    }

    /* 框架 IID 之外的 QI 转交宿主自己的钩子（APP-01 可组合宿主服务）。
     * 契约见 pi_plugin_host_services.h：钩子成功时必须给出**已 add-ref** 的
     * 指针；返回失败时把错误码原样上抛（PI_E_OUTOFMEMORY 这类信息不该被
     * 压成 NOINTERFACE），但 *out 一定回到 NULL。 */
    if (me->extra_qi) {
        PiResult hr;
        *out = NULL;
        hr = me->extra_qi(me->extra_qi_ctx, iid, out);
        if (PI_SUCCEEDED(hr) && *out != NULL) return PI_OK;
        *out = NULL;
        return PI_FAILED(hr) ? hr : PI_E_NOINTERFACE;
    }

    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiResult PI_CALL pi_plugin_default_host_ui_qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    PiPluginDefaultHostUI* me = (PiPluginDefaultHostUI*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_PLUGIN_IID_HOST_UI)) {
        *out = &me->base;
        pi_iunknown_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    /* 本包装只代表 IPiPluginHostUI 这一个接口；app 自定义服务挂在宿主服务对象上，
     * 要 QI 它请用当初拿到的 IPiPluginHostServices 指针（UI 包装不再二次转发）。 */
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static uint32_t PI_CALL pi_plugin_default_host_ui_add_ref(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}

static uint32_t PI_CALL pi_plugin_default_host_ui_release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static void pi_plugin_default_host_ui_destroy(void* self_ptr)
{
    PiPluginDefaultHostUI* me = (PiPluginDefaultHostUI*)self_ptr;
    if (me->owner)
        pi_iunknown_release((IPiUnknown*)&me->owner->base);
    free(me);
}

static uint32_t PI_CALL pi_plugin_default_host_add_ref(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}

static uint32_t PI_CALL pi_plugin_default_host_release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static void* PI_CALL pi_plugin_default_host_alloc(void* self_ptr, size_t size)
{
    (void)self_ptr;
    return malloc(size);
}

static void PI_CALL pi_plugin_default_host_free(void* self_ptr, void* ptr)
{
    (void)self_ptr;
    free(ptr);
}

static void PI_CALL pi_plugin_default_host_post(void* self_ptr, uint32_t msg,
                                          uintptr_t wparam, intptr_t lparam)
{
    PiPluginDefaultHost* me = (PiPluginDefaultHost*)self_ptr;
    if (me->post_message)
        me->post_message(me->user_data, msg, wparam, lparam);
}

/* 这两个槽位是从 **PiPluginDefaultHostUI 包装对象**上调用的：IPiPluginHostUI 的接口指针
 * 就是那个包装（它的 vtbl 是本表），所以必须先取回 owner 再读宿主状态。
 *
 * 历史缺陷（由 tests/unit 的单测发现）：这里曾把 self_ptr 直接当作
 * PiPluginDefaultHost*，于是 ui_window / ui_thread_id 会按 PiPluginDefaultHost 的偏移
 * (40 / 48) 去读一个只有 32 字节的 PiPluginDefaultHostUI 分配 —— 越界读，且返回给
 * 插件的是垃圾句柄/垃圾线程 id。修法是走 owner；因为包装持有 owner 的引用，
 * 这里读到的是活值（pi_plugin_host_default_set_ui_window 之后立刻生效）。 */
static PiNativeWindow PI_CALL pi_plugin_default_host_ui_parent_window(void* self_ptr)
{
    PiPluginDefaultHostUI* me = (PiPluginDefaultHostUI*)self_ptr;
    if (!me->owner) return PI_INVALID_WINDOW;
    return me->owner->ui_window;
}

static uint64_t PI_CALL pi_plugin_default_host_ui_thread_id(void* self_ptr)
{
    PiPluginDefaultHostUI* me = (PiPluginDefaultHostUI*)self_ptr;
    if (!me->owner) return 0;
    return me->owner->ui_thread_id;
}

static void pi_plugin_default_host_destroy(void* self_ptr)
{
    free(self_ptr);
}

PI_PLUGIN_API PiResult pi_plugin_host_services_create_ex(
    PiPluginHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,
    PiPluginHostExtraQiProc extra_qi, void* extra_qi_ctx,
    IPiPluginHostServices** out_services)
{
    if (!out_services) return PI_E_INVALIDARG;
    *out_services = NULL;

    PiPluginDefaultHost* host = (PiPluginDefaultHost*)calloc(1, sizeof(PiPluginDefaultHost));
    if (!host) return PI_E_OUTOFMEMORY;

    pi_refcounted_init_with_destroy(&host->base,
                                    (const IPiUnknownVtbl*)&s_default_host_services_vtbl,
                                    &pi_plugin_default_host_destroy);
    host->post_message = post_message;
    host->user_data    = user_data;
    host->ui_window    = ui_parent_window;
    host->extra_qi     = extra_qi;
    host->extra_qi_ctx = extra_qi_ctx;
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

    *out_services = (IPiPluginHostServices*)&host->base;
    return PI_OK;
}

PI_PLUGIN_API PiResult pi_plugin_host_services_create_default(
    PiPluginHostMessageProc post_message, void* user_data,
    PiNativeWindow ui_parent_window,
    IPiPluginHostServices** out_services)
{
    /* 刻意只是转调（APP-01）：这样"没有 extra_qi"就不是一条需要单独维护的
     * 分支，而是 create_ex 在 extra_qi == NULL 时的同一条路径 —— 回归风险
     * 归零，也是 roadmap 验收要求的"行为与 create_default 完全一致"。 */
    return pi_plugin_host_services_create_ex(post_message, user_data, ui_parent_window,
                                      NULL, NULL, out_services);
}

PI_PLUGIN_API void pi_plugin_host_default_set_ui_window(IPiPluginHostServices* services,
                                              PiNativeWindow window)
{
    /* The vtbl pointer at offset 0 is the services vtbl for this default
     * object; recover the state behind it. */
    PiPluginDefaultHost* host = (PiPluginDefaultHost*)services;
    if (!host || host->base.unk.lpVtbl != (const IPiUnknownVtbl*)&s_default_host_services_vtbl)
        return; /* not a default host services object */
    host->ui_window = window;
}

/* --------------------------------------------------------------------------
 * pi_plugin_host_create_plugin — convenience loader
 * -------------------------------------------------------------------------- */
PI_PLUGIN_API PiResult pi_plugin_host_create_plugin(const char* dll_path,
                                          const PiGuid* class_guid,
                                          IPiPluginHostServices* host,
                                          IPiPluginBase** out_plugin,
                                          PiPluginModule** out_module)
{
    if (!dll_path || !class_guid || !out_plugin) return PI_E_INVALIDARG;

    /* 终审约定（BLK-08）：失败时 out 参数一律为 NULL，调用方不必自带预置。
     * 之前这里什么都不写，失败后调用方读到的会是它自己的旧值。 */
    *out_plugin = NULL;
    if (out_module) *out_module = NULL;

    PiPluginModule* module = pi_plugin_module_load(dll_path);
    if (!module) return PI_E_NOTFOUND;

    IPiPluginFactory* factory = NULL;
    PiResult hr = pi_plugin_module_get_factory(module, &factory);
    if (PI_FAILED(hr)) {
        pi_plugin_module_unload(module);
        return hr;
    }

    hr = pi_plugin_factory_create_instance(factory, class_guid, host, out_plugin);
    pi_iunknown_release((IPiUnknown*)factory);

    if (PI_FAILED(hr)) {
        pi_plugin_module_unload(module);
        return hr;
    }

    hr = pi_plugin_initialize(*out_plugin, host);
    if (PI_FAILED(hr)) {
        pi_iunknown_release((IPiUnknown*)*out_plugin);
        *out_plugin = NULL;
        pi_plugin_module_unload(module);
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
