/*
 * piplugin - Host kit L0: PiPluginHostSession (implementation)
 *
 * 设计纪律见 pi_host_session.h 与 host_kits/README.md。
 * 本文件只做机制：加载、门禁、实例化、多槽位、多视图账本、七步卸载；不碰任何 UI 决策。
 */
#include "pi_host_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal types
 * -------------------------------------------------------------------------- */

/* 宿主已经从 provider 取过引用（或正在使用的）一个视图。
 *
 * id 必须是**副本**：provider 的 PiPluginViewInfo::id 是借用字符串，插件下一次
 * pi_plugin_views() 就可能让它失效（或者整块换掉）。本层的表要活到卸载，
 * 所以自己存一份。 */
typedef struct SlotView {
    char*          id;      /* malloc 出来的副本；NULL 表示空槽 */
    IPiPluginView* view;    /* add-ref'd */
    int            attached;
    PiNativeWindow attach_window;
} SlotView;

typedef struct PiPluginHostSessionSlot {
    PiPluginModule*           module;     /* loaded module (owns the code) */
    IPiPluginFactory*         factory;    /* add-ref'd at load              */
    IPiPluginBase*            plugin;     /* add-ref'd at instantiate       */
    IPiPluginViewProvider*    provider;   /* add-ref'd; NULL = no GUI       */
    SlotView                  views[PI_PLUGIN_HOST_SESSION_MAX_VIEWS_PER_SLOT];
    uint32_t                  view_count; /* dense: views[0..view_count-1]  */
    IPiPluginService*         service;    /* add-ref'd; may be NULL         */
    IPiPluginEventSink*       event_sink; /* add-ref'd; may be NULL (APP-06) */
    PiPluginDescriptor const* descriptor; /* borrowed from the factory      */
    int                       in_use;
} PiPluginHostSessionSlot;

struct PiPluginHostSession {
    IPiPluginHostServices*     services;    /* add-ref'd */
    IPiPluginHostEvents*       host_events; /* add-ref'd; may be NULL (APP-06) */
    PiPluginHostSessionSlot    slots[PI_PLUGIN_HOST_SESSION_MAX_SLOTS];
    PiGuid                     required[PI_PLUGIN_HOST_SESSION_MAX_SLOTS];
    uint32_t                   required_count;
    int                        skip_detach;
    PiPluginHostSessionLogProc log;
    void*                      log_user;
    char                       last_error[PI_PLUGIN_HOST_SESSION_ERROR_MAX];
};

/* --------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------- */
static void SessionSetError(PiPluginHostSession* session, char const* fmt, ...)
{
    va_list ap;
    if (!session)
    {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(session->last_error, sizeof(session->last_error), fmt, ap);
    va_end(ap);
}

static void SessionLog(PiPluginHostSession* session, char const* fmt, ...)
{
    char    buf[PI_PLUGIN_HOST_SESSION_ERROR_MAX + 192];
    va_list ap;
    if (!session || !session->log)
    {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    session->log(session->log_user, buf);
}

/* --------------------------------------------------------------------------
 * Slot bookkeeping
 * -------------------------------------------------------------------------- */
static int SessionFindFreeSlot(PiPluginHostSession const* session)
{
    uint32_t i;
    for (i = 0; i < PI_PLUGIN_HOST_SESSION_MAX_SLOTS; ++i)
    {
        if (!session->slots[i].in_use)
        {
            return (int)i;
        }
    }
    return -1;
}

static PiPluginHostSessionSlot* SessionSlotAt(PiPluginHostSession* session, uint32_t slot)
{
    if (!session || slot >= PI_PLUGIN_HOST_SESSION_MAX_SLOTS)
    {
        return NULL;
    }
    if (!session->slots[slot].in_use)
    {
        return NULL;
    }
    return &session->slots[slot];
}

/* 自己复制一份 id：provider 的字符串是借用的，且不保证 NUL 之外的长度。
 * 超过 PI_PLUGIN_VIEW_ID_MAX 的部分截断（框架说"更长会被宿主截断，不是拒绝"）。 */
static char* SessionCopyId(char const* id)
{
    size_t n;
    char*  copy;

    if (!id || !id[0])
    {
        return NULL;
    }
    n = strlen(id);
    if (n >= PI_PLUGIN_VIEW_ID_MAX)
    {
        n = PI_PLUGIN_VIEW_ID_MAX - 1;
    }
    copy = (char*)malloc(n + 1);
    if (!copy)
    {
        return NULL;
    }
    memcpy(copy, id, n);
    copy[n] = '\0';
    return copy;
}

/* 在槽位的视图表里按 id 找；找不到返回 -1。 */
static int SessionFindView(PiPluginHostSessionSlot const* slot, char const* view_id)
{
    uint32_t i;
    if (!view_id || !view_id[0])
    {
        return -1;
    }
    for (i = 0; i < slot->view_count; ++i)
    {
        if (slot->views[i].id && strcmp(slot->views[i].id, view_id) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

/* 视图表里最后一个条目。表是 dense 的，卸载时从尾部一个个放掉。 */
static void SessionClearViews(PiPluginHostSession* session, PiPluginHostSessionSlot* slot,
                              uint32_t index, char const* tag)
{
    /* detach 必须先于 release，且两者都必须先于模块卸载：插件的控件/窗口对象
     * 的析构函数在插件模块里，模块一卸就是调到已释放内存。 */
    while (slot->view_count > 0)
    {
        SlotView* sv = &slot->views[slot->view_count - 1];

        if (sv->attached && !session->skip_detach)
        {
            SessionLog(session, "%s[%u]: detach view '%s'", tag, index, sv->id ? sv->id : "?");
            pi_plugin_view_detach(sv->view);
        }
        else if (sv->attached)
        {
            SessionLog(session, "%s[%u]: SKIPPING detach (diagnostic) for view '%s'", tag, index,
                       sv->id ? sv->id : "?");
        }
        SessionLog(session, "%s[%u]: release view '%s'", tag, index, sv->id ? sv->id : "?");
        pi_iunknown_release((IPiUnknown*)sv->view);
        free(sv->id);
        memset(sv, 0, sizeof(*sv));
        --slot->view_count;
    }
}

/* 七步卸载序列（也用于加载失败时的就地回卷）。
 * 释放顺序不可调换：插件/视图的代码在模块里，任何一步先卸模块都是 UB。 */
static void SessionTearDownSlot(PiPluginHostSession* session, PiPluginHostSessionSlot* slot,
                                uint32_t index, char const* tag)
{
    if (!slot)
    {
        return;
    }

    /* 1) 视图（一个槽位 N 个）：每个都 detach + release */
    SessionClearViews(session, slot, index, tag);

    /* 2) provider：插件实例上的能力对象，必须在 terminate / 模块卸载之前放掉 */
    if (slot->provider)
    {
        SessionLog(session, "%s[%u]: release view provider", tag, index);
        pi_iunknown_release((IPiUnknown*)slot->provider);
        slot->provider = NULL;
    }

    /* 3) 事件（APP-06）：先让宿主按 owner 退订，再放掉 sink。
     *
     * 顺序是这一层存在的意义：owner 就是插件实例指针，退订必须在插件代码还映射着
     * 的时候做完；sink 的引用也必须在模块卸载前放掉。两件事都由 kit 做，宿主
     * 手写就会漏 —— 漏了就是"插件卸载后回调进已卸载内存"。 */
    if (session->host_events && slot->plugin)
    {
        SessionLog(session, "%s[%u]: drop event subscriptions", tag, index);
        pi_plugin_host_events_drop_owner(session->host_events, (void*)slot->plugin);
    }
    if (slot->event_sink)
    {
        SessionLog(session, "%s[%u]: release event sink", tag, index);
        pi_iunknown_release((IPiUnknown*)slot->event_sink);
        slot->event_sink = NULL;
    }

    /* 4) 服务：可能还在别的线程上跑 I/O，先停再释放 */
    if (slot->service)
    {
        SessionLog(session, "%s[%u]: service stop", tag, index);
        pi_plugin_service_stop(slot->service);
        pi_iunknown_release((IPiUnknown*)slot->service);
        slot->service = NULL;
    }

    /* 5) 插件实例：terminate 再 release —— 此刻插件代码必须仍然 mapped。
     *    到这一步所有视图都已经 detach + release，与 pi_plugin_base.h 里
     *    terminate 的契约（"所有视图都已归还"）对得上。 */
    if (slot->plugin)
    {
        SessionLog(session, "%s[%u]: terminate plugin", tag, index);
        pi_plugin_terminate(slot->plugin);
        pi_iunknown_release((IPiUnknown*)slot->plugin);
        slot->plugin = NULL;
    }

    /* 6) 工厂 */
    if (slot->factory)
    {
        pi_iunknown_release((IPiUnknown*)slot->factory);
        slot->factory = NULL;
    }

    /* 7) 模块 */
    if (slot->module)
    {
        SessionLog(session, "%s[%u]: unload module", tag, index);
        pi_plugin_module_unload(slot->module);
        slot->module = NULL;
    }

    /* 8) descriptor 属于模块，随模块失效 */
    slot->descriptor = NULL;
    slot->view_count = 0;
    slot->in_use     = 0;
}

/* --------------------------------------------------------------------------
 * Capability gates (both directions)
 * -------------------------------------------------------------------------- */

/* 方向一：插件 REQUIRE 的能力，宿主必须有 —— 用 QI 探测宿主服务对象。
 * 通用化了测试宿主原先只探 PI_PLUGIN_IID_HOST_UI 的一次性检查：任何 REQUIRED 能力
 * 宿主给不出，就拒绝实例化（headless 宿主拒绝 GUI 插件正是这条）。 */
static PiResult SessionGatePluginRequirements(PiPluginHostSession*      session,
                                              PiPluginDescriptor const* desc)
{
    uint32_t i;
    if (!desc || !desc->capabilities || desc->capability_count == 0)
    {
        return PI_OK;
    }

    for (i = 0; i < desc->capability_count; ++i)
    {
        PiPluginCapability const* cap   = &desc->capabilities[i];
        void*                     probe = NULL;
        PiResult                  hr;

        if (!(cap->flags & PI_PLUGIN_CAP_REQUIRED))
        {
            continue;
        }

        hr = pi_iunknown_query_interface((IPiUnknown*)session->services, &cap->iid, &probe);
        if (PI_FAILED(hr) || !probe)
        {
            SessionSetError(session,
                            "plugin requires capability iid data1=0x%08X but this host does not provide it",
                            (unsigned)cap->iid.data1);
            return PI_E_MISSINGCAPABILITY;
        }
        pi_iunknown_release((IPiUnknown*)probe);
    }
    return PI_OK;
}

/* 方向二：宿主生态要求的能力，插件必须 PROVIDES —— app 定义的特化协议门禁。
 * session 未声明任何要求时是空操作。 */
static PiResult SessionGateHostRequirements(PiPluginHostSession*      session,
                                            PiPluginDescriptor const* desc)
{
    uint32_t i;
    for (i = 0; i < session->required_count; ++i)
    {
        if (!desc || !pi_plugin_descriptor_provides(desc, &session->required[i]))
        {
            SessionSetError(session,
                            "plugin does not provide iid data1=0x%08X required by this host",
                            (unsigned)session->required[i].data1);
            return PI_E_MISSINGCAPABILITY;
        }
    }
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
PiResult pi_plugin_host_session_create(IPiPluginHostServices* services,
                                       PiPluginHostSession**  out_session)
{
    PiPluginHostSession* session;

    if (!services || !out_session)
    {
        return PI_E_INVALIDARG;
    }
    *out_session = NULL;

    session = (PiPluginHostSession*)calloc(1, sizeof(PiPluginHostSession));
    if (!session)
    {
        return PI_E_OUTOFMEMORY;
    }

    session->services = services;
    pi_iunknown_add_ref((IPiUnknown*)services);

    /* 宿主可选提供事件接口（APP-06）：有就持有（借给宿主用），没有就一直是 NULL。
     * 宿主通常用 pi_plugin_host_services_create_ex() 的 extra_qi 钩子把路由器挂上（APP-01）——
     * 这里走的就是插件将来会走的那条 QI 路径。（此处还没有 logger，故不写日志。） */
    if (PI_FAILED(pi_plugin_host_events_query(services, &session->host_events)))
    {
        session->host_events = NULL;
    }

    *out_session = session;
    return PI_OK;
}

void pi_plugin_host_session_destroy(PiPluginHostSession* session)
{
    uint32_t i;
    if (!session)
    {
        return;
    }

    for (i = 0; i < PI_PLUGIN_HOST_SESSION_MAX_SLOTS; ++i)
    {
        PiPluginHostSessionSlot* slot = &session->slots[i];
        if (slot->in_use || slot->module || slot->plugin || slot->provider ||
            slot->view_count > 0 || slot->event_sink)
        {
            SessionTearDownSlot(session, slot, i, "destroy");
        }
    }

    if (session->host_events)
    {
        pi_iunknown_release((IPiUnknown*)session->host_events);
        session->host_events = NULL;
    }
    if (session->services)
    {
        pi_iunknown_release((IPiUnknown*)session->services);
        session->services = NULL;
    }
    free(session);
}

PiResult pi_plugin_host_session_require(PiPluginHostSession* session, PiGuid const* iid)
{
    uint32_t i;
    if (!session || !iid)
    {
        return PI_E_INVALIDARG;
    }

    for (i = 0; i < session->required_count; ++i)
    {
        if (pi_guid_equal(&session->required[i], iid))
        {
            return PI_OK; /* idempotent */
        }
    }
    if (session->required_count >= PI_PLUGIN_HOST_SESSION_MAX_SLOTS)
    {
        SessionSetError(session, "too many host requirements (max %u)",
                        (unsigned)PI_PLUGIN_HOST_SESSION_MAX_SLOTS);
        return PI_E_OUTOFMEMORY;
    }
    session->required[session->required_count++] = *iid;
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Load / inspect / instantiate
 * -------------------------------------------------------------------------- */
PiResult pi_plugin_host_session_inspect(PiPluginHostSession* session,
                                        char const*          dll_path,
                                        uint32_t*            out_slot)
{
    int                       index;
    PiPluginHostSessionSlot*  slot;
    IPiPluginFactory*         factory = NULL;
    PiPluginDescriptor const* desc    = NULL;
    PiResult                  hr;

    if (!session || !dll_path || !out_slot)
    {
        return PI_E_INVALIDARG;
    }
    *out_slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;

    index = SessionFindFreeSlot(session);
    if (index < 0)
    {
        SessionSetError(session, "session slot table is full (%u slots)",
                        (unsigned)PI_PLUGIN_HOST_SESSION_MAX_SLOTS);
        return PI_E_UNEXPECTED;
    }
    slot = &session->slots[index];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1; /* 先占住槽位：任何失败都走同一条回卷路径 */

    SessionLog(session, "load[%d]: %s", index, dll_path);

    slot->module = pi_plugin_module_load(dll_path);
    if (!slot->module)
    {
        SessionSetError(session, "pi_plugin_module_load failed: %s", pi_plugin_module_get_load_error());
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return PI_E_NOTFOUND;
    }

    hr = pi_plugin_module_get_factory(slot->module, &factory);
    if (PI_FAILED(hr) || !factory)
    {
        SessionSetError(session, "pi_plugin_module_get_factory failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return PI_FAILED(hr) ? hr : PI_E_UNEXPECTED;
    }
    slot->factory = factory;

    pi_plugin_factory_get_descriptor(slot->factory, &desc);
    slot->descriptor = desc;

    /* 版本门禁（roadmap BLK-03）：插件声明的 api_version 必须与宿主兼容。
     * 与能力门禁一样在**实例化之前**判定，所以不兼容的插件连实例都不会被创建。 */
    if (desc && !pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION, desc->api_version))
    {
        SessionSetError(session,
                        "plugin api_version 0x%08X (major %u, minor %u) is incompatible with host 0x%08X (major %u, minor %u); the plugin must not be newer than the host",
                        (unsigned)desc->api_version,
                        (unsigned)PI_PLUGIN_API_VERSION_MAJOR(desc->api_version),
                        (unsigned)PI_PLUGIN_API_VERSION_MINOR(desc->api_version),
                        (unsigned)PI_PLUGIN_API_VERSION,
                        (unsigned)PI_PLUGIN_API_VERSION_MAJOR(PI_PLUGIN_API_VERSION),
                        (unsigned)PI_PLUGIN_API_VERSION_MINOR(PI_PLUGIN_API_VERSION));
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return PI_E_VERSIONMISMATCH;
    }

    /* 双向门禁：都要在实例化之前 */
    hr = SessionGatePluginRequirements(session, desc);
    if (PI_FAILED(hr))
    {
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return hr;
    }
    hr = SessionGateHostRequirements(session, desc);
    if (PI_FAILED(hr))
    {
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return hr;
    }

    SessionLog(session, "load[%d]: gate passed (category=%s, capabilities=%u)",
               index, (desc && desc->category) ? desc->category : "-",
               (unsigned)(desc ? desc->capability_count : 0u));

    *out_slot = (uint32_t)index;
    return PI_OK;
}

PiResult pi_plugin_host_session_instantiate(PiPluginHostSession* session, uint32_t slot_index)
{
    PiPluginHostSessionSlot* slot;
    PiGuid                   class_guid;
    uint32_t                 class_count;
    PiResult                 hr;
    uint32_t                 view_count = 0;

    if (!session)
    {
        return PI_E_INVALIDARG;
    }
    slot = SessionSlotAt(session, slot_index);
    if (!slot)
    {
        return PI_E_INVALIDARG;
    }

    if (slot->plugin)
    {
        SessionSetError(session, "slot %u is already instantiated", (unsigned)slot_index);
        return PI_E_UNEXPECTED;
    }
    if (!slot->factory)
    {
        SessionSetError(session, "slot %u has no factory (inspect first)", (unsigned)slot_index);
        return PI_E_UNEXPECTED;
    }

    class_count = pi_plugin_factory_get_class_count(slot->factory);
    if (class_count == 0)
    {
        SessionSetError(session, "plugin exposes no classes");
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return PI_E_NOTFOUND;
    }

    hr = pi_plugin_factory_get_class_guid(slot->factory, 0, &class_guid);
    if (PI_FAILED(hr))
    {
        SessionSetError(session, "pi_plugin_factory_get_class_guid(0) failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return hr;
    }

    hr = pi_plugin_factory_create_instance(slot->factory, &class_guid, session->services, &slot->plugin);
    if (PI_FAILED(hr) || !slot->plugin)
    {
        SessionSetError(session, "pi_plugin_factory_create_instance failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return PI_FAILED(hr) ? hr : PI_E_UNEXPECTED;
    }

    hr = pi_plugin_initialize(slot->plugin, session->services);
    if (PI_FAILED(hr))
    {
        SessionSetError(session, "pi_plugin_initialize failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return hr;
    }

    /* 视图能力：不是"取视图"，而是"取 provider"。headless 插件不支持
     * PI_PLUGIN_IID_VIEW_PROVIDER 就是 QI 失败 —— 与 service / event sink
     * 一样是能力协商的常态，不是错误。具体要哪个视图由宿主按 id 后取。 */
    {
        IPiPluginViewProvider* provider = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)slot->plugin,
                                                      &PI_PLUGIN_IID_VIEW_PROVIDER, (void**)&provider)))
        {
            slot->provider = provider;
            /* 顺手枚举一次只为了日志：声明了 PROVIDES 却一个视图都不给，
             * 是插件自己的 bug，宿主看日志能立刻分辨"没有界面"与"有界面但空"。 */
            {
                PiPluginViewInfo const* views = NULL;
                uint32_t                count = 0;
                if (PI_SUCCEEDED(pi_plugin_views(provider, &views, &count)))
                {
                    view_count = count;
                }
            }
        }
    }
    {
        IPiPluginService* service = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)slot->plugin,
                                                     &PI_PLUGIN_IID_SERVICE, (void**)&service)))
        {
            slot->service = service;
        }
    }
    {
        /* 事件 sink（APP-06）：插件可选实现，没有就是没有 —— 与 view/service 一样
         * 是能力协商的常态，不是错误。卸载序列会按正确的时机释放它。 */
        IPiPluginEventSink* sink = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)slot->plugin,
                                                     &PI_PLUGIN_IID_EVENT_SINK, (void**)&sink)))
        {
            slot->event_sink = sink;
        }
    }

    SessionLog(session, "load[%u]: ready (views:%u service:%s events:%s)",
               (unsigned)slot_index, (unsigned)view_count,
               slot->service ? "Y" : "N", slot->event_sink ? "Y" : "N");
    return PI_OK;
}

PiResult pi_plugin_host_session_load(PiPluginHostSession* session,
                                     char const*          dll_path,
                                     uint32_t*            out_slot)
{
    uint32_t slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
    PiResult hr;

    if (!session || !dll_path)
    {
        return PI_E_INVALIDARG;
    }
    if (out_slot)
    {
        *out_slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
    }

    hr = pi_plugin_host_session_inspect(session, dll_path, &slot);
    if (PI_FAILED(hr))
    {
        return hr;
    }

    hr = pi_plugin_host_session_instantiate(session, slot);
    if (PI_FAILED(hr))
    {
        /* instantiate 内部已回卷，这里只需把槽位下标还回去 */
        if (out_slot)
        {
            *out_slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
        }
        return hr;
    }

    if (out_slot)
    {
        *out_slot = slot;
    }
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Slot queries
 * -------------------------------------------------------------------------- */
uint32_t pi_plugin_host_session_count(PiPluginHostSession const* session)
{
    uint32_t i, n = 0;
    if (!session)
    {
        return 0;
    }
    for (i = 0; i < PI_PLUGIN_HOST_SESSION_MAX_SLOTS; ++i)
    {
        if (session->slots[i].in_use)
        {
            ++n;
        }
    }
    return n;
}

int pi_plugin_host_session_is_loaded(PiPluginHostSession const* session, uint32_t slot)
{
    if (!session || slot >= PI_PLUGIN_HOST_SESSION_MAX_SLOTS)
    {
        return 0;
    }
    return session->slots[slot].in_use ? 1 : 0;
}

IPiPluginBase* pi_plugin_host_session_get_plugin(PiPluginHostSession* session, uint32_t slot)
{
    PiPluginHostSessionSlot* s = SessionSlotAt(session, slot);
    return s ? s->plugin : NULL;
}

IPiPluginService* pi_plugin_host_session_get_service(PiPluginHostSession* session, uint32_t slot)
{
    PiPluginHostSessionSlot* s = SessionSlotAt(session, slot);
    return s ? s->service : NULL;
}

PiPluginDescriptor const* pi_plugin_host_session_get_descriptor(PiPluginHostSession const* session,
                                                                uint32_t                   slot)
{
    if (!session || slot >= PI_PLUGIN_HOST_SESSION_MAX_SLOTS)
    {
        return NULL;
    }
    if (!session->slots[slot].in_use)
    {
        return NULL;
    }
    return session->slots[slot].descriptor;
}

/* --------------------------------------------------------------------------
 * Events (APP-06, channel C)
 * -------------------------------------------------------------------------- */
int pi_plugin_host_session_has_event_sink(PiPluginHostSession const* session, uint32_t slot)
{
    if (!session || slot >= PI_PLUGIN_HOST_SESSION_MAX_SLOTS)
    {
        return 0;
    }
    return session->slots[slot].event_sink ? 1 : 0;
}

PiResult pi_plugin_host_session_deliver_event(PiPluginHostSession* session, uint32_t slot,
                                              PiPluginEvent const* event)
{
    PiPluginHostSessionSlot* s;

    if (!session || !event)
    {
        return PI_E_INVALIDARG;
    }
    s = SessionSlotAt(session, slot);
    if (!s || !s->plugin)
    {
        return PI_E_NOINTERFACE;
    }
    if (!s->event_sink)
    {
        /* 插件没实现 sink：这是"优雅降级"的那条路径，调用方跳过即可，不是错误。
         * 记一行日志便于诊断（有些宿主会想知道自己的事件没人收）。 */
        SessionLog(session, "events[%u]: plugin has no sink, event '%s' skipped",
                   (unsigned)slot, event->topic ? event->topic : "(null)");
        return PI_E_NOINTERFACE;
    }
    return pi_plugin_event_deliver(s->event_sink, event);
}

IPiPluginHostEvents* pi_plugin_host_session_get_host_events(PiPluginHostSession* session)
{
    return session ? session->host_events : NULL;
}

/* --------------------------------------------------------------------------
 * Views: enumerate / take / attach / pump
 * -------------------------------------------------------------------------- */
PiResult pi_plugin_host_session_views(PiPluginHostSession* session, uint32_t slot_index,
                                      PiPluginViewInfo const** out_views, uint32_t* out_count)
{
    PiPluginHostSessionSlot* slot;

    if (out_views)
    {
        *out_views = NULL;
    }
    if (out_count)
    {
        *out_count = 0;
    }
    if (!session || !out_views || !out_count)
    {
        return PI_E_INVALIDARG;
    }
    slot = SessionSlotAt(session, slot_index);
    if (!slot)
    {
        return PI_E_INVALIDARG;
    }
    if (!slot->provider)
    {
        SessionSetError(session, "slot %u has no view provider (headless plugin?)",
                        (unsigned)slot_index);
        return PI_E_NOINTERFACE;
    }
    /* 直接转发，不缓存：集合会变，宿主每帧重读；插件给的数组只在这次调用内有效。 */
    return pi_plugin_views(slot->provider, out_views, out_count);
}

IPiPluginViewProvider* pi_plugin_host_session_get_view_provider(PiPluginHostSession* session,
                                                               uint32_t             slot)
{
    PiPluginHostSessionSlot* s = SessionSlotAt(session, slot);
    return s ? s->provider : NULL;
}

IPiPluginView* pi_plugin_host_session_get_view(PiPluginHostSession* session, uint32_t slot_index,
                                               char const* view_id)
{
    PiPluginHostSessionSlot* slot;
    SlotView*                sv;
    IPiPluginView*           view = NULL;
    char*                    id_copy;
    int                      index;
    PiResult                 hr;

    if (!view_id || !view_id[0])
    {
        return NULL;
    }
    slot = SessionSlotAt(session, slot_index);
    if (!slot)
    {
        return NULL;
    }

    index = SessionFindView(slot, view_id);
    if (index >= 0)
    {
        return slot->views[index].view; /* 已经取过：同一个指针，宿主不用管引用 */
    }
    if (!slot->provider)
    {
        SessionSetError(session, "slot %u has no view provider (headless plugin?)",
                        (unsigned)slot_index);
        return NULL;
    }
    if (slot->view_count >= PI_PLUGIN_HOST_SESSION_MAX_VIEWS_PER_SLOT)
    {
        SessionSetError(session, "slot %u already holds %u views (kit table limit)",
                        (unsigned)slot_index, (unsigned)PI_PLUGIN_HOST_SESSION_MAX_VIEWS_PER_SLOT);
        return NULL;
    }

    hr = pi_plugin_get_view(slot->provider, view_id, &view);
    if (PI_FAILED(hr) || !view)
    {
        SessionSetError(session, "slot %u has no view '%s' (hr=%d)", (unsigned)slot_index, view_id,
                        (int)hr);
        return NULL;
    }

    id_copy = SessionCopyId(view_id);
    if (!id_copy)
    {
        pi_iunknown_release((IPiUnknown*)view);
        SessionSetError(session, "out of memory while remembering view id '%s'", view_id);
        return NULL;
    }

    sv                  = &slot->views[slot->view_count++];
    sv->id              = id_copy;
    sv->view            = view;
    sv->attached        = 0;
    sv->attach_window   = PI_INVALID_WINDOW;
    SessionLog(session, "view[%u]: took '%s'", (unsigned)slot_index, id_copy);
    return view;
}

PiResult pi_plugin_host_session_attach_view(PiPluginHostSession* session, uint32_t slot_index,
                                            char const* view_id, PiNativeWindow parent_window,
                                            int set_visible)
{
    PiPluginHostSessionSlot* slot;
    IPiPluginView*           view;
    SlotView*                sv;
    int                      index;
    PiResult                 hr;

    if (!session)
    {
        return PI_E_INVALIDARG;
    }
    slot = SessionSlotAt(session, slot_index);
    if (!slot)
    {
        return PI_E_INVALIDARG;
    }
    if (!PI_IS_VALID_WINDOW(parent_window))
    {
        return PI_E_INVALIDARG;
    }
    if (!view_id || !view_id[0])
    {
        return PI_E_INVALIDARG;
    }

    view = pi_plugin_host_session_get_view(session, slot_index, view_id);
    if (!view)
    {
        /* headless 与"id 不存在"是两件事，调用方据此决定是自己错了还是插件没界面 */
        if (!slot->provider)
        {
            SessionSetError(session, "slot %u has no view provider (headless plugin?)",
                            (unsigned)slot_index);
            return PI_E_NOINTERFACE;
        }
        return PI_E_NOTFOUND;
    }

    index = SessionFindView(slot, view_id);
    sv    = (index >= 0) ? &slot->views[index] : NULL;
    if (!sv)
    {
        return PI_E_NOTFOUND; /* 理论上不可达：get_view 刚刚把表补齐 */
    }
    if (sv->attached)
    {
        SessionSetError(session, "slot %u view '%s' is already attached", (unsigned)slot_index,
                        view_id);
        return PI_E_UNEXPECTED;
    }

    hr = pi_plugin_view_attach(view, parent_window);
    if (PI_FAILED(hr))
    {
        SessionSetError(session, "pi_plugin_view_attach('%s') failed (hr=%d)", view_id, (int)hr);
        return hr;
    }
    sv->attached      = 1;
    sv->attach_window = parent_window;

    if (set_visible)
    {
        pi_plugin_view_set_visible(view, 1);
    }

    SessionLog(session, "attach[%u]: view '%s' plugin window=0x%llx container=0x%llx",
               (unsigned)slot_index, view_id,
               (unsigned long long)(uintptr_t)pi_plugin_view_get_native_window(view),
               (unsigned long long)(uintptr_t)parent_window);
    return PI_OK;
}

PiResult pi_plugin_host_session_detach_view(PiPluginHostSession* session, uint32_t slot_index,
                                            char const* view_id)
{
    PiPluginHostSessionSlot* slot;
    SlotView*                sv;
    int                      index;
    PiResult                 hr;

    slot = SessionSlotAt(session, slot_index);
    if (!slot || !view_id || !view_id[0])
    {
        return PI_E_INVALIDARG;
    }
    index = SessionFindView(slot, view_id);
    if (index < 0)
    {
        SessionSetError(session, "slot %u has no view '%s'", (unsigned)slot_index, view_id);
        return PI_E_NOTFOUND;
    }
    sv = &slot->views[index];
    if (!sv->attached)
    {
        return PI_OK; /* 无操作：宿主可以"确保它已摘下" */
    }

    hr = pi_plugin_view_detach(sv->view);
    if (PI_FAILED(hr))
    {
        SessionSetError(session, "pi_plugin_view_detach('%s') failed (hr=%d)", view_id, (int)hr);
        return hr;
    }
    sv->attached      = 0;
    sv->attach_window = PI_INVALID_WINDOW;
    SessionLog(session, "detach[%u]: view '%s' from container 0x%llx", (unsigned)slot_index,
               view_id, (unsigned long long)(uintptr_t)sv->attach_window);
    return PI_OK;
}

void pi_plugin_host_session_drive_idle(PiPluginHostSession* session)
{
    uint32_t i, v;
    if (!session)
    {
        return;
    }
    for (i = 0; i < PI_PLUGIN_HOST_SESSION_MAX_SLOTS; ++i)
    {
        PiPluginHostSessionSlot* slot = &session->slots[i];
        if (!slot->in_use)
        {
            continue;
        }
        for (v = 0; v < slot->view_count; ++v)
        {
            pi_plugin_view_on_idle(slot->views[v].view);
        }
    }
}

/* --------------------------------------------------------------------------
 * Unload
 * -------------------------------------------------------------------------- */
PiResult pi_plugin_host_session_unload(PiPluginHostSession* session, uint32_t slot)
{
    PiPluginHostSessionSlot* s;

    if (!session)
    {
        return PI_E_INVALIDARG;
    }
    s = SessionSlotAt(session, slot);
    if (!s)
    {
        SessionSetError(session, "slot %u is empty", (unsigned)slot);
        return PI_E_NOTFOUND;
    }

    SessionLog(session, "unload[%u]: begin (views=%u)", (unsigned)slot, (unsigned)s->view_count);
    SessionTearDownSlot(session, s, slot, "unload");
    SessionLog(session, "unload[%u]: done", (unsigned)slot);
    return PI_OK;
}

void pi_plugin_host_session_unload_all(PiPluginHostSession* session)
{
    uint32_t i;
    if (!session)
    {
        return;
    }
    for (i = 0; i < PI_PLUGIN_HOST_SESSION_MAX_SLOTS; ++i)
    {
        if (session->slots[i].in_use || session->slots[i].module || session->slots[i].plugin ||
            session->slots[i].view_count > 0)
        {
            SessionLog(session, "unload[%u]: begin", (unsigned)i);
            SessionTearDownSlot(session, &session->slots[i], i, "unload");
            SessionLog(session, "unload[%u]: done", (unsigned)i);
        }
    }
}

/* --------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------- */
void pi_plugin_host_session_set_logger(PiPluginHostSession*       session,
                                       PiPluginHostSessionLogProc log, void* user_data)
{
    if (!session)
    {
        return;
    }
    session->log      = log;
    session->log_user = user_data;
}

void pi_plugin_host_session_set_skip_detach(PiPluginHostSession* session, int skip)
{
    if (!session)
    {
        return;
    }
    session->skip_detach = skip ? 1 : 0;
}

char const* pi_plugin_host_session_last_error(PiPluginHostSession const* session)
{
    return session ? session->last_error : "";
}
