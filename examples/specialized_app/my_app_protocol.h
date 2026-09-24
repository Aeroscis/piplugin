/*
 * piplugin example - an app-defined protocol (roadmap 通道 A) and an
 * app-provided host service (通道 B), in one header.
 *
 * This is what an application does when it wants MORE than the framework's
 * vocabulary: it defines its own interface, gives it a random GUID, and requires
 * every plugin in its ecosystem to provide it. The plugin implements it; the
 * host gates on it before instantiation and then calls it through
 * QueryInterface - exactly like a framework interface, only the GUID and the
 * vtable are the app's.
 *
 * The reverse direction is here too: MY_APP_INFO_IID is an interface the APP
 * implements and hands to its plugins through the host services object, so a
 * plugin can ask the app which application it is running inside.
 *
 * See docs/design/interfaces.md §5 for the full walkthrough.
 */
#ifndef MY_APP_PROTOCOL_H
#define MY_APP_PROTOCOL_H

#include "piplugin/pi_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 通道 A: the app defines a protocol the PLUGIN implements
 *
 * GUID rule (interfaces.md 5.1): below 0x80000000 is the framework's reserved
 * range - never allocate there yourself. Apps use a random 128-bit UUID:
 *   python -c "import uuid; print(uuid.uuid4())"
 * -------------------------------------------------------------------------- */
#define MY_APP_JOB_IID_INIT \
    PI_GUID(0x8E52B1D7, 0x4C39, 0x4A0E, 0xB7, 0x63, 0x2F, 0x91, 0xD4, 0x58, 0x0A, 0xC6)

static const PiGuid MY_APP_JOB_IID = MY_APP_JOB_IID_INIT;

/* The protocol itself: a job queue the app knows how to drive. */
typedef struct IMyAppJobQueueVtbl {
    IPiUnknownVtbl base;

    /* Accept a job. Returns PI_OK and *out_job_id >= 0, or a failure code. */
    PiResult (PI_CALL *submit_job)(void* this_ptr, const char* job_name, int32_t* out_job_id);

    /* How many jobs have been submitted so far. */
    PiResult (PI_CALL *job_count)(void* this_ptr, uint32_t* out_count);
} IMyAppJobQueueVtbl;

typedef struct IMyAppJobQueue {
    const IMyAppJobQueueVtbl* lpVtbl;
} IMyAppJobQueue;

static inline PiResult my_app_submit_job(IMyAppJobQueue* self, const char* job_name,
                                         int32_t* out_job_id)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->submit_job) return PI_E_NOINTERFACE;
    return self->lpVtbl->submit_job((void*)self, job_name, out_job_id);
}

static inline PiResult my_app_job_count(IMyAppJobQueue* self, uint32_t* out_count)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->job_count) return PI_E_NOINTERFACE;
    return self->lpVtbl->job_count((void*)self, out_count);
}

/* --------------------------------------------------------------------------
 * 通道 B: the app defines a service the PLUGIN consumes
 *
 * Same GUID rule; this one is answered by the app's host services object (see
 * pi_plugin_host_services_create_ex and its extra_qi hook in the app example).
 * -------------------------------------------------------------------------- */
#define MY_APP_INFO_IID_INIT \
    PI_GUID(0x3F91C6A4, 0x7D28, 0x4E51, 0x8A, 0x05, 0xB9, 0x47, 0x62, 0xD1, 0x3E, 0x70)

static const PiGuid MY_APP_INFO_IID = MY_APP_INFO_IID_INIT;

typedef struct IMyAppInfoVtbl {
    IPiUnknownVtbl base;

    /* The application's name, borrowed (lives as long as the app's object). */
    const char* (PI_CALL *app_name)(void* this_ptr);

    /* How many plugins the app has loaded right now. */
    uint32_t (PI_CALL *loaded_plugins)(void* this_ptr);
} IMyAppInfoVtbl;

typedef struct IMyAppInfo {
    const IMyAppInfoVtbl* lpVtbl;
} IMyAppInfo;

static inline const char* my_app_name(IMyAppInfo* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->app_name) return NULL;
    return self->lpVtbl->app_name((void*)self);
}

static inline uint32_t my_app_loaded_plugins(IMyAppInfo* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->loaded_plugins) return 0;
    return self->lpVtbl->loaded_plugins((void*)self);
}

#ifdef __cplusplus
}
#endif

#endif /* MY_APP_PROTOCOL_H */
