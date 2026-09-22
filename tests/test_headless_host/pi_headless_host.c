/*
 * pipluginframework - Headless Test Host (console)
 *
 * Simulates the "task server" side of the distributed application: it
 * loads the SAME plugin binary as the GUI host, but creates its host
 * services object without a window, so IPiHostUI is not exposed.
 *
 * Demonstrates:
 *   1. LV2-style capability inspection BEFORE instantiation
 *   2. A plugin running headless (its QueryInterface for IPiHostUI fails,
 *      so it creates no UI and advertises no view)
 *   3. IPiService discovery for future server-side plugins
 *
 * 宿主侧机制（加载 / 双向能力门禁 / 实例化 / 七步卸载序列）全部来自宿主 kit L0
 * （pipluginframework_host）；本文件只剩"这一台宿主想展示什么"。
 * 这正是 inspect()/instantiate() 分解形式存在的理由：门禁与实例化的**顺序**由
 * kit 保证（实例化前门禁），而"要不要实例化"的决策仍归宿主。
 */
#include "pipluginframework/pi_plugin.h"
#include "pi_host_session.h"

#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>   /* nanosleep 用到的 struct timespec */
#endif

static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    printf("[plugin message] msg=0x%04X wparam=%llu\n", msg, (unsigned long long)wparam);
}

/* kit 的步骤日志 -> stdout（去向由宿主决定，故 kit 只提供回调） */
static void SessionLogProc(void* user_data, const char* message)
{
    (void)user_data;
    printf("[session] %s\n", message);
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

    /* 宿主 kit L0：会话对象负责加载、门禁与卸载序列 */
    PiPluginHostSession* session = NULL;
    if (PI_FAILED(pi_host_session_create(host, &session))) {
        printf("FATAL: cannot create host session\n");
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }
    pi_host_session_set_logger(session, &SessionLogProc, NULL);

    /* 第一步：只加载模块 + 跑能力门禁，**不**实例化。headless 宿主关心的
     * "这个插件要不要 GUI"必须在付出实例化代价之前就问清楚。 */
    uint32_t slot = PI_HOST_SESSION_INVALID_SLOT;
    PiResult hr = pi_host_session_inspect(session, dllPath, &slot);
    if (PI_FAILED(hr)) {
        if (hr == PI_E_MISSINGCAPABILITY) {
            printf("REJECTED: %s\n", pi_host_session_last_error(session));
        } else {
            printf("FATAL: pi_host_session_inspect failed (hr=%d): %s\n",
                   (int)hr, pi_host_session_last_error(session));
        }
        pi_host_session_destroy(session);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }

    const PiPluginDescriptor* desc = pi_host_session_get_descriptor(session, slot);
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

    /* 门禁已经在 inspect() 里跑完了（且是在 create_instance 之前跑的）：
     * 插件 REQUIRED 而宿主给不出的能力、以及本宿主生态 REQUIRE 而插件 PROVIDES
     * 不出的能力，都会让 inspect() 直接失败。能走到这里就说明双向门禁都过了。 */
    printf("Capability gate passed (no hard GUI requirement).\n\n");

    /* 第二步：门禁通过后才实例化 + 初始化 */
    hr = pi_host_session_instantiate(session, slot);
    if (PI_FAILED(hr)) {
        printf("FATAL: instantiate failed (hr=%d): %s\n",
               (int)hr, pi_host_session_last_error(session));
        pi_host_session_destroy(session);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }
    printf("Plugin initialized headlessly.\n\n");

    /* The headless story in action: no view is available, no UI is created,
     * and the process keeps running without any event loop. */
    IPiPluginView* view = pi_host_session_get_view(session, slot);   /* borrowed */
    printf("pi_get_view() -> %s (hr=%d)\n",
           view ? "VIEW CREATED" : "no view (headless)",
           view ? (int)PI_OK : (int)PI_E_NOINTERFACE);

    /* Service capability discovery: this Qt test plugin has none, which is
     * exactly what a task server would check before loading it. */
    IPiService* service = pi_host_session_get_service(session, slot);   /* borrowed */
    printf("QueryInterface(PI_IID_SERVICE) -> %s (hr=%d)\n",
           service ? "service found" : "no service capability",
           service ? (int)PI_OK : (int)PI_E_NOINTERFACE);
    if (service) {
        pi_service_start(service, NULL, 0);
        int32_t status = -1;
        pi_service_get_status(service, &status);
        printf("  service status: %d\n", status);
        pi_service_stop(service);
        /* 不 release：借用指针，所有权在 session。卸载序列会再 stop 一次，stop 幂等。 */
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
    /* 七步卸载序列由 kit 内化：service stop/release -> detach/release view ->
     * terminate/release plugin -> release factory -> unload module -> 清槽位 */
    pi_host_session_unload(session, slot);
    pi_host_session_destroy(session);        /* 释放 session 持有的宿主服务引用 */
    pi_iunknown_release((IPiUnknown*)host);  /* 释放宿主自己那一份引用 */
    printf("Done. No leaks, no GUI, no crash.\n");
    return 0;
}
