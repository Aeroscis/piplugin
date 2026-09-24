/*
 * piplugin - host-side event router (implementation)
 *
 * See pi_event_router.h for the contract and the layering discipline. This file
 * is deliberately plain C with no framework dependency beyond the public
 * headers, so any host can link it.
 */
#include "pi_event_router.h"

#include <stdlib.h>
#include <string.h>

#if PI_PLATFORM_WINDOWS
#  include <windows.h>
#  define PI_PLUGIN_ROUTER_LOCK_INIT(l)   InitializeCriticalSection(l)
#  define PI_PLUGIN_ROUTER_LOCK_FREE(l)   DeleteCriticalSection(l)
#  define PI_PLUGIN_ROUTER_LOCK_ACQUIRE(l) EnterCriticalSection(l)
#  define PI_PLUGIN_ROUTER_LOCK_RELEASE(l) LeaveCriticalSection(l)
typedef CRITICAL_SECTION PiPluginRouterLock;
#else
#  include <pthread.h>
#  define PI_PLUGIN_ROUTER_LOCK_INIT(l)    pthread_mutex_init((l), NULL)
#  define PI_PLUGIN_ROUTER_LOCK_FREE(l)    pthread_mutex_destroy(l)
#  define PI_PLUGIN_ROUTER_LOCK_ACQUIRE(l) pthread_mutex_lock(l)
#  define PI_PLUGIN_ROUTER_LOCK_RELEASE(l) pthread_mutex_unlock(l)
typedef pthread_mutex_t PiPluginRouterLock;
#endif

/* --------------------------------------------------------------------------
 * Internal types
 * -------------------------------------------------------------------------- */

/* A queued event owns its copies: the publisher's strings are only valid for the
 * duration of its publish() call. */
typedef struct PiPluginQueuedEvent {
    uint32_t          type;
    char*             topic;         /* owned, NUL-terminated            */
    PiPluginProperty* payload;       /* owned array (may be NULL)        */
    uint32_t          payload_count;
    int               has_origin;
    PiGuid            origin;
} PiPluginQueuedEvent;

typedef struct PiPluginRouterSubscription {
    uint32_t         handle;
    char*            topic;          /* owned */
    void*            owner;          /* NULL = the host itself */
    PiPluginEventCallback  callback;
    void*            user_data;
} PiPluginRouterSubscription;

/* What a pump collects for one event before calling anyone. */
typedef struct PiPluginRouterMatch {
    uint32_t        handle;
    PiPluginEventCallback callback;
    void*           user_data;
} PiPluginRouterMatch;

struct PiPluginEventRouter {
    PiRefCountedBase  base;          /* MUST be first: this is an IPiPluginHostEvents */

    PiPluginRouterLock      lock;          /* publish() may come from any thread */

    PiPluginQueuedEvent*    queue;
    uint32_t          capacity;
    uint32_t          head;          /* index of the oldest queued event */
    uint32_t          queued;

    PiPluginRouterSubscription* subs;
    uint32_t          sub_count;
    uint32_t          sub_capacity;
    uint32_t          next_handle;

    PiPluginEventRouterStats stats;
};

/* --------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */
static char* RouterStrdup(const char* s)
{
    size_t n;
    char* copy;
    if (!s) return NULL;
    n = strlen(s) + 1;
    copy = (char*)malloc(n);
    if (!copy) return NULL;
    memcpy(copy, s, n);
    return copy;
}

static void RouterFreeQueued(PiPluginQueuedEvent* ev)
{
    uint32_t i;
    if (!ev) return;
    free(ev->topic);
    for (i = 0; i < ev->payload_count; ++i) {
        free((void*)ev->payload[i].key);
        free((void*)ev->payload[i].value);
    }
    free(ev->payload);
    memset(ev, 0, sizeof(*ev));
}

/* Deep copy one event into the queue slot. Returns 0 when out of memory (the
 * caller counts it as dropped - events are best effort, never a hard failure). */
static int RouterCopyEvent(PiPluginQueuedEvent* dst, const PiPluginEvent* src)
{
    uint32_t i;

    memset(dst, 0, sizeof(*dst));
    dst->type = src->type;
    dst->topic = RouterStrdup(src->topic);
    if (!dst->topic) return 0;

    if (src->payload && src->payload_count > 0) {
        dst->payload = (PiPluginProperty*)calloc(src->payload_count, sizeof(PiPluginProperty));
        if (!dst->payload) { RouterFreeQueued(dst); return 0; }
        dst->payload_count = src->payload_count;
        for (i = 0; i < src->payload_count; ++i) {
            dst->payload[i].key = src->payload[i].key ? RouterStrdup(src->payload[i].key) : NULL;
            dst->payload[i].value = src->payload[i].value ? RouterStrdup(src->payload[i].value) : NULL;
            if ((src->payload[i].key && !dst->payload[i].key) ||
                (src->payload[i].value && !dst->payload[i].value)) {
                RouterFreeQueued(dst);
                return 0;
            }
        }
    } else {
        dst->payload = NULL;
        dst->payload_count = 0;
    }

    if (src->origin) {
        dst->has_origin = 1;
        dst->origin = *src->origin;
    }
    return 1;
}

static uint32_t RouterFindSubscription(PiPluginEventRouter* router, uint32_t handle)
{
    uint32_t i;
    for (i = 0; i < router->sub_count; ++i) {
        if (router->subs[i].handle == handle) return i;
    }
    return 0xFFFFFFFFu;
}

static void RouterRemoveSubscriptionAt(PiPluginEventRouter* router, uint32_t index)
{
    if (index >= router->sub_count) return;
    free(router->subs[index].topic);
    if (index + 1 < router->sub_count) {
        memmove(&router->subs[index], &router->subs[index + 1],
                (size_t)(router->sub_count - index - 1) * sizeof(PiPluginRouterSubscription));
    }
    --router->sub_count;
}

/* --------------------------------------------------------------------------
 * IPiPluginHostEvents implementation
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Router_Publish(void* self_ptr, const PiPluginEvent* event)
{
    PiPluginEventRouter* me = (PiPluginEventRouter*)self_ptr;
    PiPluginQueuedEvent* slot;
    size_t topic_len;

    if (!me || !event || !event->topic || !event->topic[0]) return PI_E_INVALIDARG;
    topic_len = strlen(event->topic);
    if (topic_len >= PI_PLUGIN_EVENT_TOPIC_MAX) return PI_E_INVALIDARG;   /* documented limit */

    PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&me->lock);

    ++me->stats.published;

    if (me->queued >= me->capacity || me->capacity == 0) {
        /* Best effort: drop the NEW event (oldest-first is the host's other
         * legitimate policy; this router keeps FIFO of what it accepted) and
         * make the drop observable. */
        ++me->stats.dropped_full;
        me->stats.queued = me->queued;
        PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
        return PI_OK;
    }

    slot = &me->queue[(me->head + me->queued) % me->capacity];
    ++me->queued;
    if (!RouterCopyEvent(slot, event)) {
        /* Out of memory: same treatment as a full queue. */
        --me->queued;
        ++me->stats.dropped_full;
        me->stats.queued = me->queued;
        PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
        return PI_OK;
    }

    me->stats.queued = me->queued;
    PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
    return PI_OK;
}

static PiResult PI_CALL Router_Subscribe(void* self_ptr, const char* topic, void* owner,
                                         PiPluginEventCallback callback, void* user_data,
                                         uint32_t* out_subscription)
{
    PiPluginEventRouter* me = (PiPluginEventRouter*)self_ptr;
    PiPluginRouterSubscription* sub;

    if (!me || !topic || !topic[0] || !callback || !out_subscription) return PI_E_INVALIDARG;
    if (strlen(topic) >= PI_PLUGIN_EVENT_TOPIC_MAX) return PI_E_INVALIDARG;

    PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&me->lock);

    if (me->sub_count == me->sub_capacity) {
        uint32_t grown = me->sub_capacity ? me->sub_capacity * 2 : 8;
        PiPluginRouterSubscription* resized =
            (PiPluginRouterSubscription*)realloc(me->subs, (size_t)grown * sizeof(PiPluginRouterSubscription));
        if (!resized) {
            PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
            return PI_E_OUTOFMEMORY;
        }
        me->subs = resized;
        me->sub_capacity = grown;
    }

    sub = &me->subs[me->sub_count];
    memset(sub, 0, sizeof(*sub));
    sub->topic = RouterStrdup(topic);
    if (!sub->topic) {
        PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
        return PI_E_OUTOFMEMORY;
    }
    sub->owner     = owner;
    sub->callback  = callback;
    sub->user_data = user_data;
    sub->handle    = ++me->next_handle;
    if (sub->handle == PI_PLUGIN_EVENT_INVALID_SUBSCRIPTION) sub->handle = ++me->next_handle;
    ++me->sub_count;

    *out_subscription = sub->handle;
    me->stats.subscriptions = me->sub_count;
    PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
    return PI_OK;
}

static PiResult PI_CALL Router_Unsubscribe(void* self_ptr, uint32_t subscription)
{
    PiPluginEventRouter* me = (PiPluginEventRouter*)self_ptr;
    uint32_t index;

    if (!me || subscription == PI_PLUGIN_EVENT_INVALID_SUBSCRIPTION) return PI_E_INVALIDARG;

    PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&me->lock);
    index = RouterFindSubscription(me, subscription);
    if (index == 0xFFFFFFFFu) {
        PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
        return PI_E_INVALIDARG;
    }
    RouterRemoveSubscriptionAt(me, index);
    me->stats.subscriptions = me->sub_count;
    PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
    return PI_OK;
}

static PiResult PI_CALL Router_DropOwner(void* self_ptr, void* owner)
{
    PiPluginEventRouter* me = (PiPluginEventRouter*)self_ptr;
    uint32_t i = 0;
    uint32_t dropped = 0;

    if (!me || !owner) return PI_E_INVALIDARG;

    PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&me->lock);
    while (i < me->sub_count) {
        if (me->subs[i].owner == owner) {
            RouterRemoveSubscriptionAt(me, i);
            ++dropped;
        } else {
            ++i;
        }
    }
    me->stats.subscriptions_dropped += dropped;
    me->stats.subscriptions = me->sub_count;
    PI_PLUGIN_ROUTER_LOCK_RELEASE(&me->lock);
    return PI_OK;
}

/* 本模块内的薄封装（避免 C 里取 dllimport 函数地址的 C4232 警告）。 */
static uint32_t PI_CALL Router_AddRef(void* self_ptr) { return pi_refcounted_add_ref(self_ptr); }
static uint32_t PI_CALL Router_Release(void* self_ptr) { return pi_refcounted_release(self_ptr); }

static PiResult PI_CALL Router_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_HOST_EVENTS)) {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static const IPiPluginHostEventsVtbl s_router_vtbl = {
    { &Router_Qi, &Router_AddRef, &Router_Release },
    &Router_Publish,
    &Router_Subscribe,
    &Router_Unsubscribe,
    &Router_DropOwner
};

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
static void Router_Destroy(void* self_ptr)
{
    PiPluginEventRouter* me = (PiPluginEventRouter*)self_ptr;
    uint32_t i;

    for (i = 0; i < me->queued; ++i) {
        RouterFreeQueued(&me->queue[(me->head + i) % me->capacity]);
    }
    free(me->queue);
    for (i = 0; i < me->sub_count; ++i) free(me->subs[i].topic);
    free(me->subs);

    PI_PLUGIN_ROUTER_LOCK_FREE(&me->lock);
    free(me);
}

PiResult pi_plugin_event_router_create(PiPluginEventRouter** out_router)
{
    PiPluginEventRouter* router;

    if (!out_router) return PI_E_INVALIDARG;
    *out_router = NULL;

    router = (PiPluginEventRouter*)calloc(1, sizeof(PiPluginEventRouter));
    if (!router) return PI_E_OUTOFMEMORY;

    PI_PLUGIN_ROUTER_LOCK_INIT(&router->lock);

    router->capacity = PI_PLUGIN_EVENT_ROUTER_DEFAULT_CAPACITY;
    router->queue = (PiPluginQueuedEvent*)calloc(router->capacity, sizeof(PiPluginQueuedEvent));
    if (!router->queue) {
        PI_PLUGIN_ROUTER_LOCK_FREE(&router->lock);
        free(router);
        return PI_E_OUTOFMEMORY;
    }

    pi_refcounted_init_with_destroy(&router->base, (const IPiUnknownVtbl*)&s_router_vtbl,
                                    &Router_Destroy);
    *out_router = router;
    return PI_OK;
}

void pi_plugin_event_router_destroy(PiPluginEventRouter* router)
{
    if (!router) return;
    pi_iunknown_release((IPiUnknown*)&router->base);
}

IPiPluginHostEvents* pi_plugin_event_router_host_events(PiPluginEventRouter* router)
{
    return router ? (IPiPluginHostEvents*)&router->base : NULL;
}

PiResult pi_plugin_event_router_extra_qi(void* ctx, const PiGuid* iid, void** out)
{
    PiPluginEventRouter* me = (PiPluginEventRouter*)ctx;
    if (!out) return PI_E_INVALIDARG;
    if (me && pi_guid_equal(iid, &PI_PLUGIN_IID_HOST_EVENTS)) {
        *out = &me->base;
        pi_refcounted_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

/* --------------------------------------------------------------------------
 * Pump
 * -------------------------------------------------------------------------- */
uint32_t pi_plugin_event_router_pump(PiPluginEventRouter* router)
{
    uint32_t batch, i;
    uint32_t delivered = 0;

    if (!router) return 0;

    PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&router->lock);
    batch = router->queued;   /* events published BY callbacks wait for next pump */
    PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);

    for (i = 0; i < batch; ++i) {
        PiPluginQueuedEvent  event;
        PiPluginEvent        view;
        uint32_t       j;

        /* Take the oldest event out of the queue (its strings move with it). */
        PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&router->lock);
        if (router->queued == 0) {
            PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);
            break;
        }
        event = router->queue[router->head];
        memset(&router->queue[router->head], 0, sizeof(PiPluginQueuedEvent));
        router->head = (router->head + 1) % router->capacity;
        --router->queued;
        router->stats.queued = router->queued;

        /* Snapshot the matching subscriptions (handle + callback + user_data)
         * while holding the lock, then call them with it released: a callback may
         * publish/subscribe/unsubscribe, and holding our own lock across plugin
         * code would invite a deadlock.
         *
         * Matching is by HANDLE, not by array index: an unsubscribe (or a
         * drop_owner) from inside a callback compacts the table, so an index
         * collected up front can end up pointing at a *different* subscription -
         * which would silently skip one subscriber and call another twice.
         * Re-checking the handle before each call keeps "everyone who was
         * subscribed at pump time gets it exactly once, unless they left". */
        {
            PiPluginRouterMatch* matches = NULL;
            uint32_t       match_count = 0;
            uint32_t       skipped = 0;

            if (router->sub_count > 0) {
                matches = (PiPluginRouterMatch*)malloc((size_t)router->sub_count * sizeof(PiPluginRouterMatch));
            }
            if (matches) {
                for (j = 0; j < router->sub_count; ++j) {
                    if (strcmp(router->subs[j].topic, event.topic) == 0) {
                        matches[match_count].handle = router->subs[j].handle;
                        matches[match_count].callback = router->subs[j].callback;
                        matches[match_count].user_data = router->subs[j].user_data;
                        ++match_count;
                    }
                }
            } else if (router->sub_count > 0) {
                skipped = 1;   /* out of memory: best effort means "lose it" */
            }
            PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);

            view.type          = event.type;
            view.topic         = event.topic;
            view.payload       = event.payload;
            view.payload_count = event.payload_count;
            view.origin        = event.has_origin ? &event.origin : NULL;

            for (j = 0; j < match_count; ++j) {
                uint32_t index;

                PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&router->lock);
                index = RouterFindSubscription(router, matches[j].handle);
                PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);

                if (index != 0xFFFFFFFFu) {
                    matches[j].callback(matches[j].user_data, &view);
                    ++delivered;
                }
            }
            free(matches);

            if (skipped) {
                PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&router->lock);
                ++router->stats.dropped_full;
                PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);
            }
        }

        RouterFreeQueued(&event);
    }

    if (delivered) {
        PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&router->lock);
        router->stats.delivered += delivered;
        PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);
    }
    return delivered;
}

void pi_plugin_event_router_stats(PiPluginEventRouter* router, PiPluginEventRouterStats* out_stats)
{
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!router) return;
    PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&router->lock);
    *out_stats = router->stats;
    out_stats->queued = router->queued;
    out_stats->subscriptions = router->sub_count;
    PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);
}

void pi_plugin_event_router_set_capacity(PiPluginEventRouter* router, uint32_t capacity)
{
    PiPluginQueuedEvent* resized;
    uint32_t i;
    uint32_t keep;

    if (!router) return;
    if (capacity == 0) capacity = PI_PLUGIN_EVENT_ROUTER_DEFAULT_CAPACITY;

    PI_PLUGIN_ROUTER_LOCK_ACQUIRE(&router->lock);
    if (capacity == router->capacity) {
        PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);
        return;
    }

    /* Rebuild the ring with the surviving events in order. Too small: keep the
     * NEWEST `capacity` events and count the dropped ones. */
    resized = (PiPluginQueuedEvent*)calloc(capacity, sizeof(PiPluginQueuedEvent));
    if (!resized) {
        PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);
        return;   /* keep the old queue; dropping nothing is the safer failure */
    }

    keep = router->queued < capacity ? router->queued : capacity;
    if (router->queued > keep) {
        uint32_t discard = router->queued - keep;
        for (i = 0; i < discard; ++i) {
            RouterFreeQueued(&router->queue[(router->head + i) % router->capacity]);
        }
        router->stats.dropped_full += discard;
        router->head = (router->head + discard) % router->capacity;
        router->queued = keep;
    }
    for (i = 0; i < keep; ++i) {
        resized[i] = router->queue[(router->head + i) % router->capacity];
    }
    free(router->queue);
    router->queue = resized;
    router->capacity = capacity;
    router->head = 0;
    router->queued = keep;
    router->stats.queued = keep;
    PI_PLUGIN_ROUTER_LOCK_RELEASE(&router->lock);
}
