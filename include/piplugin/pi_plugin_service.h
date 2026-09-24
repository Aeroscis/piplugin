/*
 * piplugin - IPiPluginService
 *
 * Optional capability interface for headless / server-side plugins.
 *
 * Use case: a distributed application where a task server loads plugins
 * that provide network computation. Such plugins have no GUI; instead of
 * IPiPluginView they implement IPiPluginService. The server host queries
 * PI_PLUGIN_IID_SERVICE and drives the plugin's lifecycle with start/stop/poll,
 * while never touching anything GUI-related.
 *
 * The interface is deliberately transport-agnostic: configuration is a
 * simple key/value string list, so a service plugin can define its own
 * config schema (ports, endpoints, ...).
 */
#ifndef PI_PLUGIN_SERVICE_H
#define PI_PLUGIN_SERVICE_H

#include <pibase/pi_base.h>

#include "pi_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Key/value configuration option */
typedef struct PiPluginServiceOption {
    char const* key;   /* NUL-terminated, UTF-8 */
    char const* value; /* NUL-terminated, UTF-8 */
} PiPluginServiceOption;

/* Service status codes returned by pi_plugin_service_get_status */
#define PI_PLUGIN_SERVICE_STOPPED  ((int32_t)0)
#define PI_PLUGIN_SERVICE_STARTING ((int32_t)1)
#define PI_PLUGIN_SERVICE_RUNNING  ((int32_t)2)
#define PI_PLUGIN_SERVICE_ERROR    ((int32_t)3)

typedef struct IPiPluginServiceVtbl {
    IPiUnknownVtbl base;

    /* Start the service with the given options (array may be NULL when
     * option_count == 0). Returns PI_OK, or PI_E_MISSINGCAPABILITY when
     * a required option is absent, or PI_FAIL on startup error. */
    PiResult(PI_CALL* pi_plugin_service_start)(void*                        this_ptr,
                                               PiPluginServiceOption const* options,
                                               uint32_t                     option_count);

    /* Stop the service and release its resources. Idempotent. */
    PiResult(PI_CALL* pi_plugin_service_stop)(void* this_ptr);

    /* Poll the service: give it a slice of time to process I/O.
     * A server host calls this from its own main loop (analogous to
     * IPiPluginView::pi_plugin_on_idle for GUI plugins). May be called on any
     * thread the service declares; default expectation: host thread. */
    PiResult(PI_CALL* pi_plugin_service_poll)(void* this_ptr);

    /* Query current status (PI_SERVICE_*). */
    PiResult(PI_CALL* pi_plugin_service_get_status)(void* this_ptr, int32_t* out_status);
} IPiPluginServiceVtbl;

typedef struct IPiPluginService {
    IPiPluginServiceVtbl const* lpVtbl;
} IPiPluginService;

/* Inline helpers */
static inline PiResult pi_plugin_service_start(IPiPluginService*            self,
                                               PiPluginServiceOption const* options,
                                               uint32_t                     option_count)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_service_start)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_service_start((void*)self, options, option_count);
}

static inline PiResult pi_plugin_service_stop(IPiPluginService* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_service_stop)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_service_stop((void*)self);
}

static inline PiResult pi_plugin_service_poll(IPiPluginService* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_service_poll)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_service_poll((void*)self);
}

static inline PiResult pi_plugin_service_get_status(IPiPluginService* self, int32_t* out_status)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_plugin_service_get_status)
    {
        return PI_E_INVALIDARG;
    }
    return self->lpVtbl->pi_plugin_service_get_status((void*)self, out_status);
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_SERVICE_H */
