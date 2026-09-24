/*
 * piplugin - Service test plugin (roadmap APP-07)
 *
 * The missing workhorse of the framework's story: a plugin that has no UI at
 * all and is driven by a server-side host. It implements IPiPluginBase (with
 * pi_plugin_get_view() saying "no view") plus IPiPluginService, and declares
 *
 *     PI_PLUGIN_IID_SERVICE  PROVIDES
 *
 * so a host can find it out from the descriptor before it pays for
 * instantiation - which is exactly what a task server wants to filter on.
 *
 * Pure C, no adapters, no third-party dependency: this is also the reference
 * for "how do I implement a second interface on a plugin object" without
 * touching the C++ test plugins. The plugin instance itself carries the
 * IPiPluginBase interface; QueryInterface(PI_PLUGIN_IID_SERVICE) hands out a separate
 * small wrapper object with the service vtable (see ServiceIfc below) - the
 * same containment pattern the framework's own PiPluginDefaultHost uses for
 * IPiPluginHostUI, and the only way to give one object two different vtables without
 * reinterpreting a pointer through the wrong layout.
 *
 * Lifecycle contract this sample implements (docs/design/interfaces.md 2.6):
 *   start(options)  - the "slot" option is REQUIRED; without it the call fails
 *                     with PI_E_MISSINGCAPABILITY, exactly as the header says;
 *                     starting an already-running service is a no-op success;
 *   poll()          - only valid while running (otherwise PI_FAIL); every
 *                     `interval` polls it posts PI_PLUGIN_TEST_MSG_SERVICE_TICK to the
 *                     host so an automated run can see the work happening;
 *   stop()          - IDEMPOTENT and safe before the first start: it always
 *                     posts PI_PLUGIN_TEST_MSG_SERVICE_STOP with a call counter, which
 *                     is how the host proves both the idempotency and the extra
 *                     stop the unload sequence performs;
 *   get_status()    - PI_PLUGIN_SERVICE_STOPPED / RUNNING.
 */
#include <stdlib.h>
#include <string.h>

#include "pi_test_service_protocol.h"
#include "piplugin/pi_plugin.h"

/* 完整随机的 128 位 UUID 风格 class GUID（不是框架保留区里的小整数编号，
 * 见 docs/design/interfaces.md 5.1）。 */
static PiGuid const SERVICE_CLASS_GUID =
    PI_GUID(0x6D24A8F3, 0x51B7, 0x4C0E, 0xA7, 0x39, 0xE2, 0x84, 0x1F, 0x6B, 0xD5, 0x02);

/* --------------------------------------------------------------------------
 * The plugin instance (the IPiPluginBase side) - also where the state lives
 * -------------------------------------------------------------------------- */
typedef struct ServicePlugin {
    PiRefCountedBase       base;     /* MUST be first: this is the IPiPluginBase object */
    IPiPluginHostServices* host;     /* add-ref'd; borrowed by Initialize */
    int32_t                status;   /* PI_SERVICE_* */
    uint32_t               interval; /* report a tick every N polls */
    uint32_t               polls;
    uint32_t               ticks_posted;
    uint32_t               start_calls;
    uint32_t               stop_calls;
    int                    running;
    char                   slot[32]; /* start() option "slot", required */
} ServicePlugin;

/* The IPiPluginService side: a small wrapper that owns a reference to the plugin.
 * COM identity rules want each interface to have its own vtable slot, and the
 * two vtable layouts (IPiPluginBaseVtbl vs IPiPluginServiceVtbl) cannot both live at
 * offset 0 of one object. */
typedef struct ServiceIfc {
    PiRefCountedBase base;  /* MUST be first */
    ServicePlugin*   owner; /* add-ref'd */
} ServiceIfc;

static uint32_t PI_CALL Plugin_AddRef(void* self_ptr);
static uint32_t PI_CALL Plugin_Release(void* self_ptr);
static PiResult PI_CALL Plugin_Qi(void* self_ptr, PiGuid const* iid, void** out);
static PiResult PI_CALL Plugin_Initialize(void* self_ptr, IPiPluginHostServices* host);
static PiResult PI_CALL Plugin_Terminate(void* self_ptr);
static PiResult PI_CALL Plugin_GetView(void* self_ptr, IPiPluginView** out);

static uint32_t PI_CALL Service_AddRef(void* self_ptr);
static uint32_t PI_CALL Service_Release(void* self_ptr);
static PiResult PI_CALL Service_Qi(void* self_ptr, PiGuid const* iid, void** out);
static PiResult PI_CALL Service_Start(void* self_ptr, PiPluginServiceOption const* options,
                                      uint32_t option_count);
static PiResult PI_CALL Service_Stop(void* self_ptr);
static PiResult PI_CALL Service_Poll(void* self_ptr);
static PiResult PI_CALL Service_GetStatus(void* self_ptr, int32_t* out_status);

/* 本模块内的薄封装：直接把框架导出的 pi_refcounted_add_ref/release 的地址填进
 * vtbl，在 C 里会触发 warning C4232（取 dllimport 函数地址不保证跨模块标识）——
 * 与本仓库其它 C 实现用同一种消法，见 docs/design/interfaces.md 5.3。 */
static uint32_t PI_CALL Plugin_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL Plugin_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}
static uint32_t PI_CALL Service_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL Service_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static IPiPluginBaseVtbl const s_plugin_vtbl = {
    {&Plugin_Qi, &Plugin_AddRef, &Plugin_Release},
    &Plugin_Initialize,
    &Plugin_Terminate,
    &Plugin_GetView
};

static IPiPluginServiceVtbl const s_service_vtbl = {
    {&Service_Qi, &Service_AddRef, &Service_Release},
    &Service_Start,
    &Service_Stop,
    &Service_Poll,
    &Service_GetStatus
};

/* --------------------------------------------------------------------------
 * Service helpers (operate on the plugin's state, not on the wrapper)
 * -------------------------------------------------------------------------- */
static void ServicePost(ServicePlugin* plugin, uint32_t msg, uintptr_t wparam)
{
    if (plugin && plugin->host)
    {
        pi_plugin_host_post_message(plugin->host, msg, wparam, 0);
    }
}

static PiResult ServiceDoStart(ServicePlugin*               plugin,
                               PiPluginServiceOption const* options, uint32_t option_count)
{
    char const* slot     = NULL;
    uint32_t    interval = 1;
    uint32_t    i;

    if (!plugin)
    {
        return PI_E_INVALIDARG;
    }

    for (i = 0; i < option_count; ++i)
    {
        if (!options || !options[i].key)
        {
            continue;
        }
        if (strcmp(options[i].key, PI_PLUGIN_TEST_SERVICE_OPTION_SLOT) == 0)
        {
            slot = options[i].value;
        }
        else if (strcmp(options[i].key, PI_PLUGIN_TEST_SERVICE_OPTION_INTERVAL) == 0 &&
                 options[i].value)
        {
            int parsed = atoi(options[i].value);
            interval   = (parsed > 0) ? (uint32_t)parsed : 1u;
        }
    }

    /* 必填选项缺失 —— 头文件承诺的错误码，宿主会断言这一条。 */
    if (!slot || !slot[0])
    {
        return PI_E_MISSINGCAPABILITY;
    }

    /* 已经在跑：幂等成功，不重置计数。 */
    if (plugin->running)
    {
        return PI_OK;
    }

    {
        size_t n = strlen(slot);
        if (n >= sizeof(plugin->slot))
        {
            n = sizeof(plugin->slot) - 1;
        }
        memcpy(plugin->slot, slot, n);
        plugin->slot[n] = '\0';
    }
    plugin->interval     = interval;
    plugin->polls        = 0;
    plugin->ticks_posted = 0;
    plugin->running      = 1;
    plugin->status       = PI_PLUGIN_SERVICE_RUNNING;
    ++plugin->start_calls;
    return PI_OK;
}

static PiResult ServiceDoStop(ServicePlugin* plugin)
{
    if (!plugin)
    {
        return PI_E_INVALIDARG;
    }

    /* 幂等：没 start 过、或者已经停过，调用同样成功（文档承诺）。
     * 每次都报数，宿主因此能区分"调了几次"。 */
    ++plugin->stop_calls;
    ServicePost(plugin, PI_PLUGIN_TEST_MSG_SERVICE_STOP, (uintptr_t)plugin->stop_calls);
    plugin->running = 0;
    plugin->status  = PI_PLUGIN_SERVICE_STOPPED;
    return PI_OK;
}

static PiResult ServiceDoPoll(ServicePlugin* plugin)
{
    if (!plugin)
    {
        return PI_E_INVALIDARG;
    }
    if (!plugin->running || plugin->status != PI_PLUGIN_SERVICE_RUNNING)
    {
        return PI_FAIL; /* 停了的服务不该假装还在干活 */
    }

    ++plugin->polls;
    if (plugin->interval && (plugin->polls % plugin->interval) == 0)
    {
        ++plugin->ticks_posted;
        ServicePost(plugin, PI_PLUGIN_TEST_MSG_SERVICE_TICK, (uintptr_t)plugin->ticks_posted);
    }
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * IPiPluginService slots (self_ptr is the ServiceIfc wrapper)
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Service_Start(void* self_ptr, PiPluginServiceOption const* options,
                                      uint32_t option_count)
{
    ServiceIfc* me = (ServiceIfc*)self_ptr;
    if (!me || !me->owner)
    {
        return PI_E_INVALIDARG;
    }
    return ServiceDoStart(me->owner, options, option_count);
}

static PiResult PI_CALL Service_Stop(void* self_ptr)
{
    ServiceIfc* me = (ServiceIfc*)self_ptr;
    if (!me || !me->owner)
    {
        return PI_E_INVALIDARG;
    }
    return ServiceDoStop(me->owner);
}

static PiResult PI_CALL Service_Poll(void* self_ptr)
{
    ServiceIfc* me = (ServiceIfc*)self_ptr;
    if (!me || !me->owner)
    {
        return PI_E_INVALIDARG;
    }
    return ServiceDoPoll(me->owner);
}

static PiResult PI_CALL Service_GetStatus(void* self_ptr, int32_t* out_status)
{
    ServiceIfc* me = (ServiceIfc*)self_ptr;
    if (!me || !me->owner || !out_status)
    {
        return PI_E_INVALIDARG;
    }
    *out_status = me->owner->status;
    return PI_OK;
}

static PiResult PI_CALL Service_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_PLUGIN_IID_SERVICE))
    {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static void ServiceIfc_Destroy(void* self_ptr)
{
    ServiceIfc* me = (ServiceIfc*)self_ptr;
    if (me->owner)
    {
        /* 包装持有 owner 的引用，所以只要还有人在用这个服务接口，
         * 插件实例就不会消失。 */
        pi_iunknown_release((IPiUnknown*)&me->owner->base);
        me->owner = NULL;
    }
    free(me);
}

/* --------------------------------------------------------------------------
 * IPiPluginBase slots (self_ptr is the ServicePlugin instance)
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Plugin_Initialize(void* self_ptr, IPiPluginHostServices* host)
{
    ServicePlugin* me = (ServicePlugin*)self_ptr;

    if (me->host)
    {
        return PI_OK; /* 幂等（见 docs/tutorial/write-plugin.md） */
    }
    if (!host)
    {
        return PI_OK;
    }

    /* host 是借用入参：要留住就必须自己 add-ref（冻结约定 2.3）。 */
    me->host = host;
    pi_iunknown_add_ref((IPiUnknown*)host);
    return PI_OK;
}

static PiResult PI_CALL Plugin_Terminate(void* self_ptr)
{
    ServicePlugin* me = (ServicePlugin*)self_ptr;

    /* 卸载序列会先 stop 服务再 terminate；这里再兜一次，只有"还在跑"才动手，
     * 免得给宿主多报一次 stop。 */
    if (me && me->running)
    {
        ServiceDoStop(me);
    }
    return PI_OK;
}

static PiResult PI_CALL Plugin_GetView(void* self_ptr, IPiPluginView** out)
{
    (void)self_ptr;
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    *out = NULL;
    return PI_E_NOINTERFACE; /* 纯服务插件：没有任何 UI */
}

static PiResult PI_CALL Plugin_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    ServicePlugin* me = (ServicePlugin*)self_ptr;

    if (!out)
    {
        return PI_E_INVALIDARG;
    }

    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE))
    {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }

    if (pi_guid_equal(iid, &PI_PLUGIN_IID_SERVICE))
    {
        /* 交出一个独立的服务接口包装（见文件头说明）。每次 QI 新建一个 ——
         * 与框架自己的 IPiPluginHostUI 包装行为一致（freeze review F5 记录在案）。 */
        ServiceIfc* svc = (ServiceIfc*)calloc(1, sizeof(ServiceIfc));
        if (!svc)
        {
            return PI_E_OUTOFMEMORY;
        }
        pi_refcounted_init_with_destroy(&svc->base, (IPiUnknownVtbl const*)&s_service_vtbl,
                                        &ServiceIfc_Destroy);
        svc->owner = me;
        pi_refcounted_add_ref((IPiUnknown*)&me->base);
        *out = &svc->base;
        return PI_OK;
    }

    *out = NULL;
    return PI_E_NOINTERFACE;
}

static void Plugin_Destroy(void* self_ptr)
{
    ServicePlugin* me = (ServicePlugin*)self_ptr;
    if (me->host)
    {
        pi_iunknown_release((IPiUnknown*)me->host);
        me->host = NULL;
    }
    free(me);
}

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */
typedef struct ServiceFactory {
    PiRefCountedBase base; /* MUST be first */
} ServiceFactory;

static ServiceFactory     s_factory;
static PiPluginCapability s_caps[1];
static PiPluginProperty   s_props[2];
static PiPluginDescriptor s_desc;
static int                s_initialized = 0;

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
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY))
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

static PiResult PI_CALL Factory_GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid)
{
    (void)self_ptr;
    if (index != 0 || !guid)
    {
        return PI_E_INVALIDARG;
    }
    *guid = SERVICE_CLASS_GUID;
    return PI_OK;
}

static PiResult PI_CALL Factory_CreateInstance(void* self_ptr, PiGuid const* guid,
                                               IPiPluginHostServices* host, IPiPluginBase** out)
{
    ServicePlugin* plugin;

    (void)self_ptr;
    if (!guid || !out)
    {
        return PI_E_INVALIDARG;
    }
    *out = NULL;
    if (!pi_guid_equal(guid, &SERVICE_CLASS_GUID))
    {
        return PI_E_NOINTERFACE;
    }

    plugin = (ServicePlugin*)calloc(1, sizeof(ServicePlugin));
    if (!plugin)
    {
        return PI_E_OUTOFMEMORY;
    }

    pi_refcounted_init_with_destroy(&plugin->base, (IPiUnknownVtbl const*)&s_plugin_vtbl,
                                    &Plugin_Destroy);
    plugin->status = PI_PLUGIN_SERVICE_STOPPED;

    /* 宿主随后还会调用 pi_plugin_initialize（生命周期里的一步）；这里先初始化一次，
     * 与两个 GUI 测试插件保持同一种写法，Initialize 本身是幂等的。 */
    if (PI_FAILED(Plugin_Initialize(plugin, host)))
    {
        pi_iunknown_release((IPiUnknown*)&plugin->base); /* 归零 -> Plugin_Destroy */
        return PI_FAIL;
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

/* --------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------- */
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

        /* 只声明一条能力：本插件提供一个服务。没有任何 REQUIRED 能力 ——
         * 于是任何宿主（GUI 的、无头的）都能加载它，无头宿主正是它存在的理由。 */
        s_caps[0].iid   = PI_PLUGIN_IID_SERVICE;
        s_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;

        s_desc.name             = "Service Test Plugin";
        s_desc.vendor           = "piplugin";
        s_desc.version          = "1.0.0";
        s_desc.category         = "Service/Test";
        s_desc.api_version      = PI_PLUGIN_API_VERSION;
        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 1;

        /* 自由元数据（roadmap APP-04）：纯 C 插件声明属性的写法就这几行。
         * `pi.` 前缀是框架保留区，所以用自有前缀。 */
        s_props[0].key        = "com.example.kind";
        s_props[0].value      = "service-plugin";
        s_props[1].key        = "com.example.service.kind";
        s_props[1].value      = "task-server";
        s_desc.properties     = s_props;
        s_desc.property_count = 2;
        s_initialized         = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
