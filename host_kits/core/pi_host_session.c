/*
 * piplugin - Host kit L0: PiPluginHostSession (implementation)
 *
 * 设计纪律见 pi_host_session.h 与 host_kits/README.md。
 * 本文件只做机制：加载、门禁、实例化、多槽位、七步卸载；不碰任何 UI 决策。
 */
#include "pi_host_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal types
 * -------------------------------------------------------------------------- */
typedef struct PiHostSessionSlot {
    PiPluginModule*           module;       /* loaded module (owns the code) */
    IPiPluginFactory*         factory;      /* add-ref'd at load              */
    IPiPluginBase*            plugin;       /* add-ref'd at instantiate       */
    IPiPluginView*            view;         /* add-ref'd; may be NULL         */
    IPiService*               service;      /* add-ref'd; may be NULL         */
    IPiEventSink*             event_sink;   /* add-ref'd; may be NULL (APP-06) */
    const PiPluginDescriptor* descriptor;   /* borrowed from the factory      */
    PiNativeWindow            attach_window;
    int                       view_attached;
    int                       in_use;
} PiHostSessionSlot;

struct PiPluginHostSession {
    IPiHostServices*   services;            /* add-ref'd */
    IPiHostEvents*     host_events;         /* add-ref'd; may be NULL (APP-06) */
    PiHostSessionSlot  slots[PI_HOST_SESSION_MAX_SLOTS];
    PiGuid             required[PI_HOST_SESSION_MAX_SLOTS];
    uint32_t           required_count;
    int                skip_detach;
    PiHostSessionLogProc log;
    void*              log_user;
    char               last_error[PI_HOST_SESSION_ERROR_MAX];
};

/* --------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------- */
static void SessionSetError(PiPluginHostSession* session, const char* fmt, ...)
{
    va_list ap;
    if (!session) return;
    va_start(ap, fmt);
    vsnprintf(session->last_error, sizeof(session->last_error), fmt, ap);
    va_end(ap);
}

static void SessionLog(PiPluginHostSession* session, const char* fmt, ...)
{
    char buf[PI_HOST_SESSION_ERROR_MAX + 192];
    va_list ap;
    if (!session || !session->log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    session->log(session->log_user, buf);
}

/* --------------------------------------------------------------------------
 * Slot bookkeeping
 * -------------------------------------------------------------------------- */
static int SessionFindFreeSlot(const PiPluginHostSession* session)
{
    uint32_t i;
    for (i = 0; i < PI_HOST_SESSION_MAX_SLOTS; ++i) {
        if (!session->slots[i].in_use) return (int)i;
    }
    return -1;
}

static PiHostSessionSlot* SessionSlotAt(PiPluginHostSession* session, uint32_t slot)
{
    if (!session || slot >= PI_HOST_SESSION_MAX_SLOTS) return NULL;
    if (!session->slots[slot].in_use) return NULL;
    return &session->slots[slot];
}

/* 七步卸载序列（也用于加载失败时的就地回卷）。
 * 释放顺序不可调换：插件/视图的代码在模块里，任何一步先卸模块都是 UB。 */
static void SessionTearDownSlot(PiPluginHostSession* session, PiHostSessionSlot* slot,
                                uint32_t index, const char* tag)
{
    if (!slot) return;

    /* 1) 服务：可能还在别的线程上跑 I/O，先停再释放 */
    if (slot->service) {
        SessionLog(session, "%s[%u]: service stop", tag, index);
        pi_service_stop(slot->service);
        pi_iunknown_release((IPiUnknown*)slot->service);
        slot->service = NULL;
    }

    /* 2) 事件（APP-06）：先让宿主按 owner 退订，再放掉 sink。
     *
     * 顺序是这一层存在的意义：owner 就是插件实例指针，退订必须在插件代码还映射着
     * 的时候做完；sink 的引用也必须在模块卸载前放掉。两件事都由 kit 做，宿主
     * 手写就会漏 —— 漏了就是"插件卸载后回调进已卸载内存"。 */
    if (session->host_events && slot->plugin) {
        SessionLog(session, "%s[%u]: drop event subscriptions", tag, index);
        pi_host_events_drop_owner(session->host_events, (void*)slot->plugin);
    }
    if (slot->event_sink) {
        SessionLog(session, "%s[%u]: release event sink", tag, index);
        pi_iunknown_release((IPiUnknown*)slot->event_sink);
        slot->event_sink = NULL;
    }

    /* 3) 视图：detach 必须先于 release，且两者都必须先于模块卸载 */
    if (slot->view) {
        if (slot->view_attached && !session->skip_detach) {
            SessionLog(session, "%s[%u]: detach view", tag, index);
            pi_view_detach(slot->view);
        } else if (slot->view_attached) {
            SessionLog(session, "%s[%u]: SKIPPING detach (diagnostic)", tag, index);
        }
        SessionLog(session, "%s[%u]: release view", tag, index);
        pi_iunknown_release((IPiUnknown*)slot->view);
        slot->view = NULL;
    }
    slot->view_attached = 0;
    slot->attach_window = PI_INVALID_WINDOW;

    /* 4) 插件实例：terminate 再 release —— 此刻插件代码必须仍然 mapped */
    if (slot->plugin) {
        SessionLog(session, "%s[%u]: terminate plugin", tag, index);
        pi_plugin_terminate(slot->plugin);
        pi_iunknown_release((IPiUnknown*)slot->plugin);
        slot->plugin = NULL;
    }

    /* 5) 工厂 */
    if (slot->factory) {
        pi_iunknown_release((IPiUnknown*)slot->factory);
        slot->factory = NULL;
    }

    /* 6) 模块 */
    if (slot->module) {
        SessionLog(session, "%s[%u]: unload module", tag, index);
        pi_module_unload(slot->module);
        slot->module = NULL;
    }

    /* 7) descriptor 属于模块，随模块失效 */
    slot->descriptor = NULL;
    slot->in_use = 0;
}

/* --------------------------------------------------------------------------
 * Capability gates (both directions)
 * -------------------------------------------------------------------------- */

/* 方向一：插件 REQUIRE 的能力，宿主必须有 —— 用 QI 探测宿主服务对象。
 * 通用化了测试宿主原先只探 PI_IID_HOST_UI 的一次性检查：任何 REQUIRED 能力
 * 宿主给不出，就拒绝实例化（headless 宿主拒绝 GUI 插件正是这条）。 */
static PiResult SessionGatePluginRequirements(PiPluginHostSession* session,
                                              const PiPluginDescriptor* desc)
{
    uint32_t i;
    if (!desc || !desc->capabilities || desc->capability_count == 0) return PI_OK;

    for (i = 0; i < desc->capability_count; ++i) {
        const PiPluginCapability* cap = &desc->capabilities[i];
        void* probe = NULL;
        PiResult hr;

        if (!(cap->flags & PI_CAP_REQUIRED)) continue;

        hr = pi_iunknown_query_interface((IPiUnknown*)session->services, &cap->iid, &probe);
        if (PI_FAILED(hr) || !probe) {
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
static PiResult SessionGateHostRequirements(PiPluginHostSession* session,
                                            const PiPluginDescriptor* desc)
{
    uint32_t i;
    for (i = 0; i < session->required_count; ++i) {
        if (!desc || !pi_descriptor_provides(desc, &session->required[i])) {
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
PiResult pi_host_session_create(IPiHostServices* services,
                                PiPluginHostSession** out_session)
{
    PiPluginHostSession* session;

    if (!services || !out_session) return PI_E_INVALIDARG;
    *out_session = NULL;

    session = (PiPluginHostSession*)calloc(1, sizeof(PiPluginHostSession));
    if (!session) return PI_E_OUTOFMEMORY;

    session->services = services;
    pi_iunknown_add_ref((IPiUnknown*)services);

    /* 宿主可选提供事件接口（APP-06）：有就持有（借给宿主用），没有就一直是 NULL。
     * 宿主通常用 pi_host_services_create_ex() 的 extra_qi 钩子把路由器挂上（APP-01）——
     * 这里走的就是插件将来会走的那条 QI 路径。（此处还没有 logger，故不写日志。） */
    if (PI_FAILED(pi_host_events_query(services, &session->host_events))) {
        session->host_events = NULL;
    }

    *out_session = session;
    return PI_OK;
}

void pi_host_session_destroy(PiPluginHostSession* session)
{
    uint32_t i;
    if (!session) return;

    for (i = 0; i < PI_HOST_SESSION_MAX_SLOTS; ++i) {
        PiHostSessionSlot* slot = &session->slots[i];
        if (slot->in_use || slot->module || slot->plugin || slot->view ||
            slot->event_sink) {
            SessionTearDownSlot(session, slot, i, "destroy");
        }
    }

    if (session->host_events) {
        pi_iunknown_release((IPiUnknown*)session->host_events);
        session->host_events = NULL;
    }
    if (session->services) {
        pi_iunknown_release((IPiUnknown*)session->services);
        session->services = NULL;
    }
    free(session);
}

PiResult pi_host_session_require(PiPluginHostSession* session, const PiGuid* iid)
{
    uint32_t i;
    if (!session || !iid) return PI_E_INVALIDARG;

    for (i = 0; i < session->required_count; ++i) {
        if (pi_guid_equal(&session->required[i], iid)) return PI_OK;   /* idempotent */
    }
    if (session->required_count >= PI_HOST_SESSION_MAX_SLOTS) {
        SessionSetError(session, "too many host requirements (max %u)",
                        (unsigned)PI_HOST_SESSION_MAX_SLOTS);
        return PI_E_OUTOFMEMORY;
    }
    session->required[session->required_count++] = *iid;
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Load / inspect / instantiate
 * -------------------------------------------------------------------------- */
PiResult pi_host_session_inspect(PiPluginHostSession* session,
                                 const char* dll_path,
                                 uint32_t* out_slot)
{
    int index;
    PiHostSessionSlot* slot;
    IPiPluginFactory* factory = NULL;
    const PiPluginDescriptor* desc = NULL;
    PiResult hr;

    if (!session || !dll_path || !out_slot) return PI_E_INVALIDARG;
    *out_slot = PI_HOST_SESSION_INVALID_SLOT;

    index = SessionFindFreeSlot(session);
    if (index < 0) {
        SessionSetError(session, "session slot table is full (%u slots)",
                        (unsigned)PI_HOST_SESSION_MAX_SLOTS);
        return PI_E_UNEXPECTED;
    }
    slot = &session->slots[index];
    memset(slot, 0, sizeof(*slot));
    slot->attach_window = PI_INVALID_WINDOW;
    slot->in_use = 1;   /* 先占住槽位：任何失败都走同一条回卷路径 */

    SessionLog(session, "load[%d]: %s", index, dll_path);

    slot->module = pi_module_load(dll_path);
    if (!slot->module) {
        SessionSetError(session, "pi_module_load failed: %s", pi_module_get_load_error());
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return PI_E_NOTFOUND;
    }

    hr = pi_module_get_factory(slot->module, &factory);
    if (PI_FAILED(hr) || !factory) {
        SessionSetError(session, "pi_module_get_factory failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return PI_FAILED(hr) ? hr : PI_E_UNEXPECTED;
    }
    slot->factory = factory;

    pi_factory_get_descriptor(slot->factory, &desc);
    slot->descriptor = desc;

    /* 版本门禁（roadmap BLK-03）：插件声明的 api_version 必须与宿主兼容。
     * 与能力门禁一样在**实例化之前**判定，所以不兼容的插件连实例都不会被创建。 */
    if (desc && !pi_api_version_compatible(PIPLUGIN_API_VERSION, desc->api_version)) {
        SessionSetError(session,
                        "plugin api_version 0x%08X (major %u, minor %u) is incompatible with host 0x%08X (major %u, minor %u); the plugin must not be newer than the host",
                        (unsigned)desc->api_version,
                        (unsigned)PIPLUGIN_API_VERSION_MAJOR(desc->api_version),
                        (unsigned)PIPLUGIN_API_VERSION_MINOR(desc->api_version),
                        (unsigned)PIPLUGIN_API_VERSION,
                        (unsigned)PIPLUGIN_API_VERSION_MAJOR(PIPLUGIN_API_VERSION),
                        (unsigned)PIPLUGIN_API_VERSION_MINOR(PIPLUGIN_API_VERSION));
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return PI_E_VERSIONMISMATCH;
    }

    /* 双向门禁：都要在实例化之前 */
    hr = SessionGatePluginRequirements(session, desc);
    if (PI_FAILED(hr)) {
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return hr;
    }
    hr = SessionGateHostRequirements(session, desc);
    if (PI_FAILED(hr)) {
        SessionTearDownSlot(session, slot, (uint32_t)index, "rollback");
        return hr;
    }

    SessionLog(session, "load[%d]: gate passed (category=%s, capabilities=%u)",
               index, (desc && desc->category) ? desc->category : "-",
               (unsigned)(desc ? desc->capability_count : 0u));

    *out_slot = (uint32_t)index;
    return PI_OK;
}

PiResult pi_host_session_instantiate(PiPluginHostSession* session, uint32_t slot_index)
{
    PiHostSessionSlot* slot;
    PiGuid class_guid;
    uint32_t class_count;
    PiResult hr;

    if (!session) return PI_E_INVALIDARG;
    slot = SessionSlotAt(session, slot_index);
    if (!slot) return PI_E_INVALIDARG;

    if (slot->plugin) {
        SessionSetError(session, "slot %u is already instantiated", (unsigned)slot_index);
        return PI_E_UNEXPECTED;
    }
    if (!slot->factory) {
        SessionSetError(session, "slot %u has no factory (inspect first)", (unsigned)slot_index);
        return PI_E_UNEXPECTED;
    }

    class_count = pi_factory_get_class_count(slot->factory);
    if (class_count == 0) {
        SessionSetError(session, "plugin exposes no classes");
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return PI_E_NOTFOUND;
    }

    hr = pi_factory_get_class_guid(slot->factory, 0, &class_guid);
    if (PI_FAILED(hr)) {
        SessionSetError(session, "pi_factory_get_class_guid(0) failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return hr;
    }

    hr = pi_factory_create_instance(slot->factory, &class_guid, session->services, &slot->plugin);
    if (PI_FAILED(hr) || !slot->plugin) {
        SessionSetError(session, "pi_factory_create_instance failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return PI_FAILED(hr) ? hr : PI_E_UNEXPECTED;
    }

    hr = pi_plugin_initialize(slot->plugin, session->services);
    if (PI_FAILED(hr)) {
        SessionSetError(session, "pi_plugin_initialize failed (hr=%d)", (int)hr);
        SessionTearDownSlot(session, slot, slot_index, "rollback");
        return hr;
    }

    /* 视图与可选服务。取不到都不是错误：headless 插件本来就不该有 view，
     * 而不实现 IPiService 的插件 QI 失败正是能力协商的常态。 */
    {
        IPiPluginView* view = NULL;
        if (PI_SUCCEEDED(pi_plugin_get_view(slot->plugin, &view)) && view) {
            slot->view = view;
        }
    }
    {
        IPiService* service = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)slot->plugin,
                                                     &PI_IID_SERVICE, (void**)&service))) {
            slot->service = service;
        }
    }
    {
        /* 事件 sink（APP-06）：插件可选实现，没有就是没有 —— 与 view/service 一样
         * 是能力协商的常态，不是错误。卸载序列会按正确的时机释放它。 */
        IPiEventSink* sink = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)slot->plugin,
                                                     &PI_IID_EVENT_SINK, (void**)&sink))) {
            slot->event_sink = sink;
        }
    }

    SessionLog(session, "load[%u]: ready (view:%s service:%s events:%s)",
               (unsigned)slot_index,
               slot->view ? "Y" : "N", slot->service ? "Y" : "N",
               slot->event_sink ? "Y" : "N");
    return PI_OK;
}

PiResult pi_host_session_load(PiPluginHostSession* session,
                              const char* dll_path,
                              uint32_t* out_slot)
{
    uint32_t slot = PI_HOST_SESSION_INVALID_SLOT;
    PiResult hr;

    if (!session || !dll_path) return PI_E_INVALIDARG;
    if (out_slot) *out_slot = PI_HOST_SESSION_INVALID_SLOT;

    hr = pi_host_session_inspect(session, dll_path, &slot);
    if (PI_FAILED(hr)) return hr;

    hr = pi_host_session_instantiate(session, slot);
    if (PI_FAILED(hr)) {
        /* instantiate 内部已回卷，这里只需把槽位下标还回去 */
        if (out_slot) *out_slot = PI_HOST_SESSION_INVALID_SLOT;
        return hr;
    }

    if (out_slot) *out_slot = slot;
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Slot queries
 * -------------------------------------------------------------------------- */
uint32_t pi_host_session_count(const PiPluginHostSession* session)
{
    uint32_t i, n = 0;
    if (!session) return 0;
    for (i = 0; i < PI_HOST_SESSION_MAX_SLOTS; ++i) {
        if (session->slots[i].in_use) ++n;
    }
    return n;
}

int pi_host_session_is_loaded(const PiPluginHostSession* session, uint32_t slot)
{
    if (!session || slot >= PI_HOST_SESSION_MAX_SLOTS) return 0;
    return session->slots[slot].in_use ? 1 : 0;
}

IPiPluginBase* pi_host_session_get_plugin(PiPluginHostSession* session, uint32_t slot)
{
    PiHostSessionSlot* s = SessionSlotAt(session, slot);
    return s ? s->plugin : NULL;
}

IPiPluginView* pi_host_session_get_view(PiPluginHostSession* session, uint32_t slot)
{
    PiHostSessionSlot* s = SessionSlotAt(session, slot);
    return s ? s->view : NULL;
}

IPiService* pi_host_session_get_service(PiPluginHostSession* session, uint32_t slot)
{
    PiHostSessionSlot* s = SessionSlotAt(session, slot);
    return s ? s->service : NULL;
}

const PiPluginDescriptor* pi_host_session_get_descriptor(const PiPluginHostSession* session,
                                                         uint32_t slot)
{
    if (!session || slot >= PI_HOST_SESSION_MAX_SLOTS) return NULL;
    if (!session->slots[slot].in_use) return NULL;
    return session->slots[slot].descriptor;
}

/* --------------------------------------------------------------------------
 * Events (APP-06, channel C)
 * -------------------------------------------------------------------------- */
int pi_host_session_has_event_sink(const PiPluginHostSession* session, uint32_t slot)
{
    if (!session || slot >= PI_HOST_SESSION_MAX_SLOTS) return 0;
    return session->slots[slot].event_sink ? 1 : 0;
}

PiResult pi_host_session_deliver_event(PiPluginHostSession* session, uint32_t slot,
                                       const PiEvent* event)
{
    PiHostSessionSlot* s;

    if (!session || !event) return PI_E_INVALIDARG;
    s = SessionSlotAt(session, slot);
    if (!s || !s->plugin) return PI_E_NOINTERFACE;
    if (!s->event_sink) {
        /* 插件没实现 sink：这是"优雅降级"的那条路径，调用方跳过即可，不是错误。
         * 记一行日志便于诊断（有些宿主会想知道自己的事件没人收）。 */
        SessionLog(session, "events[%u]: plugin has no sink, event '%s' skipped",
                   (unsigned)slot, event->topic ? event->topic : "(null)");
        return PI_E_NOINTERFACE;
    }
    return pi_event_deliver(s->event_sink, event);
}

IPiHostEvents* pi_host_session_get_host_events(PiPluginHostSession* session)
{
    return session ? session->host_events : NULL;
}

/* --------------------------------------------------------------------------
 * Embedding / per-frame pump
 * -------------------------------------------------------------------------- */
PiResult pi_host_session_attach_view(PiPluginHostSession* session, uint32_t slot_index,
                                     PiNativeWindow parent_window, int set_visible)
{
    PiHostSessionSlot* slot;
    PiResult hr;

    if (!session) return PI_E_INVALIDARG;
    slot = SessionSlotAt(session, slot_index);
    if (!slot) return PI_E_INVALIDARG;
    if (!PI_IS_VALID_WINDOW(parent_window)) return PI_E_INVALIDARG;
    if (!slot->view) {
        SessionSetError(session, "slot %u has no view to attach (headless plugin?)",
                        (unsigned)slot_index);
        return PI_E_NOINTERFACE;
    }
    if (slot->view_attached) {
        SessionSetError(session, "slot %u already has an attached view", (unsigned)slot_index);
        return PI_E_UNEXPECTED;
    }

    hr = pi_view_attach(slot->view, parent_window);
    if (PI_FAILED(hr)) {
        SessionSetError(session, "pi_view_attach failed (hr=%d)", (int)hr);
        return hr;
    }
    slot->view_attached = 1;
    slot->attach_window = parent_window;

    if (set_visible) pi_view_set_visible(slot->view, 1);

    SessionLog(session, "attach[%u]: plugin window=0x%llx container=0x%llx",
               (unsigned)slot_index,
               (unsigned long long)(uintptr_t)pi_view_get_native_window(slot->view),
               (unsigned long long)(uintptr_t)parent_window);
    return PI_OK;
}

void pi_host_session_drive_idle(PiPluginHostSession* session)
{
    uint32_t i;
    if (!session) return;
    for (i = 0; i < PI_HOST_SESSION_MAX_SLOTS; ++i) {
        PiHostSessionSlot* slot = &session->slots[i];
        if (slot->in_use && slot->view) {
            pi_view_on_idle(slot->view);
        }
    }
}

/* --------------------------------------------------------------------------
 * Unload
 * -------------------------------------------------------------------------- */
PiResult pi_host_session_unload(PiPluginHostSession* session, uint32_t slot)
{
    PiHostSessionSlot* s;

    if (!session) return PI_E_INVALIDARG;
    s = SessionSlotAt(session, slot);
    if (!s) {
        SessionSetError(session, "slot %u is empty", (unsigned)slot);
        return PI_E_NOTFOUND;
    }

    SessionLog(session, "unload[%u]: begin", (unsigned)slot);
    SessionTearDownSlot(session, s, slot, "unload");
    SessionLog(session, "unload[%u]: done", (unsigned)slot);
    return PI_OK;
}

void pi_host_session_unload_all(PiPluginHostSession* session)
{
    uint32_t i;
    if (!session) return;
    for (i = 0; i < PI_HOST_SESSION_MAX_SLOTS; ++i) {
        if (session->slots[i].in_use || session->slots[i].module || session->slots[i].plugin) {
            SessionLog(session, "unload[%u]: begin", (unsigned)i);
            SessionTearDownSlot(session, &session->slots[i], i, "unload");
            SessionLog(session, "unload[%u]: done", (unsigned)i);
        }
    }
}

/* --------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------- */
void pi_host_session_set_logger(PiPluginHostSession* session,
                                PiHostSessionLogProc log, void* user_data)
{
    if (!session) return;
    session->log = log;
    session->log_user = user_data;
}

void pi_host_session_set_skip_detach(PiPluginHostSession* session, int skip)
{
    if (!session) return;
    session->skip_detach = skip ? 1 : 0;
}

const char* pi_host_session_last_error(const PiPluginHostSession* session)
{
    return session ? session->last_error : "";
}
