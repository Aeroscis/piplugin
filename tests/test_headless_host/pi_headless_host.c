/*
 * piplugin - Headless Test Host (console)
 *
 * Simulates the "task server" side of the distributed application: it
 * loads the SAME plugin binary as the GUI host, but creates its host
 * services object without a window, so IPiPluginHostUI is not exposed.
 *
 * Demonstrates:
 *   1. LV2-style capability inspection BEFORE instantiation
 *   2. A plugin running headless (its QueryInterface for IPiPluginHostUI fails,
 *      so it creates no UI and advertises no view)
 *   3. IPiPluginService discovery for future server-side plugins
 *   4. 通道 B / roadmap APP-01: the host exposes a service of its OWN through
 *      pi_plugin_host_services_create_ex()'s extra-QI hook, and a plugin that knows
 *      about it queries the host object and calls it (tests/common/
 *      pi_test_host_service.h). Plugins that do not know it are unaffected.
 *
 * 宿主侧机制（加载 / 双向能力门禁 / 实例化 / 七步卸载序列）全部来自宿主 kit L0
 * （piplugin_host）；本文件只剩"这一台宿主想展示什么"。
 * 这正是 inspect()/instantiate() 分解形式存在的理由：门禁与实例化的**顺序**由
 * kit 保证（实例化前门禁），而"要不要实例化"的决策仍归宿主。
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"
#include "pi_test_host_service_impl.h"
#include "pi_test_service_protocol.h"

#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>   /* nanosleep 用到的 struct timespec */
#endif

/* 宿主侧状态：插件消息计数。本函数之外没人知道这个值 —— 插件要读出它，只能
 * 走宿主自定义服务（这正是 ctest 断言"自定义服务真的被调用过"的依据）。 */
static uint32_t              g_pluginMessages = 0;
static PiPluginTestHostServiceImpl g_extraService;

/* APP-07：按消息码分类计数，用来断言服务真的在 poll() 里投递了 tick、
 * 以及 stop() 一共被调了几次（含卸载序列那一次）。 */
static uint32_t g_tickMessages      = 0;
static uint32_t g_stopMessages      = 0;
static uint32_t g_stopsBeforeUnload = 0;
static int      g_hadService        = 0;

static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    ++g_pluginMessages;
    if (msg == PI_PLUGIN_TEST_MSG_SERVICE_TICK) ++g_tickMessages;
    else if (msg == PI_PLUGIN_TEST_MSG_SERVICE_STOP) ++g_stopMessages;
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
    if (flags & PI_PLUGIN_CAP_PROVIDES) return "provides";
    if (flags & PI_PLUGIN_CAP_REQUIRED) return "requires";
    return "optional";
}

int main(int argc, char** argv)
{
    const char* dllPath = (argc > 1) ? argv[1] : "pi_plugin_test_plugin_qt.dll";
    printf("== piplugin headless host ==\n");
    printf("Loading plugin: %s\n\n", dllPath);

    /* 通道 B（APP-01）：宿主把自己的服务挂到宿主对象上。create_ex 的 extra_qi
     * 钩子只负责"框架 IID 之外的 QI"；插件侧完全不新增 API，它只是对自己
     * 拿到的那个宿主对象 QI 一次（见 tests/common/pi_test_host_service.h）。 */
    PiPluginTestHostServiceImpl_Init(&g_extraService, "headless-test-host", &g_pluginMessages);

    /* Headless host services: NO window -> IPiPluginHostUI is not exposed. */
    IPiPluginHostServices* host = NULL;
    if (PI_FAILED(pi_plugin_host_services_create_ex(&HostMessageProc, NULL,
                                             PI_INVALID_WINDOW,
                                             &PiPluginTestHostServiceImpl_ExtraQi, &g_extraService,
                                             &host))) {
        printf("FATAL: cannot create host services\n");
        return 1;
    }

    /* Prove we are headless, and prove the app-defined service is there. */
    {
        void* probe = NULL;
        PiResult qhr = pi_iunknown_query_interface((IPiUnknown*)host, &PI_PLUGIN_IID_HOST_UI, &probe);
        printf("Host exposes IPiPluginHostUI? %s (hr=%d)\n",
               PI_SUCCEEDED(qhr) ? "yes" : "no", (int)qhr);
        if (PI_SUCCEEDED(qhr)) pi_iunknown_release((IPiUnknown*)probe);

        probe = NULL;
        qhr = pi_iunknown_query_interface((IPiUnknown*)host,
                                          &PI_PLUGIN_TEST_IID_HOST_SERVICE, &probe);
        printf("Host exposes its own (app-defined) service? %s (hr=%d)\n\n",
               PI_SUCCEEDED(qhr) ? "yes" : "no", (int)qhr);
        if (PI_SUCCEEDED(qhr)) pi_iunknown_release((IPiUnknown*)probe);
    }

    /* 宿主 kit L0：会话对象负责加载、门禁与卸载序列 */
    PiPluginHostSession* session = NULL;
    if (PI_FAILED(pi_plugin_host_session_create(host, &session))) {
        printf("FATAL: cannot create host session\n");
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }
    pi_plugin_host_session_set_logger(session, &SessionLogProc, NULL);

    /* 第一步：只加载模块 + 跑能力门禁，**不**实例化。headless 宿主关心的
     * "这个插件要不要 GUI"必须在付出实例化代价之前就问清楚。 */
    uint32_t slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
    PiResult hr = pi_plugin_host_session_inspect(session, dllPath, &slot);
    if (PI_FAILED(hr)) {
        /* 能力门禁与版本门禁都属于"这台宿主按规矩拒绝了它"，不是宿主故障 */
        if (hr == PI_E_MISSINGCAPABILITY || hr == PI_E_VERSIONMISMATCH) {
            printf("REJECTED: %s\n", pi_plugin_host_session_last_error(session));
        } else {
            printf("FATAL: pi_plugin_host_session_inspect failed (hr=%d): %s\n",
                   (int)hr, pi_plugin_host_session_last_error(session));
        }
        pi_plugin_host_session_destroy(session);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }

    const PiPluginDescriptor* desc = pi_plugin_host_session_get_descriptor(session, slot);
    printf("Plugin:   %s %s\n", desc ? desc->name : "?", desc ? desc->version : "");
    printf("Category: %s\n\n", desc ? desc->category : "");

    /* Capability inspection (what a task server would filter on) */
    if (desc && desc->capability_count > 0) {
        printf("Declared capabilities:\n");
        for (uint32_t i = 0; i < desc->capability_count; ++i) {
            const PiPluginCapability* cap = &desc->capabilities[i];
            const char* what =
                pi_guid_equal(&cap->iid, &PI_PLUGIN_IID_PLUGIN_VIEW) ? "PLUGIN_VIEW" :
                pi_guid_equal(&cap->iid, &PI_PLUGIN_IID_HOST_UI)    ? "HOST_UI"    :
                pi_guid_equal(&cap->iid, &PI_PLUGIN_IID_SERVICE)    ? "SERVICE"    : "custom";
            printf("  [%s] %s\n", CapKind(cap->flags), what);
        }
        printf("\n");
    }

    /* 门禁已经在 inspect() 里跑完了（且是在 create_instance 之前跑的）：
     * 插件 REQUIRED 而宿主给不出的能力、以及本宿主生态 REQUIRE 而插件 PROVIDES
     * 不出的能力，都会让 inspect() 直接失败。能走到这里就说明双向门禁都过了。 */
    printf("Capability gate passed (no hard GUI requirement).\n\n");

    /* ---- 自由元数据（roadmap APP-04）------------------------------------
     * 描述性事实（用什么 UI 工具包、支持什么格式……）不适合塞进 capabilities，
     * 现在走 descriptor 的键值对：既可以直接列出来展示，也可以按键取值。 */
    if (desc && desc->property_count > 0) {
        printf("Declared properties:\n");
        for (uint32_t i = 0; i < desc->property_count; ++i) {
            const PiPluginProperty* prop = &desc->properties[i];
            printf("  %s = %s\n",
                   prop->key   ? prop->key   : "(null)",
                   prop->value ? prop->value : "(null)");
        }
        printf("\n");
    }
    {
        /* 按键取值：宿主真正会拿去分支的那类信息。单独打一行，ctest 用例
         * descriptor_properties_* 就断言这一行（值随插件不同而不同）。
         * 取不到不算宿主故障 —— 第三方插件可以没有这条属性，故打印 (absent)。 */
        const char* kind = pi_plugin_descriptor_find_property(desc, "com.example.kind");
        printf("[host] property com.example.kind = %s\n\n", kind ? kind : "(absent)");
    }

    /* 第二步：门禁通过后才实例化 + 初始化 */
    hr = pi_plugin_host_session_instantiate(session, slot);
    if (PI_FAILED(hr)) {
        printf("FATAL: instantiate failed (hr=%d): %s\n",
               (int)hr, pi_plugin_host_session_last_error(session));
        pi_plugin_host_session_destroy(session);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }
    printf("Plugin initialized headlessly.\n\n");

    /* The headless story in action: no view is available, no UI is created,
     * and the process keeps running without any event loop. */
    IPiPluginView* view = pi_plugin_host_session_get_view(session, slot);   /* borrowed */
    printf("pi_plugin_get_view() -> %s (hr=%d)\n",
           view ? "VIEW CREATED" : "no view (headless)",
           view ? (int)PI_OK : (int)PI_E_NOINTERFACE);

    /* Service capability discovery: a GUI plugin has none, which is exactly
     * what a task server would check before loading it. */
    IPiPluginService* service = pi_plugin_host_session_get_service(session, slot);   /* borrowed */
    printf("QueryInterface(PI_PLUGIN_IID_SERVICE) -> %s (hr=%d)\n",
           service ? "service found" : "no service capability",
           service ? (int)PI_OK : (int)PI_E_NOINTERFACE);

    /* ---- IPiPluginService 全生命周期断言（roadmap APP-07）----------------------
     *
     * 这是"headless + service"那一格的自动化验收：宿主像真正的服务器主循环
     * 那样驱动插件，并把每一步的返回值与可观测副作用（插件投递的消息）都断言
     * 一遍。任何一条不符 -> 退出码 1，ctest 用例 headless_host_service_lifecycle
     * 只按退出码判定。
     *
     * 不 release 服务指针：它是借用指针，所有权在 session，卸载序列里还会再
     * stop 一次（stop 幂等），正好也被下面的计数覆盖。 */
    if (service) {
        static const PiPluginServiceOption opts[2] = {
            { PI_PLUGIN_TEST_SERVICE_OPTION_SLOT,     "alpha" },
            { PI_PLUGIN_TEST_SERVICE_OPTION_INTERVAL, "1"     }
        };
        int32_t  status = -1;
        PiResult shr;
        uint32_t ticks0, stops0;
        int      ok = 1;
        unsigned step = 0;

        g_hadService = 1;
        printf("\nIPiService lifecycle (poll-driven, no event loop):\n");

        /* 1) 从未 start 就 stop：契约说幂等，必须成功 */
        shr = pi_plugin_service_stop(service);
        ++step;
        printf("  %u. stop before start          -> %d (expect %d)\n",
               step, (int)shr, (int)PI_OK);
        ok = ok && (shr == PI_OK);

        /* 2) 缺少必填选项就 start：文档承诺 PI_E_MISSINGCAPABILITY */
        shr = pi_plugin_service_start(service, NULL, 0);
        ++step;
        printf("  %u. start without 'slot'       -> %d (expect %d)\n",
               step, (int)shr, (int)PI_E_MISSINGCAPABILITY);
        ok = ok && (shr == PI_E_MISSINGCAPABILITY);

        /* 3) 正常 start，状态必须变成 RUNNING */
        shr = pi_plugin_service_start(service, opts, 2);
        ++step;
        status = -1;
        pi_plugin_service_get_status(service, &status);
        printf("  %u. start(slot=alpha)          -> %d, status=%d (expect %d/%d)\n",
               step, (int)shr, (int)status, (int)PI_OK, (int)PI_PLUGIN_SERVICE_RUNNING);
        ok = ok && (shr == PI_OK) && (status == PI_PLUGIN_SERVICE_RUNNING);

        /* 4) poll：服务器主循环驱动它干活。插件每 poll 报一次 tick，
         *    宿主收到的消息数就是"真的在跑"的证据（不是只看返回值）。 */
        ticks0 = g_tickMessages;
        for (int i = 0; i < 5; ++i) {
            shr = pi_plugin_service_poll(service);
            if (shr != PI_OK) ok = 0;
        }
        ++step;
        status = -1;
        pi_plugin_service_get_status(service, &status);
        printf("  %u. poll x5                     -> ticks=%u (expect 5), status=%d\n",
               step, (unsigned)(g_tickMessages - ticks0), (int)status);
        ok = ok && (g_tickMessages - ticks0 == 5) && (status == PI_PLUGIN_SERVICE_RUNNING);

        /* 5) stop 幂等：连调两次都要成功，且状态变成 STOPPED */
        stops0 = g_stopMessages;
        shr = pi_plugin_service_stop(service);
        ok = ok && (shr == PI_OK);
        shr = pi_plugin_service_stop(service);
        ++step;
        status = -1;
        pi_plugin_service_get_status(service, &status);
        printf("  %u. stop x2 (idempotent)        -> stops=%u (expect 2), status=%d (expect %d)\n",
               step, (unsigned)(g_stopMessages - stops0), (int)status, (int)PI_PLUGIN_SERVICE_STOPPED);
        ok = ok && (shr == PI_OK) && (g_stopMessages - stops0 == 2) &&
             (status == PI_PLUGIN_SERVICE_STOPPED);

        /* 6) 停了以后再 poll：插件必须明确报错，而不是假装还在工作 */
        shr = pi_plugin_service_poll(service);
        ++step;
        printf("  %u. poll after stop             -> %d (expect %d)\n",
               step, (int)shr, (int)PI_FAIL);
        ok = ok && (shr == PI_FAIL);

        /* 7) get_status 的 NULL 出参必须被拒绝而不是崩溃 */
        shr = pi_plugin_service_get_status(service, NULL);
        ++step;
        printf("  %u. get_status(NULL)            -> %d (expect %d)\n",
               step, (int)shr, (int)PI_E_INVALIDARG);
        ok = ok && (shr == PI_E_INVALIDARG);

        if (!ok) {
            printf("[host] service lifecycle: FAILED\n");
            pi_plugin_host_session_unload(session, slot);
            pi_plugin_host_session_destroy(session);
            pi_iunknown_release((IPiUnknown*)host);
            return 1;
        }

        /* 这个计数等下用来断言"卸载序列确实又 stop 了一次" */
        g_stopsBeforeUnload = g_stopMessages;
        printf("[host] service lifecycle: OK (polls=5, ticks=5, stops=%u)\n",
               (unsigned)g_stopMessages);
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
    pi_plugin_host_session_unload(session, slot);

    /* APP-07 的最后一条断言：卸载序列自己也要 stop 一次服务（插件 stop 幂等，
     * 所以这是一次可观测的额外调用，且模块此刻仍然 mapped）。 */
    if (g_hadService && g_stopMessages != g_stopsBeforeUnload + 1) {
        printf("[host] FAILED: the unload sequence did not stop the service "
               "(stops before=%u after=%u)\n",
               (unsigned)g_stopsBeforeUnload, (unsigned)g_stopMessages);
        pi_plugin_host_session_destroy(session);
        pi_iunknown_release((IPiUnknown*)host);
        return 1;
    }
    if (g_hadService) {
        printf("[host] unload sequence stopped the service (stop #%u)\n",
               (unsigned)g_stopMessages);
    }

    pi_plugin_host_session_destroy(session);        /* 释放 session 持有的宿主服务引用 */
    pi_iunknown_release((IPiUnknown*)host);  /* 释放宿主自己那一份引用 */

    /* APP-01 的证据行：name() 只可能被插件经宿主自定义服务调到 —— 本宿主
     * 自己的探测只做 QI（不调方法），所以这个计数非 0 就等于"插件 QI 到了
     * 宿主自定义服务并成功调用了它"。tests/test_headless_host/CMakeLists.txt
     * 里的 ctest 用例就断言这一行（格式刻意不含括号，便于按正则断言）。 */
    printf("[host] app-defined host service: %u call(s) reached it from plugins\n",
           g_extraService.name_calls);

    printf("Done. No leaks, no GUI, no crash.\n");
    return 0;
}
