/*
 * piplugin - host-side event router (optional, roadmap APP-06)
 *
 * IPiHostEvents is an interface a host implements; this is a ready-made
 * implementation for hosts that do not want to write the bookkeeping
 * themselves. It is a HOST-side library (like piplugin_host): plugins never
 * link it, they only see the interface.
 *
 * What it does - mechanism only, no policy beyond the RFC's contract:
 *   - accepts published events from any thread and queues a deep copy of them
 *     (the caller's strings are borrowed for the call only);
 *   - fans out on pump() to the subscriptions whose topic matches exactly, on
 *     the thread that calls pump() - so the host's main thread, per the events
 *     contract;
 *   - tracks subscriptions by owner token, so pi_host_events_drop_owner() (which
 *     the host kit calls for every slot it unloads) can guarantee that a plugin
 *     which disappears leaves no callback behind;
 *   - counts what it did, because "events may be dropped" is only acceptable if
 *     the drops are observable.
 *
 * What it does NOT do: decide which events matter, decide when to pump, marshal
 * to a specific thread, or know anything about plugins. Those stay with the host.
 *
 * Wiring it into a host (see tests/test_host_events for a full example):
 *
 *     PiEventRouter* router = NULL;
 *     pi_event_router_create(&router);
 *     pi_host_services_create_ex(&OnMessage, NULL, window,
 *                                &pi_event_router_extra_qi, router,   // <- channel B
 *                                &services);
 *     ... subscribe as the broker ...
 *     pi_host_events_subscribe(pi_event_router_host_events(router),
 *                              "com.example.ready", NULL, OnReady, ctx, &handle);
 *     ...
 *     pi_event_router_pump(router);            // once per host loop iteration
 *     ...
 *     pi_event_router_destroy(router);         // after the session is gone
 */
#ifndef PI_EVENT_ROUTER_H
#define PI_EVENT_ROUTER_H

#include "piplugin/pi_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Queue depth before events start being dropped (they are best effort). */
#define PI_EVENT_ROUTER_DEFAULT_CAPACITY 256u

typedef struct PiEventRouter PiEventRouter;

/* Everything the router knows about its own behaviour. `dropped_full` and
 * `subscriptions_dropped` are the two numbers the events contract requires a
 * host to be able to show. */
typedef struct PiEventRouterStats {
    uint64_t published;              /* publish() calls accepted               */
    uint64_t delivered;              /* subscription callbacks invoked         */
    uint64_t dropped_full;           /* dropped because the queue was full     */
    uint64_t subscriptions_dropped;  /* removed by drop_owner (plugin unload)  */
    uint32_t queued;                 /* events waiting for a pump              */
    uint32_t subscriptions;          /* live subscriptions                     */
} PiEventRouterStats;

/* Create a router. The object implements IPiHostEvents and starts with refcount
 * 1; release it with pi_event_router_destroy(). */
PiResult pi_event_router_create(PiEventRouter** out_router);

/* Release the host's reference. Any plugin still holding the interface keeps it
 * alive until it releases too. NULL-safe. */
void pi_event_router_destroy(PiEventRouter* router);

/* The router as IPiHostEvents (borrowed - do NOT release it; the router owns
 * that reference). Returns NULL for a NULL router. */
IPiHostEvents* pi_event_router_host_events(PiEventRouter* router);

/* extra_qi hook for pi_host_services_create_ex() (channel B): a plugin that
 * queries PI_IID_HOST_EVENTS on the host object gets this router. `ctx` is the
 * PiEventRouter*. */
PiResult pi_event_router_extra_qi(void* ctx, const PiGuid* iid, void** out);

/* Hand every queued event to the subscriptions that existed when the pump
 * started, on the calling thread. Returns how many callbacks were invoked.
 *
 * Reentrancy rule: an event published from inside a callback is delivered by the
 * NEXT pump, never recursively - a plugin cannot be re-entered by its own
 * publish. Timing (every frame? every N ms?) belongs to the host. */
uint32_t pi_event_router_pump(PiEventRouter* router);

/* Snapshot the counters. NULL-safe (writes zeroes). */
void pi_event_router_stats(PiEventRouter* router, PiEventRouterStats* out_stats);

/* Queue capacity; 0 restores the default. Lowering it below the current depth
 * discards the oldest queued events (and counts them as dropped). */
void pi_event_router_set_capacity(PiEventRouter* router, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* PI_EVENT_ROUTER_H */
