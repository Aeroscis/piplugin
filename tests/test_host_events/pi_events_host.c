/*
 * piplugin - events test host (roadmap APP-06 acceptance)
 *
 * The two-way loop the roadmap asks for, asserted step by step, plus the two
 * degradation paths (a plugin with no sink, and a plugin that never
 * unsubscribes):
 *
 *   1. the host installs the optional router through
 *      pi_plugin_host_services_create_ex()'s extra-QI hook (channel B, APP-01), and the
 *      session finds it by querying the host object - the same path a plugin uses;
 *   2. the host subscribes as the broker, and checks that unsubscribe really
 *      stops delivery (and that a second unsubscribe is an error);
 *   3. plugin -> host: the plugin publishes "ready" while it is initialised;
 *   4. host -> plugin by ADDRESS: pi_plugin_host_session_deliver_event() pushes "welcome"
 *      into the plugin's IPiPluginEventSink;
 *   5. plugin -> host: the plugin publishes "ack" from inside its sink - and the
 *      pump rule is checked too (an event published from a callback is delivered
 *      by the NEXT pump, never re-entrantly);
 *   6. host -> everyone by TOPIC: a broadcast reaches the plugin because the
 *      plugin subscribed to it itself;
 *   7. D9: the plugin deliberately never unsubscribes, so unloading it proves the
 *      host kit's pi_plugin_host_events_drop_owner() removes the subscription - the host
 *      publishes the same broadcast afterwards and nothing must fire;
 *   8. graceful degradation: a plugin with no sink loads fine, and delivering to
 *      it returns PI_E_NOINTERFACE instead of failing the load.
 *
 * Exit code 0 = every assertion held. Evidence lines are printed for the log.
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"
#include "pi_event_router.h"
#include "pi_test_events_protocol.h"

#include <stdio.h>
#include <string.h>

static PiPluginEventRouter*       g_router   = NULL;
static IPiPluginHostServices*     g_services = NULL;
static PiPluginHostSession* g_session  = NULL;

static int      g_failures = 0;
static unsigned g_checks   = 0;

/* Host-side observations */
static uint32_t g_pumps = 0;
static uint32_t g_ready_count = 0;
static uint32_t g_ack_count = 0;
static uint32_t g_saw_count = 0;
static uint32_t g_nobody_count = 0;
static uint32_t g_from_callback_count = 0;
static uint32_t g_ready_at_pump = 0;
static uint32_t g_from_callback_at_pump = 0;
static int      g_published_from_callback = 0;
static char     g_ready_plugin[64] = "";
static char     g_ack_value[16]    = "";
static char     g_saw_count_text[16] = "";

static void Check(int condition, const char* what)
{
    ++g_checks;
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        printf("  FAIL %s\n", what);
    }
}

static void CheckEqInt(long long actual, long long expected, const char* what)
{
    ++g_checks;
    if (actual == expected) {
        printf("  ok   %s (%lld)\n", what, actual);
    } else {
        ++g_failures;
        printf("  FAIL %s: got %lld, expected %lld\n", what, actual, expected);
    }
}

static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    printf("  [plugin message] msg=0x%04X wparam=%llu\n", msg, (unsigned long long)wparam);
}

static uint32_t Pump(void)
{
    uint32_t delivered;
    ++g_pumps;
    delivered = pi_plugin_event_router_pump(g_router);
    return delivered;
}

static void CopyString(char* dst, size_t dst_size, const char* src)
{
    size_t n;
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* --------------------------------------------------------------------------
 * The host's own subscriptions (the "broker" role)
 * -------------------------------------------------------------------------- */
static void OnReady(void* user_data, const PiPluginEvent* event)
{
    (void)user_data;
    ++g_ready_count;
    g_ready_at_pump = g_pumps;
    CopyString(g_ready_plugin, sizeof(g_ready_plugin),
               pi_plugin_test_event_payload(event, PI_PLUGIN_TEST_EVENTS_KEY_PLUGIN));

    /* Publish from inside a callback: allowed, and the contract says it must not
     * be delivered re-entrantly - the next pump picks it up. */
    if (!g_published_from_callback) {
        PiPluginProperty prop;
        PiPluginEvent          nested;
        g_published_from_callback = 1;
        prop.key = "origin"; prop.value = "callback";
        memset(&nested, 0, sizeof(nested));
        nested.type          = PI_PLUGIN_EVENT_NOTIFY;
        nested.topic         = "com.example.host.from_callback";
        nested.payload       = &prop;
        nested.payload_count = 1;
        (void)pi_plugin_host_events_publish(pi_plugin_event_router_host_events(g_router), &nested);
    }
}

static void OnAck(void* user_data, const PiPluginEvent* event)
{
    (void)user_data;
    ++g_ack_count;
    CopyString(g_ack_value, sizeof(g_ack_value),
               pi_plugin_test_event_payload(event, PI_PLUGIN_TEST_EVENTS_KEY_ACKED));
}

static void OnSawBroadcast(void* user_data, const PiPluginEvent* event)
{
    (void)user_data;
    ++g_saw_count;
    CopyString(g_saw_count_text, sizeof(g_saw_count_text),
               pi_plugin_test_event_payload(event, PI_PLUGIN_TEST_EVENTS_KEY_COUNT));
}

static void OnNobody(void* user_data, const PiPluginEvent* event)
{
    (void)user_data; (void)event;
    ++g_nobody_count;   /* must stay 0 after the subscription was dropped */
}

static void OnFromCallback(void* user_data, const PiPluginEvent* event)
{
    (void)user_data; (void)event;
    ++g_from_callback_count;
    g_from_callback_at_pump = g_pumps;
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static PiPluginEvent MakeWelcomeEvent(PiPluginProperty* prop)
{
    PiPluginEvent event;
    prop->key   = PI_PLUGIN_TEST_EVENTS_KEY_GREETING;
    prop->value = "hello";
    memset(&event, 0, sizeof(event));
    event.type          = PI_PLUGIN_EVENT_REQUEST;
    event.topic         = PI_PLUGIN_TEST_EVENTS_TOPIC_WELCOME;
    event.payload       = prop;
    event.payload_count = 1;
    return event;
}

static void PublishBroadcast(const char* which)
{
    PiPluginProperty prop;
    PiPluginEvent          event;
    prop.key = PI_PLUGIN_TEST_EVENTS_KEY_N; prop.value = which;
    memset(&event, 0, sizeof(event));
    event.type          = PI_PLUGIN_EVENT_NOTIFY;
    event.topic         = PI_PLUGIN_TEST_EVENTS_TOPIC_BROADCAST;
    event.payload       = &prop;
    event.payload_count = 1;
    (void)pi_plugin_host_events_publish(pi_plugin_event_router_host_events(g_router), &event);
}

static void PublishNobody(void)
{
    PiPluginEvent event;
    memset(&event, 0, sizeof(event));
    event.type  = PI_PLUGIN_EVENT_NOTIFY;
    event.topic = PI_PLUGIN_TEST_EVENTS_TOPIC_NOBODY;
    (void)pi_plugin_host_events_publish(pi_plugin_event_router_host_events(g_router), &event);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char** argv)
{
    const char* events_plugin = (argc > 1) ? argv[1] : "pi_test_plugin_events.dll";
    const char* plain_plugin  = (argc > 2) ? argv[2] : "pi_test_plugin_imgui.dll";

    IPiPluginHostEvents*        events = NULL;
    PiPluginEventRouterStats    stats;
    uint32_t              h_ready = 0, h_ack = 0, h_saw = 0, h_nobody = 0, h_callback = 0;
    uint32_t              slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
    uint32_t              plain_slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
    PiPluginProperty      greeting;
    PiPluginEvent               welcome;
    uint64_t              dropped_subs_before = 0;

    printf("== piplugin events test host (APP-06) ==\n");
    printf("events plugin = %s\nplain plugin  = %s\n\n", events_plugin, plain_plugin);

    /* 1) router + host object (channel B) + session */
    printf("- wiring\n");
    CheckEqInt(pi_plugin_event_router_create(&g_router), PI_OK, "router created");
    CheckEqInt(pi_plugin_host_services_create_ex(&HostMessageProc, NULL, PI_INVALID_WINDOW,
                                          &pi_plugin_event_router_extra_qi, g_router, &g_services),
               PI_OK, "host services expose the router through extra_qi");
    CheckEqInt(pi_plugin_host_session_create(g_services, &g_session), PI_OK, "session created");
    Check(pi_plugin_host_session_get_host_events(g_session) == pi_plugin_event_router_host_events(g_router),
          "the session found IPiPluginHostEvents by querying the host object");

    events = pi_plugin_event_router_host_events(g_router);

    /* 2) the host subscribes as the broker (owner NULL = the host itself) */
    printf("\n- host subscriptions\n");
    CheckEqInt(pi_plugin_host_events_subscribe(events, PI_PLUGIN_TEST_EVENTS_TOPIC_READY, NULL,
                                        &OnReady, NULL, &h_ready), PI_OK, "subscribe ready");
    CheckEqInt(pi_plugin_host_events_subscribe(events, PI_PLUGIN_TEST_EVENTS_TOPIC_ACK, NULL,
                                        &OnAck, NULL, &h_ack), PI_OK, "subscribe ack");
    CheckEqInt(pi_plugin_host_events_subscribe(events, PI_PLUGIN_TEST_EVENTS_TOPIC_SAW_BROADCAST, NULL,
                                        &OnSawBroadcast, NULL, &h_saw), PI_OK,
               "subscribe saw_broadcast");
    CheckEqInt(pi_plugin_host_events_subscribe(events, "com.example.host.from_callback", NULL,
                                        &OnFromCallback, NULL, &h_callback), PI_OK,
               "subscribe from_callback");

    /* unsubscribe must really stop delivery (and be strict about bad handles) */
    CheckEqInt(pi_plugin_host_events_subscribe(events, PI_PLUGIN_TEST_EVENTS_TOPIC_NOBODY, NULL,
                                        &OnNobody, NULL, &h_nobody), PI_OK, "subscribe nobody");
    CheckEqInt(pi_plugin_host_events_unsubscribe(events, h_nobody), PI_OK, "unsubscribe nobody");
    CheckEqInt(pi_plugin_host_events_unsubscribe(events, h_nobody), PI_E_INVALIDARG,
               "unsubscribing twice is INVALIDARG");
    CheckEqInt(pi_plugin_host_events_unsubscribe(events, PI_PLUGIN_EVENT_INVALID_SUBSCRIPTION), PI_E_INVALIDARG,
               "unsubscribing handle 0 is INVALIDARG");
    PublishNobody();
    Pump();
    CheckEqInt(g_nobody_count, 0, "the dropped subscription never fires");

    /* 3) load the events plugin */
    printf("\n- load and address the plugin\n");
    CheckEqInt(pi_plugin_host_session_load(g_session, events_plugin, &slot), PI_OK,
               "events plugin loaded");
    CheckEqInt(pi_plugin_host_session_has_event_sink(g_session, slot), 1, "plugin provides a sink");
    if (slot == PI_PLUGIN_HOST_SESSION_INVALID_SLOT) { printf("cannot continue without the plugin\n"); goto verdict; }

    /* 4) plugin -> host (published during initialize) */
    Pump();
    CheckEqInt(g_ready_count, 1, "plugin -> host: 'ready' received");
    Check(strcmp(g_ready_plugin, "events-test") == 0, "ready payload read back");

    /* 5) host -> plugin by ADDRESS (the sink) */
    welcome = MakeWelcomeEvent(&greeting);
    CheckEqInt(pi_plugin_host_session_deliver_event(g_session, slot, &welcome), PI_OK,
               "host -> plugin: event delivered to the sink");

    /* 6) plugin -> host: the reply from inside the sink */
    Pump();
    CheckEqInt(g_ack_count, 1, "plugin -> host: 'ack' published from inside the sink");
    Check(strcmp(g_ack_value, "1") == 0, "ack payload read back");
    CheckEqInt(g_from_callback_count, 1,
               "an event published from inside a callback IS delivered (not lost)");
    CheckEqInt((long long)g_from_callback_at_pump, (long long)g_ready_at_pump + 1,
               "the nested publish was delivered exactly one pump later (no re-entrancy)");

    /* 7) host -> plugin by TOPIC (the plugin's own subscription).
     * The plugin echoes from inside its subscription callback, so - by the same
     * re-entrancy rule as above - its echo lands on the NEXT pump, not this one. */
    printf("\n- topic fan-out\n");
    PublishBroadcast("1");
    Pump();
    CheckEqInt(g_saw_count, 0, "the plugin's echo is not delivered re-entrantly");
    Pump();
    CheckEqInt(g_saw_count, 1, "the plugin's subscription received broadcast 1");
    Check(strcmp(g_saw_count_text, "1") == 0, "the plugin echoed count=1");

    PublishBroadcast("2");
    Pump();
    Pump();
    CheckEqInt(g_saw_count, 2, "the plugin received broadcast 2 as well");
    Check(strcmp(g_saw_count_text, "2") == 0, "the plugin echoed count=2");

    /* 8) D9: unload drops the plugin's subscription (it never unsubscribed itself) */
    printf("\n- unload: owner subscriptions must be dropped\n");
    pi_plugin_event_router_stats(g_router, &stats);
    dropped_subs_before = stats.subscriptions_dropped;
    CheckEqInt(pi_plugin_host_session_unload(g_session, slot), PI_OK, "plugin unloaded");
    CheckEqInt(pi_plugin_host_session_has_event_sink(g_session, slot), 0, "sink is gone after unload");
    CheckEqInt(pi_plugin_host_session_deliver_event(g_session, slot, &welcome), PI_E_NOINTERFACE,
               "delivering to an unloaded slot degrades to NOINTERFACE");
    pi_plugin_event_router_stats(g_router, &stats);
    CheckEqInt((long long)(stats.subscriptions_dropped - dropped_subs_before), 1,
               "the kit dropped exactly one subscription owned by the plugin");
    PublishBroadcast("3");
    Pump();
    Pump();
    CheckEqInt(g_saw_count, 2, "after unload the same broadcast reaches nobody (no crash)");

    /* 9) graceful degradation: a plugin without a sink */
    printf("\n- a plugin without a sink\n");
    CheckEqInt(pi_plugin_host_session_load(g_session, plain_plugin, &plain_slot), PI_OK,
               "plugin without a sink still loads");
    CheckEqInt(pi_plugin_host_session_has_event_sink(g_session, plain_slot), 0,
               "it reports no sink");
    CheckEqInt(pi_plugin_host_session_deliver_event(g_session, plain_slot, &welcome), PI_E_NOINTERFACE,
               "delivering to it degrades to NOINTERFACE (no error, no crash)");
    CheckEqInt(pi_plugin_host_session_unload(g_session, plain_slot), PI_OK, "unloaded again");

verdict:
    pi_plugin_host_session_unload_all(g_session);
    pi_plugin_event_router_stats(g_router, &stats);
    printf("\n[host] events: ready=%u ack=%u saw_broadcast=%u nobody=%u from_callback=%u\n",
           (unsigned)g_ready_count, (unsigned)g_ack_count, (unsigned)g_saw_count,
           (unsigned)g_nobody_count, (unsigned)g_from_callback_count);
    printf("[host] router: published=%llu delivered=%llu dropped_full=%llu "
           "subscriptions_dropped=%llu live_subs=%u\n",
           (unsigned long long)stats.published, (unsigned long long)stats.delivered,
           (unsigned long long)stats.dropped_full,
           (unsigned long long)stats.subscriptions_dropped,
           (unsigned)stats.subscriptions);

    pi_plugin_host_session_destroy(g_session); g_session = NULL;
    if (g_services) { pi_iunknown_release((IPiUnknown*)g_services); g_services = NULL; }
    pi_plugin_event_router_destroy(g_router); g_router = NULL;

    printf("== checks=%u failures=%d ==\n", g_checks, g_failures);
    if (g_failures) { printf("RESULT: FAIL\n"); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
