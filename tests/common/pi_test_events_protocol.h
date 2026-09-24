/*
 * piplugin tests - the event protocol shared by the events test plugin and the
 * events test host (roadmap APP-06).
 *
 * Topics and payload keys exist exactly once so the two sides cannot drift. The
 * names follow the events contract: `pi.` is reserved for the framework, so app
 * and test topics use the com.example.* prefix, and a topic is a literal name
 * (no wildcards - the framework defines none).
 */
#ifndef PI_PLUGIN_TEST_EVENTS_PROTOCOL_H
#define PI_PLUGIN_TEST_EVENTS_PROTOCOL_H

#include <string.h>

#include "piplugin/pi_plugin.h"

/* plugin -> host: "I am up and my sink is ready" (published during initialize) */
#define PI_PLUGIN_TEST_EVENTS_TOPIC_READY         "com.example.plugin.ready"
/* host -> plugin: "welcome", delivered by ADDRESS (the plugin's sink) */
#define PI_PLUGIN_TEST_EVENTS_TOPIC_WELCOME       "com.example.host.welcome"
/* plugin -> host: the reply it publishes from inside its sink */
#define PI_PLUGIN_TEST_EVENTS_TOPIC_ACK           "com.example.plugin.ack"
/* host -> everyone: "broadcast", delivered by TOPIC (subscriptions) */
#define PI_PLUGIN_TEST_EVENTS_TOPIC_BROADCAST     "com.example.host.broadcast"
/* plugin -> host: what the plugin saw on that broadcast (echoes the count) */
#define PI_PLUGIN_TEST_EVENTS_TOPIC_SAW_BROADCAST "com.example.plugin.saw_broadcast"
/* host -> nobody: used to check that unsubscribe really stops delivery */
#define PI_PLUGIN_TEST_EVENTS_TOPIC_NOBODY        "com.example.nobody.listens"

/* Payload keys */
#define PI_PLUGIN_TEST_EVENTS_KEY_PLUGIN   "plugin"   /* ready:   which plugin it is    */
#define PI_PLUGIN_TEST_EVENTS_KEY_GREETING "greeting" /* welcome: what the host says    */
#define PI_PLUGIN_TEST_EVENTS_KEY_ACKED    "acked"    /* ack:     always "1"            */
#define PI_PLUGIN_TEST_EVENTS_KEY_COUNT    "count"    /* saw_broadcast: how many seen   */
#define PI_PLUGIN_TEST_EVENTS_KEY_N        "n"        /* broadcast: which broadcast it is */

/* Read one payload value out of an event (borrowed string, or NULL).
 * Duplicate keys: the first one wins, exactly like descriptor properties. */
static inline char const* pi_plugin_test_event_payload(PiPluginEvent const* event, char const* key)
{
    uint32_t i;
    if (!event || !key || !event->payload)
    {
        return NULL;
    }
    for (i = 0; i < event->payload_count; ++i)
    {
        PiPluginProperty const* prop = &event->payload[i];
        if (prop->key && strcmp(prop->key, key) == 0)
        {
            return prop->value;
        }
    }
    return NULL;
}

#endif /* PI_PLUGIN_TEST_EVENTS_PROTOCOL_H */
