/*
 * piplugin - events (channel C: structured, two-way)
 *
 * The framework already had one raw lever in each direction: a plugin can call
 * pi_host_post_message() (three integers, no topic, nothing to route on), and a
 * host can only wait for the plugin's next pi_on_idle()/pi_on_resize(). Neither
 * carries a named, structured event, and a plugin without a view has no
 * callback at all. These two interfaces fill that in without touching a single
 * published vtbl (COM rule: add interfaces, never change them):
 *
 *   IPiEventSink   - implemented by a PLUGIN (optional). The host queries it
 *                    after instantiation and pushes addressed events into it.
 *   IPiHostEvents  - provided by a HOST (optional). Plugins query it on the host
 *                    object they were given, then publish and/or subscribe.
 *
 * The host is the broker: plugins never learn about each other, the host decides
 * who receives what. A plugin that implements no sink and a host that provides
 * no events object are both perfectly normal - the query fails, and the other
 * side carries on (declare the capability PI_CAP_OPTIONAL to say so). That is
 * the same runtime negotiation IPiHostUI and IPiService already use.
 *
 * ---------------------------------------------------------------------------
 * Contract (frozen once released - see docs/design/events.md for the RFC these
 * decisions come from):
 *
 *   Threading. pi_host_events_publish() may be called from ANY thread; the host
 *   marshals it to its own main/owner thread - the thread that called
 *   pi_plugin_initialize() - and everything else (pi_event_deliver, subscription
 *   callbacks, subscribe/unsubscribe/drop_owner themselves) happens there. A
 *   sink therefore needs no locking, exactly like IPiPluginView. A host without a
 *   loop only delivers when it pumps; that timing is the host's decision.
 *
 *   Topics. UTF-8, NUL-terminated, byte-exact and case-sensitive - the same rule
 *   as descriptor property keys (APP-04). The `pi.` prefix is reserved for the
 *   framework; apps and plugins use their own prefix (com.example.*). The
 *   framework defines NO wildcard syntax: a topic is a literal name. A host may
 *   implement something cleverer behind subscribe(), but the contract stays
 *   literal so one topic cannot mean two things in two hosts.
 *
 *   Lifetime. Events, their topic strings and their payload are BORROWED for the
 *   duration of the call only; a host that queues must deep-copy. A subscription
 *   is owned by its `owner` token: when the owner goes away (drop_owner, and the
 *   host kit does that for every slot it unloads) its callbacks must never fire
 *   again. Plugins pass their instance pointer as owner - the same token
 *   pi_qt_view_shutdown_owner() uses.
 *
 *   Delivery is BEST EFFORT. No acknowledgement, no retry, no ordering promise
 *   across publishers. A host may merge or drop events (queue full); PI_OK from
 *   publish() means "accepted", not "delivered". Dropping must be observable in
 *   the host's logs/stats.
 *
 *   Payload. Key/value strings (PiPluginProperty), so an event is serialisable
 *   as-is for a future out-of-process host (FUT-05). payload may be NULL when
 *   payload_count == 0. Duplicate keys: the first one wins.
 *
 *   Origin. Filled in by the HOST when it delivers a published event, so a
 *   plugin cannot claim to be someone else. A host publishing on its own behalf
 *   passes NULL.
 *
 *   Future growth. PiEvent's field set is FROZEN: unlike PiPluginDescriptor
 *   (which carries api_version inside itself, so a reader can detect an older
 *   layout), an event has no way for a receiver to learn which layout the sender
 *   compiled - a plugin cannot know the host's API version at run time. So a
 *   richer event later means a NEW interface (IPiEventSink2 with PiEvent2), not
 *   another field here.
 * ---------------------------------------------------------------------------
 */
#ifndef PI_PLUGIN_EVENTS_H
#define PI_PLUGIN_EVENTS_H

#include "pi_plugin_unknown.h"
#include "pi_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * PiEvent - what travels
 * -------------------------------------------------------------------------- */

/* Framework event kinds. They are the framework's vocabulary (small, closed)
 * and are orthogonal to `topic`, which is the app's vocabulary (open). A host
 * may use them to route without understanding app topics at all - e.g. "REQUESTS
 * go to whoever owns the resource, NOTIFYs are broadcast" - or ignore them. */
#define PI_EVENT_NOTIFY          ((uint32_t)0x00000001u)  /* something happened  */
#define PI_EVENT_REQUEST         ((uint32_t)0x00000002u)  /* please do something */

/* Kinds at or above this value belong to apps and plugins (same rule as the
 * message codes in pi_plugin_types.h). */
#define PI_EVENT_APP_TYPE_MIN    ((uint32_t)0x80000000u)

/* Longest topic the framework will look at, including the terminator. Topics are
 * meant to be short names; a host may reject longer ones. */
#define PI_EVENT_TOPIC_MAX       128u

/* Prefix reserved for framework-defined topics; apps must not publish these. */
#define PI_EVENT_TOPIC_RESERVED_PREFIX "pi."

/* No subscription has this handle. */
#define PI_EVENT_INVALID_SUBSCRIPTION ((uint32_t)0u)

typedef struct PiEvent {
    uint32_t                 type;          /* PI_EVENT_* or >= APP_TYPE_MIN */
    const char*              topic;         /* UTF-8, non-NULL, non-empty      */
    const PiPluginProperty*  payload;       /* may be NULL (with count 0)      */
    uint32_t                 payload_count;
    const PiGuid*            origin;        /* set by the host; may be NULL    */
} PiEvent;

/* --------------------------------------------------------------------------
 * IPiEventSink - a plugin that wants to be told things
 * -------------------------------------------------------------------------- */

typedef struct IPiEventSinkVtbl {
    IPiUnknownVtbl base;

    /* Deliver one event to this plugin. Called on the host's main thread, only
     * between pi_initialize() and pi_terminate(), and only while the plugin
     * object is alive.
     *
     * Return PI_OK when the event was handled, PI_E_NOTIMPL when this plugin
     * does not care about that topic (the host may log it and moves on), or
     * PI_FAIL for "handled, but something went wrong". No return value is an
     * error condition for the host: events are best effort, and the host must
     * not retry, log a failure or take any other action because of it.
     *
     * `event` is borrowed for the duration of the call. Do not keep it, do not
     * release anything, and do not block for long - this runs on the host's
     * thread. */
    PiResult (PI_CALL *pi_event_deliver)(void* this_ptr, const PiEvent* event);
} IPiEventSinkVtbl;

typedef struct IPiEventSink {
    const IPiEventSinkVtbl* lpVtbl;
} IPiEventSink;

static inline PiResult pi_event_deliver(IPiEventSink* self, const PiEvent* event) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_event_deliver) return PI_E_NOINTERFACE;
    return self->lpVtbl->pi_event_deliver((void*)self, event);
}

/* --------------------------------------------------------------------------
 * IPiHostEvents - a host that routes events
 * -------------------------------------------------------------------------- */

/* Subscription callback. Runs on the host's main thread, with the event borrowed
 * for the duration of the call. `user_data` is what subscribe() was given. */
typedef void (*PiEventCallback)(void* user_data, const PiEvent* event);

typedef struct IPiHostEventsVtbl {
    IPiUnknownVtbl base;

    /* Publish an event (any thread). The host attributes it to the caller and
     * routes it to whoever subscribed - possibly nobody.
     *
     * Returns PI_OK when the event was accepted, PI_E_INVALIDARG when event or
     * topic is NULL/empty, or PI_E_NOTIMPL if this host does not carry events.
     * PI_OK does NOT mean "delivered": a host may drop events it cannot queue. */
    PiResult (PI_CALL *pi_host_events_publish)(void* this_ptr, const PiEvent* event);

    /* Subscribe to one topic (host main thread). `topic` is copied by the host.
     *
     * `owner` is the lifetime token: pass your plugin instance pointer (as
     * APP-08's pi_qt_view_shutdown_owner() does). NULL means "the host itself" -
     * such a subscription is never dropped automatically. The host guarantees no
     * callback after owner has been dropped, nor after the owner's module has
     * been unloaded.
     *
     * On success *out_subscription holds a non-zero handle. Returns PI_OK,
     * PI_E_INVALIDARG (NULL topic/callback/handle) or PI_E_OUTOFMEMORY. A host
     * that cannot track owners must return PI_E_NOTIMPL rather than accept a
     * subscription it cannot later drop. */
    PiResult (PI_CALL *pi_host_events_subscribe)(void* this_ptr, const char* topic,
                                                 void* owner, PiEventCallback callback,
                                                 void* user_data,
                                                 uint32_t* out_subscription);

    /* Drop one subscription (host main thread). May be called from inside a
     * callback. Returns PI_OK, or PI_E_INVALIDARG for PI_EVENT_INVALID_SUBSCRIPTION
     * and unknown handles. */
    PiResult (PI_CALL *pi_host_events_unsubscribe)(void* this_ptr, uint32_t subscription);

    /* "This owner is going away": drop every subscription registered by it
     * (host main thread). The host kit calls this for each slot it unloads, so a
     * plugin that forgets to unsubscribe cannot leave a dangling callback behind.
     * Returns PI_OK, or PI_E_INVALIDARG when owner is NULL. Dropping an owner that
     * has no subscriptions is a no-op success. */
    PiResult (PI_CALL *pi_host_events_drop_owner)(void* this_ptr, void* owner);
} IPiHostEventsVtbl;

typedef struct IPiHostEvents {
    const IPiHostEventsVtbl* lpVtbl;
} IPiHostEvents;

/* Inline helpers - NULL-safe, same shape as every other interface wrapper. */
static inline PiResult pi_host_events_publish(IPiHostEvents* self, const PiEvent* event) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_events_publish) return PI_E_NOINTERFACE;
    return self->lpVtbl->pi_host_events_publish((void*)self, event);
}

static inline PiResult pi_host_events_subscribe(IPiHostEvents* self, const char* topic,
                                                void* owner, PiEventCallback callback,
                                                void* user_data, uint32_t* out_subscription) {
    if (out_subscription) *out_subscription = PI_EVENT_INVALID_SUBSCRIPTION;
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_events_subscribe) return PI_E_NOINTERFACE;
    return self->lpVtbl->pi_host_events_subscribe((void*)self, topic, owner, callback,
                                                  user_data, out_subscription);
}

static inline PiResult pi_host_events_unsubscribe(IPiHostEvents* self, uint32_t subscription) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_events_unsubscribe) return PI_E_NOINTERFACE;
    return self->lpVtbl->pi_host_events_unsubscribe((void*)self, subscription);
}

static inline PiResult pi_host_events_drop_owner(IPiHostEvents* self, void* owner) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_host_events_drop_owner) return PI_E_NOINTERFACE;
    return self->lpVtbl->pi_host_events_drop_owner((void*)self, owner);
}

/* --------------------------------------------------------------------------
 * Convenience: does this host object carry events?
 *
 * The same QI a plugin would write, with the out-parameter conventions applied.
 * Returns PI_OK and an AddRef'd interface (release it), or PI_E_NOINTERFACE with
 * *out_events = NULL - which is the normal case for a host that has no events,
 * not an error. */
static inline PiResult pi_host_events_query(IPiHostServices* host, IPiHostEvents** out_events) {
    if (!out_events) return PI_E_INVALIDARG;
    *out_events = NULL;
    if (!host) return PI_E_INVALIDARG;
    return pi_iunknown_query_interface((IPiUnknown*)host, &PI_IID_HOST_EVENTS,
                                       (void**)out_events);
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_EVENTS_H */
