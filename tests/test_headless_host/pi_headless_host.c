/*
 * pipluginframework - Headless Test Host (console)
 *
 * Simulates the "task server" side of the distributed application: it
 * loads the SAME plugin binary as the GUI host, but creates its host
 * services object without a window, so IPiHostUI is not exposed.
 *
 * Demonstrates:
 *   1. LV2-style capability inspection before instantiation
 *   2. A plugin running headless (its QueryInterface for IPiHostUI fails,
 *      so it creates no UI and advertises no view)
 *   3. IPiService discovery for future server-side plugins
 */
#include "pipluginframework/pi_plugin.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    printf("[plugin message] msg=0x%04X wparam=%llu\n", msg, (unsigned long long)wparam);
}

static const char* CapKind(uint32_t flags)
{
    if (flags & PI_CAP_PROVIDES) return "provides";
    if (flags & PI_CAP_REQUIRED) return "requires";
    return "optional";
}

int main(int argc, char** argv)
{
    const char* dllPath = (argc > 1) ? argv[1] : "pi_test_plugin_qt.dll";
    printf("== pipluginframework headless host ==\n");
    printf("Loading plugin: %s\n\n", dllPath);

    /* Headless host services: NO window -> IPiHostUI is not exposed. */
    IPiHostServices* host = NULL;
    if (PI_FAILED(pi_host_services_create_default(&HostMessageProc, NULL,
                                                  PI_INVALID_WINDOW, &host))) {
        printf("FATAL: cannot create host services\n");
        return 1;
    }

    /* Prove we are headless */
    void* probe = NULL;
    PiResult qhr = pi_iunknown_query_interface((IPiUnknown*)host, &PI_IID_HOST_UI, &probe);
    printf("Host exposes IPiHostUI? %s (hr=%d)\n\n",
           PI_SUCCEEDED(qhr) ? "yes" : "no", (int)qhr);
    if (PI_SUCCEEDED(qhr)) pi_iunknown_release((IPiUnknown*)probe);

    PiPluginModule* module = pi_module_load(dllPath);
    if (!module) {
        printf("FATAL: pi_module_load failed: %s\n", pi_module_get_load_error());
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }

    IPiPluginFactory* factory = NULL;
    pi_module_get_factory(module, &factory);
    const PiPluginDescriptor* desc = NULL;
    pi_factory_get_descriptor(factory, &desc);

    printf("Plugin:   %s %s\n", desc ? desc->name : "?", desc ? desc->version : "");
    printf("Category: %s\n\n", desc ? desc->category : "");

    /* Capability inspection (what a task server would filter on) */
    if (desc && desc->capability_count > 0) {
        printf("Declared capabilities:\n");
        for (uint32_t i = 0; i < desc->capability_count; ++i) {
            const PiPluginCapability* cap = &desc->capabilities[i];
            const char* what =
                pi_guid_equal(&cap->iid, &PI_IID_PLUGIN_VIEW) ? "PLUGIN_VIEW" :
                pi_guid_equal(&cap->iid, &PI_IID_HOST_UI)    ? "HOST_UI"    :
                pi_guid_equal(&cap->iid, &PI_IID_SERVICE)    ? "SERVICE"    : "custom";
            printf("  [%s] %s\n", CapKind(cap->flags), what);
        }
        printf("\n");
    }

    /* Gate: reject plugins that REQUIRE a GUI host */
    if (pi_descriptor_requires(desc, &PI_IID_HOST_UI)) {
        printf("REJECTED: plugin requires a GUI host; this host is headless.\n");
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }
    printf("Capability gate passed (no hard GUI requirement).\n\n");

    uint32_t count = pi_factory_get_class_count(factory);
    if (count == 0) {
        printf("FATAL: plugin has no classes\n");
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }

    PiGuid classGuid;
    pi_factory_get_class_guid(factory, 0, &classGuid);

    IPiPluginBase* plugin = NULL;
    PiResult hr = pi_factory_create_instance(factory, &classGuid, host, &plugin);
    if (PI_FAILED(hr)) {
        printf("FATAL: create instance failed (hr=%d)\n", (int)hr);
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }

    hr = pi_plugin_initialize(plugin, host);
    if (PI_FAILED(hr)) {
        printf("FATAL: initialize failed (hr=%d)\n", (int)hr);
        pi_iunknown_release((IPiUnknown*)plugin);
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }
    printf("Plugin initialized headlessly.\n\n");

    /* The headless story in action: no view is available, no UI is created,
     * and the process keeps running without any event loop. */
    IPiPluginView* view = NULL;
    hr = pi_plugin_get_view(plugin, &view);
    printf("pi_get_view() -> %s (hr=%d)\n",
           PI_SUCCEEDED(hr) && view ? "VIEW CREATED" : "no view (headless)", (int)hr);
    if (view) pi_iunknown_release((IPiUnknown*)view);

    /* Service capability discovery: this Qt test plugin has none, which is
     * exactly what a task server would check before loading it. */
    IPiService* service = NULL;
    hr = pi_iunknown_query_interface((IPiUnknown*)plugin, &PI_IID_SERVICE, (void**)&service);
    printf("QueryInterface(PI_IID_SERVICE) -> %s (hr=%d)\n",
           PI_SUCCEEDED(hr) ? "service found" : "no service capability", (int)hr);
    if (service) {
        pi_service_start(service, NULL, 0);
        int32_t status = -1;
        pi_service_get_status(service, &status);
        printf("  service status: %d\n", status);
        pi_service_stop(service);
        pi_iunknown_release((IPiUnknown*)service);
    }

    /* Drive a headless "main loop" for a moment to show the plugin is
     * alive and can post messages to the host. */
    printf("\nRunning headless loop (300 ms)...\n");
#ifdef _WIN32
    Sleep(300);
#else
    {
        struct timespec ts = { 0, 300 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
#endif

    printf("\nShutting down cleanly.\n");
    pi_plugin_terminate(plugin);
    pi_iunknown_release((IPiUnknown*)plugin);
    pi_iunknown_release((IPiUnknown*)factory);
    pi_module_unload(module);
    pi_iunknown_release((IPiUnknown*)host);
    printf("Done. No leaks, no GUI, no crash.\n");
    return 0;
}
