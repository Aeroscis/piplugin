/*
 * piplugin example - the plugin half of the specialized app
 *
 * It does two things the framework's own vocabulary cannot express:
 *   1. it IMPLEMENTS the app's protocol (IMyAppJobQueue, 通道 A) and declares
 *      PI_CAP_PROVIDES for MY_APP_JOB_IID - that declaration is what the app's
 *      gate checks BEFORE it instantiates anything;
 *   2. it CONSUMES a service the app provides (IMyAppInfo, 通道 B) by querying
 *      the host object it was handed in pi_initialize() and calling it.
 *
 * Run it with examples/specialized_app.
 */
#include "piplugin/pi_plugin.h"
#include "my_app_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const PiGuid SPECIALIZED_CLASS_GUID =
    PI_GUID(0x6B04D9E2, 0x1A75, 0x4C38, 0x8F, 0x26, 0x53, 0xB0, 0x7E, 0x91, 0x4A, 0xD3);

typedef struct JobPlugin {
    PiRefCountedBase base;        /* MUST be first: this is the IPiPluginBase object */
    IPiHostServices* host;        /* add-ref'd */
    IMyAppInfo*      app_info;    /* add-ref'd, NULL if the app provides none */
    int32_t          next_job_id;
    uint32_t         jobs;
} JobPlugin;

/* The app protocol lives on its own wrapper object: two vtables cannot both sit
 * at offset 0 of one object. */
typedef struct JobQueueIfc {
    PiRefCountedBase base;        /* MUST be first */
    JobPlugin*       owner;       /* add-ref'd */
} JobQueueIfc;

static uint32_t PI_CALL Plugin_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL Plugin_Release(void* self) { return pi_refcounted_release(self); }
static uint32_t PI_CALL Ifc_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL Ifc_Release(void* self) { return pi_refcounted_release(self); }

/* --------------------------------------------------------------------------
 * IMyAppJobQueue (通道 A implementation)
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Job_Submit(void* self_ptr, const char* job_name, int32_t* out_job_id)
{
    JobQueueIfc* ifc = (JobQueueIfc*)self_ptr;
    JobPlugin*   me  = ifc ? ifc->owner : NULL;

    if (!me || !job_name || !out_job_id) return PI_E_INVALIDARG;
    *out_job_id = me->next_job_id++;
    ++me->jobs;
    printf("[job plugin] accepted job '%s' as #%d (total %u)\n",
           job_name, (int)*out_job_id, (unsigned)me->jobs);
    return PI_OK;
}

static PiResult PI_CALL Job_Count(void* self_ptr, uint32_t* out_count)
{
    JobQueueIfc* ifc = (JobQueueIfc*)self_ptr;
    if (!ifc || !ifc->owner || !out_count) return PI_E_INVALIDARG;
    *out_count = ifc->owner->jobs;
    return PI_OK;
}

static PiResult PI_CALL Job_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &MY_APP_JOB_IID)) {
        *out = self_ptr; pi_refcounted_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}

static void Ifc_Destroy(void* self)
{
    JobQueueIfc* ifc = (JobQueueIfc*)self;
    if (ifc->owner) { pi_iunknown_release((IPiUnknown*)&ifc->owner->base); ifc->owner = NULL; }
    free(ifc);
}

static const IMyAppJobQueueVtbl s_job_vtbl = {
    { &Job_Qi, &Ifc_AddRef, &Ifc_Release },
    &Job_Submit, &Job_Count
};

/* --------------------------------------------------------------------------
 * IPiPluginBase
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Plugin_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    JobPlugin* me = (JobPlugin*)self_ptr;
    if (!out) return PI_E_INVALIDARG;

    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_BASE)) {
        *out = self_ptr; pi_refcounted_add_ref(self_ptr); return PI_OK;
    }
    if (pi_guid_equal(iid, &MY_APP_JOB_IID)) {
        JobQueueIfc* ifc = (JobQueueIfc*)calloc(1, sizeof(JobQueueIfc));
        if (!ifc) return PI_E_OUTOFMEMORY;
        pi_refcounted_init_with_destroy(&ifc->base, (const IPiUnknownVtbl*)&s_job_vtbl, &Ifc_Destroy);
        ifc->owner = me;
        pi_refcounted_add_ref((IPiUnknown*)&me->base);
        *out = &ifc->base;
        return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}

static PiResult PI_CALL Plugin_Init(void* self_ptr, IPiHostServices* host)
{
    JobPlugin* me = (JobPlugin*)self_ptr;
    if (me->host) return PI_OK;               /* idempotent */
    if (!host) return PI_OK;

    me->host = host;
    pi_iunknown_add_ref((IPiUnknown*)host);

    /* 通道 B: ask the app for ITS service. Not every host has one - a host
     * without it is perfectly normal, so a failure here is not an error. */
    if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host, &MY_APP_INFO_IID,
                                                 (void**)&me->app_info))) {
        printf("[job plugin] running inside '%s' (the app says %u plugin(s) are loaded)\n",
               my_app_name(me->app_info) ? my_app_name(me->app_info) : "(unnamed)",
               (unsigned)my_app_loaded_plugins(me->app_info));
    } else {
        me->app_info = NULL;
        printf("[job plugin] the app provides no IMyAppInfo (host service absent)\n");
    }
    return PI_OK;
}

static PiResult PI_CALL Plugin_Term(void* self_ptr)
{
    JobPlugin* me = (JobPlugin*)self_ptr;
    printf("[job plugin] terminating with %u job(s) accepted\n", (unsigned)me->jobs);
    return PI_OK;
}

static PiResult PI_CALL Plugin_GetView(void* self_ptr, IPiPluginView** out)
{
    (void)self_ptr;
    if (!out) return PI_E_INVALIDARG;
    *out = NULL;
    return PI_E_NOINTERFACE;      /* a worker plugin: no UI */
}

static void Plugin_Destroy(void* self)
{
    JobPlugin* me = (JobPlugin*)self;
    if (me->app_info) { pi_iunknown_release((IPiUnknown*)me->app_info); me->app_info = NULL; }
    if (me->host)     { pi_iunknown_release((IPiUnknown*)me->host);     me->host = NULL; }
    free(me);
}

static const IPiPluginBaseVtbl s_plugin_vtbl = {
    { &Plugin_Qi, &Plugin_AddRef, &Plugin_Release },
    &Plugin_Init, &Plugin_Term, &Plugin_GetView
};

/* --------------------------------------------------------------------------
 * Factory + entry point
 * -------------------------------------------------------------------------- */
typedef struct JobFactory { PiRefCountedBase base; } JobFactory;

static JobFactory         s_factory;
static PiPluginCapability s_caps[2];
static PiPluginProperty   s_props[1];
static PiPluginDescriptor s_desc;
static int                s_initialized = 0;

static uint32_t PI_CALL Factory_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL Factory_Release(void* self) { return pi_refcounted_release(self); }

static PiResult PI_CALL Factory_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_FACTORY)) {
        *out = self_ptr; pi_refcounted_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}

static const PiPluginDescriptor* PI_CALL Factory_Desc(void* self) { (void)self; return &s_desc; }
static uint32_t PI_CALL Factory_Count(void* self) { (void)self; return 1; }

static PiResult PI_CALL Factory_Guid(void* self, uint32_t index, PiGuid* guid)
{
    (void)self;
    if (index != 0 || !guid) return PI_E_INVALIDARG;
    *guid = SPECIALIZED_CLASS_GUID;
    return PI_OK;
}

static PiResult PI_CALL Factory_Create(void* self, const PiGuid* guid, IPiHostServices* host,
                                       IPiPluginBase** out)
{
    JobPlugin* plugin;
    (void)self;
    if (!guid || !out) return PI_E_INVALIDARG;
    *out = NULL;
    if (!pi_guid_equal(guid, &SPECIALIZED_CLASS_GUID)) return PI_E_NOINTERFACE;

    plugin = (JobPlugin*)calloc(1, sizeof(JobPlugin));
    if (!plugin) return PI_E_OUTOFMEMORY;
    pi_refcounted_init_with_destroy(&plugin->base, (const IPiUnknownVtbl*)&s_plugin_vtbl,
                                    &Plugin_Destroy);
    Plugin_Init(plugin, host);
    *out = (IPiPluginBase*)&plugin->base;
    return PI_OK;
}

static const IPiPluginFactoryVtbl s_factory_vtbl = {
    { &Factory_Qi, &Factory_AddRef, &Factory_Release },
    &Factory_Desc, &Factory_Count, &Factory_Guid, &Factory_Create
};

PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory) return PI_E_INVALIDARG;
    *out_factory = NULL;

    if (!s_initialized) {
        pi_refcounted_init(&s_factory.base, (const IPiUnknownVtbl*)&s_factory_vtbl);

        /* The declaration the app's gate looks at: "I implement the app's job
         * protocol". The second entry says "I can use the app's info service if
         * it has one" - OPTIONAL, so the plugin also runs in a plain host. */
        s_caps[0].iid   = MY_APP_JOB_IID;      s_caps[0].flags = PI_CAP_PROVIDES;
        s_caps[1].iid   = MY_APP_INFO_IID;     s_caps[1].flags = PI_CAP_OPTIONAL;

        s_desc.name             = "Example Job Plugin";
        s_desc.vendor           = "piplugin examples";
        s_desc.version          = "1.0.0";
        s_desc.category         = "Example/Worker";
        s_desc.api_version      = PI_PLUGIN_API_VERSION;
        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 2;

        /* Free-form metadata (APP-04): the app can read this without any GUID. */
        s_props[0].key   = "com.example.protocol";
        s_props[0].value = "job-queue/1";
        s_desc.properties     = s_props;
        s_desc.property_count = 1;

        s_initialized = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
