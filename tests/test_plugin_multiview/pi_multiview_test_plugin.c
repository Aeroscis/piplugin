/*
 * piplugin - Multi-view test plugin (one plugin, two views)
 *
 * The acceptance piece for the 0.6 view capability split: a single plugin instance
 * exposes PI_PLUGIN_IID_VIEW_PROVIDER and hands out TWO independently releasable
 * views with the ids "alpha" and "beta". It deliberately owns no native window and
 * no GUI toolkit - the point of the test is the *bookkeeping* (enumerate by id,
 * take each once, attach each into its own container, pump each, detach each,
 * release the views, release the provider, terminate), not rendering.
 *
 * What it reports to the host (tests/test_host_multi --multi-view asserts on these):
 *
 *   0x2100 ATTACHED    wparam = 1 (alpha) / 2 (beta), lparam = the parent container
 *                      it was attached to  -> proves each view got its own container
 *   0x2101 DETACHED    wparam = 1 / 2
 *   0x2102 IDLE_TICKS  wparam = 1 / 2, lparam = how many on_idle calls it saw
 *                      -> proves the host pumped BOTH views, not just the first
 *   0x2103 TERMINATE   lparam = how many views were still alive when
 *                      pi_plugin_terminate ran -> must be 0, i.e. the host released
 *                      every view before unmapping this module
 *
 * Pure C, no adapters, no third-party dependency, so the case runs wherever the
 * core builds (which is the reason it is not one of the Qt/imgui plugins).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "piplugin/pi_plugin.h"

#define PI_MULTIVIEW_MSG_ATTACHED   0x2100u
#define PI_MULTIVIEW_MSG_DETACHED   0x2101u
#define PI_MULTIVIEW_MSG_IDLE_TICKS 0x2102u
#define PI_MULTIVIEW_MSG_TERMINATE  0x2103u

/* 视图编号：给消息的 wparam / 标题用，1 与 2 是插件自己的约定。 */
#define PI_MULTIVIEW_ALPHA 1
#define PI_MULTIVIEW_BETA  2

typedef struct MultiViewPlugin MultiViewPlugin;

typedef struct MultiViewView {
    PiRefCountedBase base; /* MUST be first */
    MultiViewPlugin* owner; /* add-ref'd */
    char const*      id;    /* static string: borrowed for the host */
    char const*      title; /* static string: borrowed for the host */
    int              slot;  /* PI_MULTIVIEW_ALPHA / _BETA */
    int              attached;
    PiNativeWindow   parent;
    uint32_t         ticks;
} MultiViewView;

struct MultiViewPlugin {
    PiRefCountedBase       base;  /* MUST be first */
    IPiPluginHostServices* host;  /* add-ref'd */
    int                    live_views; /* diagnostic: views the host has not released yet */
    uint32_t               created;    /* how many view objects were handed out */
};

typedef struct MultiViewProvider {
    PiRefCountedBase base;  /* MUST be first */
    MultiViewPlugin* owner; /* add-ref'd */
} MultiViewProvider;

/* 枚举结果：两个视图，顺序只是"呈现建议"，身份是 id。 */
static PiPluginViewInfo const s_views[] = {{"alpha"}, {"beta"}};

/* --------------------------------------------------------------------------
 * View object
 * -------------------------------------------------------------------------- */
static uint32_t PI_CALL View_AddRef(void* self_ptr);
static uint32_t PI_CALL View_Release(void* self_ptr);
static PiResult PI_CALL View_Qi(void* self_ptr, PiGuid const* iid, void** out);
static PiResult PI_CALL View_Attach(void* self_ptr, PiNativeWindow parent_window);
static PiResult PI_CALL View_Detach(void* self_ptr);
static PiNativeWindow PI_CALL View_GetNativeWindow(void* self_ptr);
static PiResult PI_CALL View_OnResize(void* self_ptr, int32_t width, int32_t height);
static PiResult PI_CALL View_OnIdle(void* self_ptr);
static PiResult PI_CALL View_GetPreferredSize(void* self_ptr, int32_t* width, int32_t* height);
static PiResult PI_CALL View_SetVisible(void* self_ptr, int32_t visible);
static PiResult PI_CALL View_GetTitle(void* self_ptr, char const** out_title);

static IPiPluginViewVtbl const s_view_vtbl = {
    {&View_Qi, &View_AddRef, &View_Release},
    &View_Attach,
    &View_Detach,
    &View_GetNativeWindow,
    &View_OnResize,
    &View_OnIdle,
    &View_GetPreferredSize,
    &View_SetVisible,
    &View_GetTitle
};

static uint32_t PI_CALL View_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL View_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static void View_Destroy(void* self_ptr)
{
    MultiViewView* view = (MultiViewView*)self_ptr;
    if (view->owner)
    {
        /* 宿主 GUI 线程上调用（框架的视图契约），所以不需要原子操作。 */
        --view->owner->live_views;
        pi_iunknown_release((IPiUnknown*)&view->owner->base);
        view->owner = NULL;
    }
    free(view);
}

static PiResult PI_CALL View_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    MultiViewView* view = (MultiViewView*)self_ptr;
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_VIEW))
    {
        *out = view;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiResult PI_CALL View_Attach(void* self_ptr, PiNativeWindow parent_window)
{
    MultiViewView* view = (MultiViewView*)self_ptr;
    if (!PI_IS_VALID_WINDOW(parent_window))
    {
        return PI_E_INVALIDARG;
    }
    if (view->attached)
    {
        return PI_E_UNEXPECTED;
    }
    view->attached = 1;
    view->parent   = parent_window;
    if (view->owner->host)
    {
        pi_plugin_host_post_message(view->owner->host, PI_MULTIVIEW_MSG_ATTACHED,
                                    (uintptr_t)view->slot, (intptr_t)parent_window);
    }
    return PI_OK;
}

static PiResult PI_CALL View_Detach(void* self_ptr)
{
    MultiViewView* view = (MultiViewView*)self_ptr;
    if (!view->attached)
    {
        return PI_OK; /* idempotent: the host may "make sure it is detached" */
    }
    view->attached = 0;
    view->parent   = PI_INVALID_WINDOW;
    if (view->owner->host)
    {
        pi_plugin_host_post_message(view->owner->host, PI_MULTIVIEW_MSG_DETACHED,
                                    (uintptr_t)view->slot, 0);
        /* 帧计数随 detach 一起上报：宿主要断言"两个视图都被 pump 过"，
         * 而这是唯一能在视图还有效时说清这件事的时刻。 */
        pi_plugin_host_post_message(view->owner->host, PI_MULTIVIEW_MSG_IDLE_TICKS,
                                    (uintptr_t)view->slot, (intptr_t)view->ticks);
    }
    return PI_OK;
}

static PiNativeWindow PI_CALL View_GetNativeWindow(void* self_ptr)
{
    (void)self_ptr;
    /* 本插件不拥有原生窗口：它只验证账本。宿主想拿窗口就去问真正的套件视图。 */
    return PI_INVALID_WINDOW;
}

static PiResult PI_CALL View_OnResize(void* self_ptr, int32_t width, int32_t height)
{
    (void)self_ptr;
    return (width > 0 && height > 0) ? PI_OK : PI_E_INVALIDARG;
}

static PiResult PI_CALL View_OnIdle(void* self_ptr)
{
    MultiViewView* view = (MultiViewView*)self_ptr;
    ++view->ticks;
    return PI_OK;
}

static PiResult PI_CALL View_GetPreferredSize(void* self_ptr, int32_t* width, int32_t* height)
{
    (void)self_ptr;
    if (!width || !height)
    {
        return PI_E_INVALIDARG;
    }
    *width  = 320;
    *height = 240;
    return PI_OK;
}

static PiResult PI_CALL View_SetVisible(void* self_ptr, int32_t visible)
{
    (void)self_ptr;
    (void)visible;
    return PI_OK;
}

static PiResult PI_CALL View_GetTitle(void* self_ptr, char const** out_title)
{
    MultiViewView* view = (MultiViewView*)self_ptr;
    if (!out_title)
    {
        return PI_E_INVALIDARG;
    }
    *out_title = view->title; /* borrowed, valid until detach */
    return PI_OK;
}

static IPiPluginView* ViewCreate(MultiViewPlugin* plugin, int slot)
{
    MultiViewView* view = (MultiViewView*)calloc(1, sizeof(MultiViewView));
    if (!view)
    {
        return NULL;
    }
    view->owner    = plugin;
    view->slot     = slot;
    view->id       = (slot == PI_MULTIVIEW_ALPHA) ? "alpha" : "beta";
    view->title    = (slot == PI_MULTIVIEW_ALPHA) ? "Alpha panel" : "Beta panel";
    view->attached = 0;
    view->parent   = PI_INVALID_WINDOW;
    pi_refcounted_init_with_destroy(&view->base, (IPiUnknownVtbl const*)&s_view_vtbl, &View_Destroy);
    pi_iunknown_add_ref((IPiUnknown*)&plugin->base);
    ++plugin->live_views;
    ++plugin->created;
    return (IPiPluginView*)&view->base;
}

/* --------------------------------------------------------------------------
 * Provider object
 * -------------------------------------------------------------------------- */
static uint32_t PI_CALL Provider_AddRef(void* self_ptr);
static uint32_t PI_CALL Provider_Release(void* self_ptr);
static PiResult PI_CALL Provider_Qi(void* self_ptr, PiGuid const* iid, void** out);
static PiResult PI_CALL Provider_Views(void* self_ptr, PiPluginViewInfo const** out_views,
                                       uint32_t* out_count);
static PiResult PI_CALL Provider_GetView(void* self_ptr, char const* view_id,
                                         IPiPluginView** out_view);

static IPiPluginViewProviderVtbl const s_provider_vtbl = {
    {&Provider_Qi, &Provider_AddRef, &Provider_Release},
    &Provider_Views,
    &Provider_GetView
};

static uint32_t PI_CALL Provider_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL Provider_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static void Provider_Destroy(void* self_ptr)
{
    MultiViewProvider* provider = (MultiViewProvider*)self_ptr;
    if (provider->owner)
    {
        pi_iunknown_release((IPiUnknown*)&provider->owner->base);
        provider->owner = NULL;
    }
    free(provider);
}

static PiResult PI_CALL Provider_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    MultiViewProvider* provider = (MultiViewProvider*)self_ptr;
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_VIEW_PROVIDER))
    {
        *out = provider;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiResult PI_CALL Provider_Views(void* self_ptr, PiPluginViewInfo const** out_views,
                                       uint32_t* out_count)
{
    (void)self_ptr;
    if (!out_views || !out_count)
    {
        return PI_E_INVALIDARG;
    }
    /* Borrowed array, no allocation, same two ids every call. A plugin that
     * opened documents would rebuild this and the count could differ. */
    *out_views = s_views;
    *out_count = (uint32_t)(sizeof(s_views) / sizeof(s_views[0]));
    return PI_OK;
}

static PiResult PI_CALL Provider_GetView(void* self_ptr, char const* view_id,
                                         IPiPluginView** out_view)
{
    MultiViewProvider* provider = (MultiViewProvider*)self_ptr;
    if (!out_view)
    {
        return PI_E_INVALIDARG;
    }
    *out_view = NULL;
    if (!view_id || view_id[0] == '\0')
    {
        return PI_E_INVALIDARG;
    }
    if (strcmp(view_id, "alpha") == 0)
    {
        *out_view = ViewCreate(provider->owner, PI_MULTIVIEW_ALPHA);
    }
    else if (strcmp(view_id, "beta") == 0)
    {
        *out_view = ViewCreate(provider->owner, PI_MULTIVIEW_BETA);
    }
    else
    {
        return PI_E_NOTFOUND;
    }
    return *out_view ? PI_OK : PI_E_OUTOFMEMORY;
}

/* --------------------------------------------------------------------------
 * Plugin object (lifecycle only - the views live behind the provider)
 * -------------------------------------------------------------------------- */
static uint32_t PI_CALL Plugin_AddRef(void* self_ptr);
static uint32_t PI_CALL Plugin_Release(void* self_ptr);
static PiResult PI_CALL Plugin_Qi(void* self_ptr, PiGuid const* iid, void** out);
static PiResult PI_CALL Plugin_Initialize(void* self_ptr, IPiPluginHostServices* host);
static PiResult PI_CALL Plugin_Terminate(void* self_ptr);

static IPiPluginBaseVtbl const s_plugin_vtbl = {
    {&Plugin_Qi, &Plugin_AddRef, &Plugin_Release},
    &Plugin_Initialize,
    &Plugin_Terminate
};

static uint32_t PI_CALL Plugin_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL Plugin_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static void Plugin_Destroy(void* self_ptr)
{
    MultiViewPlugin* plugin = (MultiViewPlugin*)self_ptr;
    if (plugin->host)
    {
        pi_iunknown_release((IPiUnknown*)plugin->host);
        plugin->host = NULL;
    }
    free(plugin);
}

static PiResult PI_CALL Plugin_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    MultiViewPlugin* plugin = (MultiViewPlugin*)self_ptr;

    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE))
    {
        *out = plugin;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    if (pi_guid_equal(iid, &PI_PLUGIN_IID_VIEW_PROVIDER))
    {
        MultiViewProvider* provider = (MultiViewProvider*)calloc(1, sizeof(MultiViewProvider));
        if (!provider)
        {
            *out = NULL;
            return PI_E_OUTOFMEMORY;
        }
        provider->owner = plugin;
        pi_refcounted_init_with_destroy(&provider->base, (IPiUnknownVtbl const*)&s_provider_vtbl,
                                        &Provider_Destroy);
        pi_iunknown_add_ref((IPiUnknown*)&plugin->base);
        *out = (IPiPluginViewProvider*)&provider->base;
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiResult PI_CALL Plugin_Initialize(void* self_ptr, IPiPluginHostServices* host)
{
    MultiViewPlugin* plugin = (MultiViewPlugin*)self_ptr;
    if (plugin->host)
    {
        return PI_OK; /* idempotent, like the other test plugins */
    }
    if (host)
    {
        plugin->host = host;
        pi_iunknown_add_ref((IPiUnknown*)host);
    }
    printf("[multiview plugin] initialize (views: alpha, beta)\n");
    return PI_OK;
}

static PiResult PI_CALL Plugin_Terminate(void* self_ptr)
{
    MultiViewPlugin* plugin = (MultiViewPlugin*)self_ptr;

    /* 契约（pi_plugin_base.h）：terminate 之前宿主必须已经 detach + release 了
     * 每一个视图 —— 控件的析构函数在这个模块里。把"还活着几个"报给宿主，
     * 让 --multi-view 用例能把这条契约变成断言，而不是靠注释。 */
    printf("[multiview plugin] terminate (views still alive: %d, created: %u)\n",
           plugin->live_views, (unsigned)plugin->created);
    if (plugin->host)
    {
        pi_plugin_host_post_message(plugin->host, PI_MULTIVIEW_MSG_TERMINATE, 0,
                                    (intptr_t)plugin->live_views);
    }
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */
static PiPluginCapability s_caps[1];
static PiPluginProperty   s_props[2];
static PiPluginDescriptor s_desc;

typedef struct MultiViewFactory {
    PiRefCountedBase base; /* MUST be first */
} MultiViewFactory;

static MultiViewFactory s_factory;
static int             s_initialized = 0;

static uint32_t PI_CALL Factory_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL Factory_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}
static PiResult PI_CALL Factory_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY))
    {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}
static PiPluginDescriptor const* PI_CALL Factory_GetDescriptor(void* self_ptr)
{
    (void)self_ptr;
    return &s_desc;
}
static uint32_t PI_CALL Factory_GetClassCount(void* self_ptr)
{
    (void)self_ptr;
    return 1;
}
static const PiGuid s_class_guid =
    PI_GUID(0x5A1B7C20, 0x0E44, 0x4B93, 0x9C, 0x11, 0x3D, 0x62, 0x77, 0x08, 0xAB, 0x54);

static PiResult PI_CALL Factory_GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid)
{
    (void)self_ptr;
    if (index != 0 || !guid)
    {
        return PI_E_INVALIDARG;
    }
    *guid = s_class_guid;
    return PI_OK;
}
static PiResult PI_CALL Factory_CreateInstance(void* self_ptr, PiGuid const* guid,
                                               IPiPluginHostServices* host, IPiPluginBase** out)
{
    MultiViewPlugin* plugin;

    (void)self_ptr;
    if (!guid || !out)
    {
        return PI_E_INVALIDARG;
    }
    *out = NULL;
    if (!pi_guid_equal(guid, &s_class_guid))
    {
        return PI_E_NOINTERFACE;
    }

    plugin = (MultiViewPlugin*)calloc(1, sizeof(MultiViewPlugin));
    if (!plugin)
    {
        return PI_E_OUTOFMEMORY;
    }
    pi_refcounted_init_with_destroy(&plugin->base, (IPiUnknownVtbl const*)&s_plugin_vtbl,
                                    &Plugin_Destroy);
    if (PI_FAILED(Plugin_Initialize(plugin, host)))
    {
        pi_iunknown_release((IPiUnknown*)&plugin->base);
        return PI_E_UNEXPECTED;
    }
    *out = (IPiPluginBase*)&plugin->base;
    return PI_OK;
}

static IPiPluginFactoryVtbl const s_factory_vtbl = {
    {&Factory_Qi, &Factory_AddRef, &Factory_Release},
    &Factory_GetDescriptor,
    &Factory_GetClassCount,
    &Factory_GetClassGuid,
    &Factory_CreateInstance
};

PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory)
    {
        return PI_E_INVALIDARG;
    }
    *out_factory = NULL;

    if (!s_initialized)
    {
        pi_refcounted_init(&s_factory.base, (IPiUnknownVtbl const*)&s_factory_vtbl);

        pi_plugin_descriptor_init(&s_desc);
        s_desc.name        = "Multi-view Test Plugin";
        s_desc.vendor      = "piplugin";
        s_desc.version     = "1.0.0";
        s_desc.category    = "Test/Views";
        s_desc.api_version = PI_PLUGIN_API_VERSION;

        /* 两视图插件：声明 PROVIDES 的就是"我能给视图"这件事本身（provider），
         * 具体几个视图是 provider 的运行时答案，不是 descriptor 的静态声明。 */
        s_caps[0].iid   = PI_PLUGIN_IID_VIEW_PROVIDER;
        s_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;

        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 1;

        s_props[0].key        = "com.example.kind";
        s_props[0].value      = "multiview-plugin";
        s_props[1].key        = "com.example.views";
        s_props[1].value      = "alpha,beta";
        s_desc.properties     = s_props;
        s_desc.property_count = 2;

        s_initialized = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
