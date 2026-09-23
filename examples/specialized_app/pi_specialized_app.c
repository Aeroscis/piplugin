/*
 * piplugin example - the APP half of the specialized app
 *
 * This is the shape the roadmap calls "通道 A + 通道 B": an application that
 *   1. defines its own protocol (my_app_protocol.h) and REQUIRES every plugin in
 *      its ecosystem to implement it (pi_host_session_require -> the gate runs
 *      before instantiation, so a non-conforming plugin is rejected without ever
 *      being created);
 *   2. offers ITS OWN service to plugins through the host services object
 *      (pi_host_services_create_ex + extra_qi, APP-01);
 *   3. consumes the plugin's protocol through QueryInterface - the plugin's side
 *      of the conversation is examples/specialized_app/pi_specialized_plugin.c.
 *
 * It also demonstrates 通道 C (events) in one line: the app is the broker, so it
 * can forward anything it likes to whichever plugin it likes. (Kept out of this
 * example's code to stay short; see tests/test_host_events.)
 *
 * Run: pi_specialized_app.exe [plugin.dll] [non-conforming-plugin.dll]
 *      defaults: pi_example_specialized_plugin.dll, pi_example_service.dll
 *      (the second one only needs to be a plugin that does NOT implement the
 *      protocol - ANY plugin works, which is the point of the gate)
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"
#include "my_app_protocol.h"

#include <stdio.h>
#include <string.h>

static PiPluginHostSession* g_session = NULL;
static uint32_t             g_loaded = 0;

/* --------------------------------------------------------------------------
 * 通道 B: the app's own service
 *
 * This object lives in the app; plugins reach it by querying the host object for
 * MY_APP_INFO_IID. The app installs the hook below so the framework's default
 * host object forwards that query here.
 * -------------------------------------------------------------------------- */
typedef struct AppInfo {
    PiRefCountedBase base;      /* MUST be first */
    const char*      name;
} AppInfo;

static const char* PI_CALL AppInfo_Name(void* self_ptr)
{
    return ((AppInfo*)self_ptr)->name;
}

static uint32_t PI_CALL AppInfo_Loaded(void* self_ptr)
{
    (void)self_ptr;
    return g_loaded;            /* something only the app knows */
}

static PiResult PI_CALL AppInfo_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &MY_APP_INFO_IID)) {
        *out = self_ptr; pi_refcounted_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}

static uint32_t PI_CALL AppInfo_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL AppInfo_Release(void* self) { return pi_refcounted_release(self); }

static const IMyAppInfoVtbl s_app_info_vtbl = {
    { &AppInfo_Qi, &AppInfo_AddRef, &AppInfo_Release },
    &AppInfo_Name, &AppInfo_Loaded
};

static AppInfo s_app_info;      /* static: lives as long as the app, no destroy */

/* The extra_qi hook pi_host_services_create_ex() forwards unknown IIDs to. */
static PiResult PI_CALL AppExtraQi(void* ctx, const PiGuid* iid, void** out)
{
    (void)ctx;
    if (pi_guid_equal(iid, &MY_APP_INFO_IID)) {
        *out = &s_app_info.base;
        pi_refcounted_add_ref((IPiUnknown*)*out);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

/* -------------------------------------------------------------------------- */
static void OnHostMessage(void* user_data, uint32_t msg, uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    printf("[app] plugin said msg=0x%04X (%llu)\n", msg, (unsigned long long)wparam);
}

static void OnSessionLog(void* user_data, const char* message)
{
    (void)user_data;
    printf("[kit] %s\n", message);
}

/* Try to load one plugin; report whether the app's gate accepted it. */
static int LoadAndCheck(const char* dll, IMyAppJobQueue** out_jobs)
{
    uint32_t slot = PI_HOST_SESSION_INVALID_SLOT;
    PiResult hr;

    /* Count it as loaded while it is being loaded, so a plugin asking the app
     * (through MY_APP_INFO_IID) sees the count including itself - that is what
     * "how many plugins are loaded" means to a user. */
    ++g_loaded;
    hr = pi_host_session_load(g_session, dll, &slot);

    if (PI_FAILED(hr)) {
        /* PI_E_MISSINGCAPABILITY here means: the plugin does not PROVIDES the
         * protocol this app requires. The gate ran BEFORE instantiation. */
        --g_loaded;
        printf("[app] rejected '%s' (hr=%d): %s\n", dll, (int)hr,
               pi_host_session_last_error(g_session));
        return 0;
    }

    printf("[app] accepted '%s'\n", dll);

    if (out_jobs) {
        IPiPluginBase* plugin = pi_host_session_get_plugin(g_session, slot);   /* borrowed */
        IMyAppJobQueue* jobs = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)plugin, &MY_APP_JOB_IID,
                                                     (void**)&jobs))) {
            *out_jobs = jobs;      /* add-ref'd: the app releases it below */
        }
    }

    /* Keep the slot open for the duration of the example; main() unloads all. */
    return 1;
}

int main(int argc, char** argv)
{
    const char* good_dll = (argc > 1) ? argv[1] : "pi_example_specialized_plugin.dll";
    const char* bad_dll  = (argc > 2) ? argv[2] : "pi_example_service.dll";

    IPiHostServices* services = NULL;
    IMyAppJobQueue*  jobs = NULL;
    int              exit_code = 0;

    printf("== piplugin example: a specialized app ==\n");

    s_app_info.name = "ExampleApp";
    pi_refcounted_init(&s_app_info.base, (const IPiUnknownVtbl*)&s_app_info_vtbl);

    /* 1) the host object: the framework's services + OUR service (通道 B).
     * The extra_qi hook is what makes "our service" reachable: the framework's
     * default host object answers its own three IIDs and forwards the rest here.
     * (create_default() is the same call with a NULL hook.) */
    if (PI_FAILED(pi_host_services_create_ex(&OnHostMessage, NULL, PI_INVALID_WINDOW,
                                             &AppExtraQi, NULL, &services))) {
        printf("FATAL: cannot create host services\n");
        return 1;
    }

    /* 2) the session, with the app's requirement declared up front */
    if (PI_FAILED(pi_host_session_create(services, &g_session))) {
        printf("FATAL: cannot create session\n");
        return 1;
    }
    pi_host_session_set_logger(g_session, &OnSessionLog, NULL);
    pi_host_session_require(g_session, &MY_APP_JOB_IID);
    printf("[app] this app requires com.example job-queue plugins\n");

    /* 3) a conforming plugin: accepted, and we can call its protocol */
    if (!LoadAndCheck(good_dll, &jobs) ) {
        exit_code = 1;
    } else if (jobs) {
        int32_t id = -1;
        uint32_t count = 0;
        my_app_submit_job(jobs, "import-photo", &id);
        my_app_submit_job(jobs, "export-video", &id);
        my_app_job_count(jobs, &count);
        printf("[app] the plugin accepted %u job(s)\n", (unsigned)count);
        if (count != 2) exit_code = 1;
        pi_iunknown_release((IPiUnknown*)jobs);      /* QI returned an add-ref'd pointer */
        jobs = NULL;
    } else {
        printf("[app] FAILED: the plugin does not expose the job protocol\n");
        exit_code = 1;
    }

    /* 4) a plugin that does NOT implement the protocol: rejected by the gate */
    if (!LoadAndCheck(bad_dll, NULL)) {
        printf("[app] as expected: the gate kept a non-conforming plugin out\n");
    } else {
        printf("[app] NOTE: '%s' got in - it should not have (does it declare the IID?)\n",
               bad_dll);
    }

    /* 5) unload everything in one call; the kit guarantees the order */
    pi_host_session_unload_all(g_session);
    g_loaded = 0;
    pi_host_session_destroy(g_session); g_session = NULL;
    pi_iunknown_release((IPiUnknown*)services);

    printf("RESULT: %s\n", exit_code ? "FAIL" : "PASS");
    return exit_code;
}
