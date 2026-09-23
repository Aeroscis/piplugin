/*
 * piplugin example - a service plugin (headless, IPiService)
 *
 * A plugin with no UI at all: a host's main loop calls start / poll / stop and
 * the plugin does the work. This is what a task server, an importer or a
 * background worker looks like in piplugin.
 *
 * Run it with examples/minimal_host (a window is harmless: the service ignores
 * it) or with the official headless test host:
 *
 *     pi_example_minimal_host.exe pi_example_service.dll
 *
 * What is worth copying from here:
 *   - the descriptor declares PI_IID_SERVICE PROVIDES and requires nothing, so
 *     any host - GUI or headless - can host it;
 *   - ONE plugin object carries IPiPluginBase, and QueryInterface hands out a
 *     small wrapper for IPiService. Two interfaces need two vtable layouts, and
 *     a vtable cannot live at offset 0 twice: returning the same pointer for both
 *     IIDs would make pi_service_start() call the slot that is actually
 *     pi_initialize(). The wrapper owns a reference to the plugin, so the plugin
 *     cannot die while the host still holds its service;
 *   - pi_service_stop() is IDEMPOTENT (the host's unload sequence calls it again);
 *   - poll() reports progress through pi_host_post_message() instead of blocking
 *     the host's thread.
 */
#include "piplugin/pi_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A random 128-bit class GUID (see docs/design/interfaces.md 5.1). */
static const PiGuid EXAMPLE_SERVICE_CLASS_GUID =
    PI_GUID(0x1A9C4E70, 0x6B23, 0x4D18, 0x9F, 0x52, 0x83, 0xC1, 0x47, 0x0E, 0xB6, 0x29);

/* Message code posted to the host while working (>= 0x80000000 = app range). */
#define EXAMPLE_MSG_TICK ((uint32_t)0x8001u)

typedef struct ExampleService {
    PiRefCountedBase base;      /* MUST be first: this is the IPiPluginBase object */
    IPiHostServices* host;      /* borrowed from initialize: we take a reference */
    int32_t          status;
    uint32_t         interval;  /* report every N polls */
    uint32_t         polls;
    int              running;
} ExampleService;

/* The IPiService side: a wrapper with its own vtable and a reference to the
 * plugin (same containment pattern the framework uses for IPiHostUI). */
typedef struct ExampleServiceIfc {
    PiRefCountedBase base;      /* MUST be first */
    ExampleService*  owner;     /* add-ref'd */
} ExampleServiceIfc;

static uint32_t PI_CALL Plugin_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL Plugin_Release(void* self) { return pi_refcounted_release(self); }
static uint32_t PI_CALL Ifc_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL Ifc_Release(void* self) { return pi_refcounted_release(self); }

/* --------------------------------------------------------------------------
 * IPiService slots (self_ptr is the wrapper; the state lives in the owner)
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Service_Start(void* self_ptr, const PiServiceOption* options,
                                      uint32_t option_count)
{
    ExampleServiceIfc* ifc = (ExampleServiceIfc*)self_ptr;
    ExampleService*    me  = ifc ? ifc->owner : NULL;
    uint32_t i;

    if (!me) return PI_E_INVALIDARG;
    if (me->running) return PI_OK;   /* idempotent */

    me->interval = 1;                /* every poll by default */
    for (i = 0; options && i < option_count; ++i) {
        if (options[i].key && options[i].value && strcmp(options[i].key, "interval") == 0) {
            int parsed = atoi(options[i].value);
            me->interval = (parsed > 0) ? (uint32_t)parsed : 1u;
        }
    }

    me->polls   = 0;
    me->running = 1;
    me->status  = PI_SERVICE_RUNNING;
    printf("[example service] started (report every %u poll(s))\n", (unsigned)me->interval);
    return PI_OK;
}

static PiResult ServiceDoStop(ExampleService* me)
{
    if (!me) return PI_E_INVALIDARG;
    if (me->running) printf("[example service] stopping after %u poll(s)\n", (unsigned)me->polls);
    me->running = 0;
    me->status  = PI_SERVICE_STOPPED;
    return PI_OK;                    /* idempotent: the unload sequence calls this again */
}

static PiResult PI_CALL Service_Stop(void* self_ptr)
{
    ExampleServiceIfc* ifc = (ExampleServiceIfc*)self_ptr;
    if (!ifc || !ifc->owner) return PI_E_INVALIDARG;
    return ServiceDoStop(ifc->owner);
}

static PiResult PI_CALL Service_Poll(void* self_ptr)
{
    ExampleServiceIfc* ifc = (ExampleServiceIfc*)self_ptr;
    ExampleService*    me  = ifc ? ifc->owner : NULL;
    if (!me) return PI_E_INVALIDARG;
    if (!me->running) return PI_FAIL;   /* do not pretend to work when stopped */

    ++me->polls;
    if (me->interval && (me->polls % me->interval) == 0 && me->host) {
        /* "Here is my progress" - the host decides what to do with it. */
        pi_host_post_message(me->host, EXAMPLE_MSG_TICK, (uintptr_t)me->polls, 0);
    }
    return PI_OK;
}

static PiResult PI_CALL Service_GetStatus(void* self_ptr, int32_t* out_status)
{
    ExampleServiceIfc* ifc = (ExampleServiceIfc*)self_ptr;
    if (!ifc || !ifc->owner || !out_status) return PI_E_INVALIDARG;
    *out_status = ifc->owner->status;
    return PI_OK;
}

static PiResult PI_CALL Service_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_SERVICE)) {
        *out = self_ptr; pi_refcounted_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}

static void Ifc_Destroy(void* self)
{
    ExampleServiceIfc* ifc = (ExampleServiceIfc*)self;
    if (ifc->owner) { pi_iunknown_release((IPiUnknown*)&ifc->owner->base); ifc->owner = NULL; }
    free(ifc);
}

static const IPiServiceVtbl s_service_vtbl = {
    { &Service_Qi, &Ifc_AddRef, &Ifc_Release },
    &Service_Start, &Service_Stop, &Service_Poll, &Service_GetStatus
};

/* --------------------------------------------------------------------------
 * IPiPluginBase (self_ptr is the ExampleService instance)
 * -------------------------------------------------------------------------- */
static PiResult PI_CALL Plugin_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    ExampleService* me = (ExampleService*)self_ptr;
    if (!out) return PI_E_INVALIDARG;

    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_BASE)) {
        *out = self_ptr; pi_refcounted_add_ref(self_ptr); return PI_OK;
    }
    if (pi_guid_equal(iid, &PI_IID_SERVICE)) {
        ExampleServiceIfc* ifc = (ExampleServiceIfc*)calloc(1, sizeof(ExampleServiceIfc));
        if (!ifc) return PI_E_OUTOFMEMORY;
        pi_refcounted_init_with_destroy(&ifc->base, (const IPiUnknownVtbl*)&s_service_vtbl,
                                        &Ifc_Destroy);
        ifc->owner = me;
        pi_refcounted_add_ref((IPiUnknown*)&me->base);
        *out = &ifc->base;
        return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}

static PiResult PI_CALL Plugin_Init(void* self_ptr, IPiHostServices* host)
{
    ExampleService* me = (ExampleService*)self_ptr;
    if (me->host) return PI_OK;                     /* idempotent */
    if (!host) return PI_OK;
    me->host = host;
    pi_iunknown_add_ref((IPiUnknown*)host);         /* we keep it: take a reference */
    return PI_OK;
}

static PiResult PI_CALL Plugin_Term(void* self_ptr)
{
    ExampleService* me = (ExampleService*)self_ptr;
    if (me->running) ServiceDoStop(me);   /* stop before the module goes away */
    return PI_OK;
}

static PiResult PI_CALL Plugin_GetView(void* self_ptr, IPiPluginView** out)
{
    (void)self_ptr;
    if (!out) return PI_E_INVALIDARG;
    *out = NULL;
    return PI_E_NOINTERFACE;                        /* headless plugin */
}

static void Plugin_Destroy(void* self)
{
    ExampleService* me = (ExampleService*)self;
    if (me->host) { pi_iunknown_release((IPiUnknown*)me->host); me->host = NULL; }
    free(me);
}

static const IPiPluginBaseVtbl s_plugin_vtbl = {
    { &Plugin_Qi, &Plugin_AddRef, &Plugin_Release },
    &Plugin_Init, &Plugin_Term, &Plugin_GetView
};

/* --------------------------------------------------------------------------
 * Factory + entry point
 * -------------------------------------------------------------------------- */
typedef struct ExampleFactory { PiRefCountedBase base; } ExampleFactory;

static ExampleFactory     s_factory;
static PiPluginCapability s_caps[1];
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
    *guid = EXAMPLE_SERVICE_CLASS_GUID;
    return PI_OK;
}

static PiResult PI_CALL Factory_Create(void* self, const PiGuid* guid, IPiHostServices* host,
                                       IPiPluginBase** out)
{
    ExampleService* plugin;
    (void)self;
    if (!guid || !out) return PI_E_INVALIDARG;
    *out = NULL;
    if (!pi_guid_equal(guid, &EXAMPLE_SERVICE_CLASS_GUID)) return PI_E_NOINTERFACE;

    plugin = (ExampleService*)calloc(1, sizeof(ExampleService));
    if (!plugin) return PI_E_OUTOFMEMORY;
    pi_refcounted_init_with_destroy(&plugin->base, (const IPiUnknownVtbl*)&s_plugin_vtbl,
                                    &Plugin_Destroy);
    plugin->status = PI_SERVICE_STOPPED;
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

        /* PROVIDES a service, requires nothing: any host can run this plugin. */
        s_caps[0].iid   = PI_IID_SERVICE;
        s_caps[0].flags = PI_CAP_PROVIDES;

        s_desc.name             = "Example Service";
        s_desc.vendor           = "piplugin examples";
        s_desc.version          = "1.0.0";
        s_desc.category         = "Example/Service";
        s_desc.api_version      = PI_PLUGIN_API_VERSION;
        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 1;
        s_initialized = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
