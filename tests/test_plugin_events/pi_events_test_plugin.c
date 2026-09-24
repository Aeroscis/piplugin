/*
 * piplugin - Events test plugin (roadmap APP-06, channel C)
 *
 * A pure-C plugin that exercises both halves of the event mechanism:
 *
 *   - it PROVIDES PI_PLUGIN_IID_EVENT_SINK, so a host can push ADDRESSED events into it
 *     (the host queries the sink and calls pi_plugin_event_deliver);
 *   - it uses the host's PI_PLUGIN_IID_HOST_EVENTS (declared OPTIONAL), so it can
 *     PUBLISH events and SUBSCRIBE to topics like any other participant.
 *
 * It also demonstrates the two interface objects trick again (the instance
 * carries IPiPluginBase, QueryInterface(PI_PLUGIN_IID_EVENT_SINK) hands out a small
 * wrapper with the sink vtable), the same containment pattern the service test
 * plugin and the framework's own IPiPluginHostUI use.
 *
 * The scripted conversation with tests/test_host_events (topics/keys live in
 * tests/common/pi_test_events_protocol.h):
 *
 *   1. during initialize it publishes  com.example.plugin.ready
 *   2. it subscribes to               com.example.host.broadcast  (owner = this)
 *   3. the host delivers              com.example.host.welcome    to its sink
 *   4. from inside the sink it publishes com.example.plugin.ack
 *   5. every broadcast it receives makes it publish
 *                                     com.example.plugin.saw_broadcast
 *
 * Deliberately NOT unsubscribing in terminate: the host kit calls
 * pi_plugin_host_events_drop_owner() for every slot it unloads, and this plugin is the
 * proof that a plugin which forgets cannot leave a dangling callback behind
 * (that is decision D9 of docs/design/events.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pi_test_events_protocol.h"
#include "piplugin/pi_plugin.h"

/* 完整随机的 128 位 UUID 风格 class GUID（不是框架保留区里的小整数编号）。 */
static PiGuid const EVENTS_CLASS_GUID =
    PI_GUID(0x4E7B2C58, 0x9D31, 0x4A6F, 0xB2, 0x84, 0x17, 0x5E, 0xC9, 0x30, 0xA6, 0x7D);

typedef struct EventsPlugin {
    PiRefCountedBase       base;        /* MUST be first: the IPiPluginBase object */
    IPiPluginHostServices* host;        /* add-ref'd; borrowed by Initialize */
    IPiPluginHostEvents*   host_events; /* add-ref'd; NULL when the host has none  */

    /* what it did, for its own traces */
    uint32_t ready_published;
    uint32_t sink_calls;
    uint32_t welcome_calls;
    uint32_t ack_published;
    uint32_t broadcast_seen;
    uint32_t subscription; /* handle, or PI_PLUGIN_EVENT_INVALID_SUBSCRIPTION */
    char     last_topic[PI_PLUGIN_EVENT_TOPIC_MAX];
} EventsPlugin;

typedef struct EventsSink {
    PiRefCountedBase base;  /* MUST be first */
    EventsPlugin*    owner; /* add-ref'd */
} EventsSink;

static uint32_t PI_CALL Plugin_AddRef(void* self_ptr);
static uint32_t PI_CALL Plugin_Release(void* self_ptr);
static PiResult PI_CALL Plugin_Qi(void* self_ptr, PiGuid const* iid, void** out);
static PiResult PI_CALL Plugin_Initialize(void* self_ptr, IPiPluginHostServices* host);
static PiResult PI_CALL Plugin_Terminate(void* self_ptr);
static PiResult PI_CALL Plugin_GetView(void* self_ptr, IPiPluginView** out);

static uint32_t PI_CALL Sink_AddRef(void* self_ptr);
static uint32_t PI_CALL Sink_Release(void* self_ptr);
static PiResult PI_CALL Sink_Qi(void* self_ptr, PiGuid const* iid, void** out);
static PiResult PI_CALL Sink_Deliver(void* self_ptr, PiPluginEvent const* event);

/* 本模块内的薄封装：直接取 dllimport 函数地址在 C 里会触发 C4232（见
 * docs/design/interfaces.md 5.3），与其它纯 C 实现同一消法。 */
static uint32_t PI_CALL Plugin_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL Plugin_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}
static uint32_t PI_CALL Sink_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
static uint32_t PI_CALL Sink_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static IPiPluginBaseVtbl const s_plugin_vtbl = {
    {&Plugin_Qi, &Plugin_AddRef, &Plugin_Release},
    &Plugin_Initialize,
    &Plugin_Terminate,
    &Plugin_GetView
};

static IPiPluginEventSinkVtbl const s_sink_vtbl = {
    {&Sink_Qi, &Sink_AddRef, &Sink_Release},
    &Sink_Deliver
};

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* The owner token the host and the plugin must agree on: the plugin INSTANCE
 * address (what the host sees as IPiPluginBase*). PiRefCountedBase is the first
 * member, so &plugin->base is that address. */
static void* PluginOwnerToken(EventsPlugin* plugin)
{
    return (void*)&plugin->base;
}

static void PluginPublish(EventsPlugin* plugin, uint32_t type, char const* topic,
                          PiPluginProperty const* payload, uint32_t payload_count)
{
    PiPluginEvent event;
    if (!plugin || !plugin->host_events)
    {
        return; /* no events host: carry on */
    }

    memset(&event, 0, sizeof(event));
    event.type          = type;
    event.topic         = topic;
    event.payload       = payload;
    event.payload_count = payload_count;
    event.origin        = NULL; /* the host fills this in */

    (void)pi_plugin_host_events_publish(plugin->host_events, &event);
}

/* A one-property payload, borrowed for the publish call only (the router copies). */
static void PluginPublishOne(EventsPlugin* plugin, char const* topic,
                             char const* key, char const* value)
{
    PiPluginProperty prop;
    prop.key   = key;
    prop.value = value;
    PluginPublish(plugin, PI_PLUGIN_EVENT_NOTIFY, topic, &prop, 1);
}

/* Subscription callback (runs on the host's main thread): echo what we saw. */
static void OnBroadcast(void* user_data, PiPluginEvent const* event)
{
    EventsPlugin* plugin = (EventsPlugin*)user_data;
    char          count[16];

    (void)event; /* the echo only counts what it saw */
    if (!plugin)
    {
        return;
    }
    ++plugin->broadcast_seen;

    snprintf(count, sizeof(count), "%u", (unsigned)plugin->broadcast_seen);
    PluginPublishOne(plugin, PI_PLUGIN_TEST_EVENTS_TOPIC_SAW_BROADCAST,
                     PI_PLUGIN_TEST_EVENTS_KEY_COUNT, count);
}

/* --------------------------------------------------------------------------
 * IPiPluginEventSink (self_ptr is the EventsSink wrapper)
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Sink_Deliver(void* self_ptr, PiPluginEvent const* event)
{
    EventsSink*   wrapper = (EventsSink*)self_ptr;
    EventsPlugin* plugin  = wrapper ? wrapper->owner : NULL;
    char const*   greeting;

    if (!plugin || !event)
    {
        return PI_E_INVALIDARG;
    }

    ++plugin->sink_calls;
    if (event->topic)
    {
        size_t n = strlen(event->topic);
        if (n >= sizeof(plugin->last_topic))
        {
            n = sizeof(plugin->last_topic) - 1;
        }
        memcpy(plugin->last_topic, event->topic, n);
        plugin->last_topic[n] = '\0';
    }

    if (event->topic && strcmp(event->topic, PI_PLUGIN_TEST_EVENTS_TOPIC_WELCOME) == 0)
    {
        ++plugin->welcome_calls;
        greeting = pi_plugin_test_event_payload(event, PI_PLUGIN_TEST_EVENTS_KEY_GREETING);

        /* Reply from inside the sink: publishing here is allowed, and the host
         * sees it on its next pump (never re-entrantly during this call). */
        PluginPublishOne(plugin, PI_PLUGIN_TEST_EVENTS_TOPIC_ACK,
                         PI_PLUGIN_TEST_EVENTS_KEY_ACKED, "1");
        ++plugin->ack_published;

        printf("[events plugin] welcomed (greeting='%s') -> published '%s'\n",
               greeting ? greeting : "(none)", PI_PLUGIN_TEST_EVENTS_TOPIC_ACK);
        return PI_OK;
    }

    /* Not interested in anything else: PI_E_NOTIMPL is the documented answer. */
    return PI_E_NOTIMPL;
}

static PiResult PI_CALL Sink_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_PLUGIN_IID_EVENT_SINK))
    {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static void Sink_Destroy(void* self_ptr)
{
    EventsSink* wrapper = (EventsSink*)self_ptr;
    if (wrapper->owner)
    {
        pi_iunknown_release((IPiUnknown*)&wrapper->owner->base);
        wrapper->owner = NULL;
    }
    free(wrapper);
}

/* --------------------------------------------------------------------------
 * IPiPluginBase (self_ptr is the EventsPlugin instance)
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Plugin_Initialize(void* self_ptr, IPiPluginHostServices* host)
{
    EventsPlugin* plugin = (EventsPlugin*)self_ptr;
    uint32_t      handle = PI_PLUGIN_EVENT_INVALID_SUBSCRIPTION;

    if (plugin->host)
    {
        return PI_OK; /* idempotent (see docs/tutorial/write-plugin.md) */
    }
    if (!host)
    {
        return PI_OK;
    }

    plugin->host = host; /* borrowed parameter: take our own reference */
    pi_iunknown_add_ref((IPiUnknown*)host);

    /* Events are OPTIONAL for a plugin: a host without them is a normal host. */
    if (PI_FAILED(pi_plugin_host_events_query(host, &plugin->host_events)))
    {
        plugin->host_events = NULL;
        printf("[events plugin] host provides no events (IPiPluginHostEvents absent)\n");
        return PI_OK;
    }

    /* Subscribe as a participant. owner = this instance, so the host can drop
     * the subscription on unload without asking us (D9). */
    if (PI_SUCCEEDED(pi_plugin_host_events_subscribe(plugin->host_events,
                                                     PI_PLUGIN_TEST_EVENTS_TOPIC_BROADCAST,
                                                     PluginOwnerToken(plugin),
                                                     &OnBroadcast, plugin, &handle)))
    {
        plugin->subscription = handle;
    }

    /* Announce ourselves: this is the plugin -> host direction. */
    {
        PiPluginProperty prop;
        prop.key   = PI_PLUGIN_TEST_EVENTS_KEY_PLUGIN;
        prop.value = "events-test";
        PluginPublish(plugin, PI_PLUGIN_EVENT_NOTIFY, PI_PLUGIN_TEST_EVENTS_TOPIC_READY, &prop, 1);
        ++plugin->ready_published;
    }
    return PI_OK;
}

static PiResult PI_CALL Plugin_Terminate(void* self_ptr)
{
    EventsPlugin* plugin = (EventsPlugin*)self_ptr;

    /* Note what is NOT here: unsubscribing. The host kit drops every
     * subscription owned by this instance while the module is still mapped, so a
     * plugin that forgets (this one, on purpose) cannot leave a dangling
     * callback behind. Explicitly unsubscribing earlier is allowed and
     * idempotent - see the host test, which does it for one of its own
     * subscriptions. */
    if (plugin)
    {
        printf("[events plugin] terminate (sink calls=%u, welcome=%u, "
               "broadcasts=%u, ack published=%u)\n",
               (unsigned)plugin->sink_calls, (unsigned)plugin->welcome_calls,
               (unsigned)plugin->broadcast_seen, (unsigned)plugin->ack_published);
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
    return PI_E_NOINTERFACE; /* headless plugin: no UI */
}

static PiResult PI_CALL Plugin_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    EventsPlugin* plugin = (EventsPlugin*)self_ptr;

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

    if (pi_guid_equal(iid, &PI_PLUGIN_IID_EVENT_SINK))
    {
        /* Separate wrapper object: the two vtables cannot both live at offset 0
         * (same pattern as the service test plugin and IPiPluginHostUI). */
        EventsSink* sink = (EventsSink*)calloc(1, sizeof(EventsSink));
        if (!sink)
        {
            return PI_E_OUTOFMEMORY;
        }
        pi_refcounted_init_with_destroy(&sink->base, (IPiUnknownVtbl const*)&s_sink_vtbl,
                                        &Sink_Destroy);
        sink->owner = plugin;
        pi_refcounted_add_ref((IPiUnknown*)&plugin->base);
        *out = &sink->base;
        return PI_OK;
    }

    *out = NULL;
    return PI_E_NOINTERFACE;
}

static void Plugin_Destroy(void* self_ptr)
{
    EventsPlugin* plugin = (EventsPlugin*)self_ptr;
    if (plugin->host_events)
    {
        pi_iunknown_release((IPiUnknown*)plugin->host_events);
        plugin->host_events = NULL;
    }
    if (plugin->host)
    {
        pi_iunknown_release((IPiUnknown*)plugin->host);
        plugin->host = NULL;
    }
    free(plugin);
}

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */
typedef struct EventsFactory {
    PiRefCountedBase base; /* MUST be first */
} EventsFactory;

static EventsFactory      s_factory;
static PiPluginCapability s_caps[2];
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
    *guid = EVENTS_CLASS_GUID;
    return PI_OK;
}

static PiResult PI_CALL Factory_CreateInstance(void* self_ptr, PiGuid const* guid,
                                               IPiPluginHostServices* host, IPiPluginBase** out)
{
    EventsPlugin* plugin;

    (void)self_ptr;
    if (!guid || !out)
    {
        return PI_E_INVALIDARG;
    }
    *out = NULL;
    if (!pi_guid_equal(guid, &EVENTS_CLASS_GUID))
    {
        return PI_E_NOINTERFACE;
    }

    plugin = (EventsPlugin*)calloc(1, sizeof(EventsPlugin));
    if (!plugin)
    {
        return PI_E_OUTOFMEMORY;
    }

    pi_refcounted_init_with_destroy(&plugin->base, (IPiUnknownVtbl const*)&s_plugin_vtbl,
                                    &Plugin_Destroy);
    plugin->subscription = PI_PLUGIN_EVENT_INVALID_SUBSCRIPTION;

    if (PI_FAILED(Plugin_Initialize(plugin, host)))
    {
        pi_iunknown_release((IPiUnknown*)&plugin->base);
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

        /* PROVIDES the sink (the host may push to us);
         * OPTIONAL host events (we run fine without them, just quieter). */
        s_caps[0].iid   = PI_PLUGIN_IID_EVENT_SINK;
        s_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;
        s_caps[1].iid   = PI_PLUGIN_IID_HOST_EVENTS;
        s_caps[1].flags = PI_PLUGIN_CAP_OPTIONAL;

        s_desc.name             = "Events Test Plugin";
        s_desc.vendor           = "piplugin";
        s_desc.version          = "1.0.0";
        s_desc.category         = "Test/Events";
        s_desc.api_version      = PI_PLUGIN_API_VERSION;
        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 2;

        s_props[0].key        = "com.example.kind";
        s_props[0].value      = "events-plugin";
        s_props[1].key        = "com.example.events.role";
        s_props[1].value      = "sink+publisher";
        s_desc.properties     = s_props;
        s_desc.property_count = 2;

        s_initialized = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
