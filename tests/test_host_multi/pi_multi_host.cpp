/*
 * piplugin - multi-plugin / multi-thread test host (roadmap APP-08 + W-02/W-04/W-05)
 *
 * Three modes in one small plain-Win32 host (no D3D, no resize loop - the
 * delicate rendering machinery lives in tests/test_host and must not be
 * perturbed by this file):
 *
 *   (default) [pluginA.dll [pluginB.dll]]
 *       APP-08 acceptance: ONE process loads TWO different Qt plugin modules at
 *       the same time, gives each its own container window, drives both of
 *       their event loops, and unloads both.
 *
 *   --post-thread <plugin.dll>
 *       W-04: a plugin SUBTHREAD calls pi_host_post_message() (the host must
 *       get it and marshal it to its own loop) and the Qt kit's
 *       pi_qt_view_post() (the kit must run the callback on the host GUI
 *       thread).
 *
 *   --container-switch <plugin.dll>
 *       W-02: attach into container A, switch the live view to container B and
 *       back, resize round trips, then unload.
 *
 * Why two different Qt plugin DLLs for APP-08 and not one DLL twice: a module's
 * static data is per-module, so loading the same file twice shares the adapter's
 * state even when the kit is a static library - that setup cannot see the bug.
 * Two distinct modules each used to get their own copy of `QApplication*`, so
 * the second `pi_attach()` tried to build a second QApplication in one process.
 * With the kit built as a DLL there is one copy of that state, and the second
 * plugin reuses the QApplication the first one created.
 *
 * What every mode does with failures: print to stdout and to pi_multi_host.log,
 * then exit 1. ctest judges only the exit code.
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"
#include "pi_test_thread.h"    /* W-04：宿主侧的 marshal 队列要一把锁 */

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define PI_MULTI_PLUGIN_COUNT 2
#define PI_MULTI_FRAMES       120     /* ~1.2 s at ~10 ms per frame */

/* 两个变体的心跳消息码（见 tests/test_plugin/pi_qt_test_plugin.cpp：
 * 变体 A = 0x2000，变体 B = 0x2001）。宿主据此分别确认两个插件都在跑。 */
#define PI_QT_HEARTBEAT_A 0x2000u
#define PI_QT_HEARTBEAT_B 0x2001u

/* imgui 变体的心跳码（见 tests/test_plugin_imgui/pi_imgui_test_plugin.cpp：
 * 变体 A = 0x2002，变体 B = 0x2003）。wparam = 该插件的帧计数。 */
#define PI_IMGUI_HEARTBEAT_A 0x2002u
#define PI_IMGUI_HEARTBEAT_B 0x2003u

/* W-04 探针模式的两个消息码（同一份插件源码） */
#define PI_QT_POST_REPORT_MSG  0x2010u   /* wparam: 1 = 回调跑在宿主 GUI 线程 */
#define PI_QT_WORKER_ALIVE_MSG 0x2011u   /* wparam: 子线程发出的第几条 */

static IPiHostServices*     g_services = NULL;
static PiPluginHostSession* g_session  = NULL;
static HWND                 g_window   = NULL;
static HWND                 g_containers[PI_MULTI_PLUGIN_COUNT];
static uint32_t             g_slots[PI_MULTI_PLUGIN_COUNT];
static uint32_t             g_heartbeats[PI_MULTI_PLUGIN_COUNT];
static uint32_t             g_imgui_frames[PI_MULTI_PLUGIN_COUNT];    /* 收到的 imgui 心跳条数 */
static uintptr_t            g_imgui_last_tick[PI_MULTI_PLUGIN_COUNT]; /* 心跳里的帧计数 */
static const char*          g_paths[PI_MULTI_PLUGIN_COUNT];
static int                  g_failures = 0;

/* ---- W-04 探针模式的宿主侧账本 ------------------------------------------
 * 宿主消息回调**可能在任意线程上**被调到（这就是 pi_host_post_message 的
 * 契约），所以这里按契约做一遍宿主该做的事：记下"是哪条线程调来的"，然后把
 * 消息搬进队列，由主循环在自己的线程上投递。 */
#define PI_MARSHAL_QUEUE_MAX 4096

static int           g_probe_mode          = 0;   /* 0 = APP-08 流程 */
static unsigned long g_main_thread_id      = 0;
static int           g_alive_msgs          = 0;   /* 收到的 0x2011 条数 */
static int           g_alive_from_worker   = 0;   /* 其中来自非主线程的条数 */
static int           g_report_msgs         = 0;   /* 收到的 0x2010 条数 */
static int           g_report_on_ui_thread = 0;   /* 其中 wparam==1（marshal 成功） */
static int           g_report_from_main    = 0;   /* report 本身也在主线程到达 */
static PiTestMutex   g_marshal_mutex;
static uintptr_t     g_marshal_queue[PI_MARSHAL_QUEUE_MAX];
static int           g_marshal_queued      = 0;
static int           g_marshal_delivered   = 0;
static int           g_marshal_wrong_thread = 0;
static int           g_marshal_overflow    = 0;

/* --------------------------------------------------------------------------
 * Diagnostics: stdout + pi_multi_host.log next to the exe, like the other hosts
 * -------------------------------------------------------------------------- */
static FILE* g_log = NULL;

static void LogStatus(const char* fmt, ...)
{
    va_list ap;
    char line[512];

    if (!g_log) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
        char* slash = (len > 0 && len < MAX_PATH) ? strrchr(path, '\\') : NULL;
        if (slash) {
            *(slash + 1) = 0;
            strncat_s(path, sizeof(path), "pi_multi_host.log", _TRUNCATE);
            fopen_s(&g_log, path, "a");
        }
    }

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("%s\n", line);
    if (g_log) { fprintf(g_log, "%s\n", line); fflush(g_log); }
}

static void Fail(const char* fmt, ...)
{
    va_list ap;
    char detail[400];
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    ++g_failures;
    LogStatus("FAIL: %s", detail);
}

static void SessionLogProc(void* user_data, const char* message)
{
    (void)user_data;
    LogStatus("  [session] %s", message);
}

/* 心跳计数：0x2000 -> 变体 A，0x2001 -> 变体 B，其它消息只记录。
 * W-04 探针模式下额外记账（见上面的账本说明）。 */
static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    if (msg == PI_QT_HEARTBEAT_A) ++g_heartbeats[0];
    else if (msg == PI_QT_HEARTBEAT_B) ++g_heartbeats[1];
    else if (msg == PI_IMGUI_HEARTBEAT_A) {
        ++g_imgui_frames[0];
        g_imgui_last_tick[0] = wparam;
    } else if (msg == PI_IMGUI_HEARTBEAT_B) {
        ++g_imgui_frames[1];
        g_imgui_last_tick[1] = wparam;
    }

    if (g_probe_mode) {
        const int on_main = ((unsigned long)GetCurrentThreadId() == g_main_thread_id);
        if (msg == PI_QT_WORKER_ALIVE_MSG) {
            ++g_alive_msgs;
            if (!on_main) ++g_alive_from_worker;
            /* 宿主自己的 marshal：排进队列，主循环在自己的线程上投递 */
            PiTestMutexLock(&g_marshal_mutex);
            if (g_marshal_queued < PI_MARSHAL_QUEUE_MAX)
                g_marshal_queue[g_marshal_queued++] = wparam;
            else
                ++g_marshal_overflow;
            PiTestMutexUnlock(&g_marshal_mutex);
        } else if (msg == PI_QT_POST_REPORT_MSG) {
            ++g_report_msgs;
            if (wparam == 1u) ++g_report_on_ui_thread;
            if (on_main)      ++g_report_from_main;
        }
    }

    LogStatus("  [plugin message] msg=0x%04X wparam=%llu",
              msg, (unsigned long long)wparam);
}

/* 主线程上的"事件循环投递"：把队列搬空，逐条确认是在主线程上投递的。 */
static void DrainMarshalQueueOnMainThread(void)
{
    PiTestMutexLock(&g_marshal_mutex);
    while (g_marshal_delivered < g_marshal_queued) {
        if ((unsigned long)GetCurrentThreadId() != g_main_thread_id)
            ++g_marshal_wrong_thread;
        ++g_marshal_delivered;
    }
    PiTestMutexUnlock(&g_marshal_mutex);
}

/* --------------------------------------------------------------------------
 * Layout: the host decides where the containers are (kit discipline: the
 * kit never creates or positions a window).
 * -------------------------------------------------------------------------- */
static void LayoutContainers(void)
{
    RECT rc;
    int w, h;
    if (!g_window || !GetClientRect(g_window, &rc)) return;
    w = rc.right / PI_MULTI_PLUGIN_COUNT;
    h = rc.bottom;
    for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
        if (g_containers[i])
            SetWindowPos(g_containers[i], NULL, i * w, 0, w, h, SWP_NOZORDER);
    }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            /* 容器随宿主窗口变化，再把尺寸分别转发给两个插件的 view */
            RECT rc;
            if (GetClientRect(hWnd, &rc)) {
                int w = rc.right / PI_MULTI_PLUGIN_COUNT, h = rc.bottom;
                for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
                    if (!g_containers[i]) continue;
                    SetWindowPos(g_containers[i], NULL, i * w, 0, w, h, SWP_NOZORDER);
                    IPiPluginView* view = pi_host_session_get_view(g_session, g_slots[i]);
                    if (view && w > 0 && h > 0) pi_view_on_resize(view, w, h);
                }
            }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/* --------------------------------------------------------------------------
 * Shared host shell: one window + N containers, host services, L0 session.
 * `ui_container_index` is the container the host services report as the UI
 * window (how a plugin that creates its own window finds its parent); -1 for a
 * headless-shaped host services object. It is an INDEX and not a handle on
 * purpose: the handle does not exist yet when the caller writes the call.
 * Returns 0 on success. Every mode builds on this so a low-effort second host
 * stays low effort (that is what the L0 session kit is for).
 * -------------------------------------------------------------------------- */
static int CreateHostShell(int container_count, int ui_container_index)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PiPluginMultiHost";
    ::RegisterClassExW(&wc);

    g_window = ::CreateWindowW(wc.lpszClassName, L"piplugin - multi-plugin test host",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               80, 80, 1100, 620,
                               NULL, NULL, wc.hInstance, NULL);
    if (!g_window) {
        LogStatus("FATAL: cannot create the host window");
        return 1;
    }

    for (int i = 0; i < container_count; ++i) {
        g_containers[i] = ::CreateWindowExW(0, L"STATIC", NULL,
                                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                            0, 0, 10, 10, g_window, NULL, wc.hInstance, NULL);
        if (!g_containers[i]) {
            LogStatus("FATAL: cannot create container %d", i);
            return 1;
        }
    }
    LayoutContainers();
    ::ShowWindow(g_window, SW_SHOW);
    ::UpdateWindow(g_window);

    if (PI_FAILED(pi_host_services_create_default(
            &HostMessageProc, NULL,
            (ui_container_index >= 0) ? (PiNativeWindow)g_containers[ui_container_index]
                                      : PI_INVALID_WINDOW,
            &g_services))) {
        LogStatus("FATAL: cannot create host services");
        return 1;
    }
    if (PI_FAILED(pi_host_session_create(g_services, &g_session))) {
        LogStatus("FATAL: cannot create the host session");
        return 1;
    }
    pi_host_session_set_logger(g_session, &SessionLogProc, NULL);
    return 0;
}

/* 泵一轮消息 + 驱动所有插件的 idle；返回 0 = 收到 WM_QUIT */
static int PumpOnce(void)
{
    MSG msg;
    while (::PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return 0;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    pi_host_session_drive_idle(g_session);
    return 1;
}

static void TearDownHostShell(void)
{
    if (g_session) { pi_host_session_destroy(g_session); g_session = NULL; }
    if (g_services) { pi_iunknown_release((IPiUnknown*)g_services); g_services = NULL; }
    if (g_window) { ::DestroyWindow(g_window); g_window = NULL; }
}

/* --------------------------------------------------------------------------
 * Per-plugin checks (APP-08 mode)
 * -------------------------------------------------------------------------- */
static void CheckPluginUi(unsigned index)
{
    IPiPluginView* view = pi_host_session_get_view(g_session, g_slots[index]);
    const PiPluginDescriptor* desc = pi_host_session_get_descriptor(g_session, g_slots[index]);
    HWND plugin_window;
    HWND parent;
    RECT r;

    LogStatus("plugin[%u]: %s %s (category=%s)",
              index,
              desc && desc->name ? desc->name : "?",
              desc && desc->version ? desc->version : "",
              desc && desc->category ? desc->category : "-");

    if (!view) {
        Fail("plugin[%u] published no view", index);
        return;
    }

    plugin_window = (HWND)(uintptr_t)pi_view_get_native_window(view);
    if (!plugin_window || !IsWindow(plugin_window)) {
        Fail("plugin[%u] view has no live native window", index);
        return;
    }

    /* 每个插件必须嵌进**它自己**的容器：这正是"各自有 UI"的可断言形式 */
    parent = GetParent(plugin_window);
    if (parent != g_containers[index]) {
        Fail("plugin[%u] window 0x%p is not a child of its own container (0x%p, got 0x%p)",
             index, (void*)plugin_window, (void*)g_containers[index], (void*)parent);
        return;
    }
    if (!IsWindowVisible(plugin_window)) {
        Fail("plugin[%u] window is not visible", index);
        return;
    }
    if (!GetWindowRect(plugin_window, &r) || r.right - r.left <= 0 || r.bottom - r.top <= 0) {
        Fail("plugin[%u] window has an empty rectangle", index);
        return;
    }

    LogStatus("plugin[%u]: window=0x%p container=0x%p size=%dx%d - embedded and visible",
              index, (void*)plugin_window, (void*)parent, (int)(r.right - r.left), (int)(r.bottom - r.top));
}

/* --------------------------------------------------------------------------
 * Mode: APP-08 - two Qt plugin DLLs in one process
 * -------------------------------------------------------------------------- */
static int RunMultiPluginMode(const char* path_a, const char* path_b)
{
    int frame;

    g_paths[0] = path_a;
    g_paths[1] = path_b;
    g_slots[0] = PI_HOST_SESSION_INVALID_SLOT;
    g_slots[1] = PI_HOST_SESSION_INVALID_SLOT;

    LogStatus("== piplugin multi-plugin test host (APP-08) ==");
    LogStatus("plugin[0] = %s", g_paths[0]);
    LogStatus("plugin[1] = %s", g_paths[1]);

    if (CreateHostShell(PI_MULTI_PLUGIN_COUNT, 0) != 0) return 1;

    /* load BOTH modules at the same time - the point of this host. They go
     * into two slots of the same session; the kit is multi-slot by design. */
    for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
        PiResult hr = pi_host_session_load(g_session, g_paths[i], &g_slots[i]);
        if (PI_FAILED(hr)) {
            Fail("plugin[%d] load failed (hr=%d): %s", i, (int)hr,
                 pi_host_session_last_error(g_session));
            g_slots[i] = PI_HOST_SESSION_INVALID_SLOT;
            continue;
        }
        /* 每个插件嵌进它自己的容器：容器由宿主创建并摆位，kit 只接收它 */
        hr = pi_host_session_attach_view(g_session, g_slots[i],
                                         (PiNativeWindow)g_containers[i], /*set_visible=*/1);
        if (PI_FAILED(hr)) {
            Fail("plugin[%d] attach failed (hr=%d): %s", i, (int)hr,
                 pi_host_session_last_error(g_session));
            continue;
        }
        CheckPluginUi((unsigned)i);
    }

    /* 报告 Qt 套件在本进程里的形态：SHARED 时它是一个独立模块（所有插件共用），
     * STATIC 时不存在这个模块（每个插件各带一份）。 */
    {
        HMODULE kit = ::GetModuleHandleA("piplugin_qtd.dll");
        if (!kit) kit = ::GetModuleHandleA("piplugin_qt.dll");
        LogStatus("qt adapter kit: %s", kit ? "loaded as a shared module (one copy for the process)"
                                            : "not a separate module (each plugin has its own copy)");
    }

    /* 4) drive both plugins from this host's loop for a while */
    LogStatus("driving %d frames (both plugins pumped by one session)...", PI_MULTI_FRAMES);
    for (frame = 0; frame < PI_MULTI_FRAMES; ++frame) {
        if (!PumpOnce()) break;
        if (g_slots[0] != PI_HOST_SESSION_INVALID_SLOT &&
            g_slots[1] != PI_HOST_SESSION_INVALID_SLOT &&
            g_heartbeats[0] > 0 && g_heartbeats[1] > 0)
            break;                               /* 两个都报过心跳就可以收工 */
        ::Sleep(10);
    }

    /* 5) both Qt widget trees must have been alive, not merely constructed */
    for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
        if (g_slots[i] == PI_HOST_SESSION_INVALID_SLOT) continue;
        if (g_heartbeats[i] == 0)
            Fail("plugin[%d] never posted a heartbeat: its UI was not pumped", i);
        else
            LogStatus("plugin[%d]: %u heartbeat(s) - its Qt timer is running",
                      i, (unsigned)g_heartbeats[i]);
    }

    /* 6) unload both (reverse order), then tear the host down */
    for (int i = PI_MULTI_PLUGIN_COUNT - 1; i >= 0; --i) {
        if (g_slots[i] == PI_HOST_SESSION_INVALID_SLOT) continue;
        if (PI_FAILED(pi_host_session_unload(g_session, g_slots[i])))
            Fail("plugin[%d] failed to unload", i);
        g_slots[i] = PI_HOST_SESSION_INVALID_SLOT;
    }

    TearDownHostShell();

    LogStatus("[host] multi-plugin result: failures=%d heartbeats=%u/%u",
              g_failures, (unsigned)g_heartbeats[0], (unsigned)g_heartbeats[1]);
    LogStatus("RESULT: %s", g_failures ? "FAIL" : "PASS");

    if (g_failures) return 1;
    return 0;
}

/* --------------------------------------------------------------------------
 * Mode: W-04 - a plugin SUBTHREAD talks to the host and to the Qt kit
 *
 * Two contracts are on trial here, and both are about threads:
 *   1. `pi_host_post_message` is callable from ANY thread and the host decides
 *      how to marshal it. The plugin's worker thread posts 0x2011 directly, so
 *      the host's callback really is entered on that worker thread; the host
 *      then queues it (its own marshal) and delivers it on its own loop.
 *   2. `pi_qt_view_post` runs the callback on the host GUI thread. The worker
 *      calls it and the callback reports, by message, whether it found itself
 *      on the GUI thread - which is the whole question.
 *
 * The plugin side is gated behind the environment variable
 * PI_QT_TEST_POST_THREAD=1 (set by this ctest case only), so every other user
 * of the same test plugin keeps its normal behaviour.
 * -------------------------------------------------------------------------- */
#define PI_PROBE_TIMEOUT_MS 15000

static int RunPostThreadMode(const char* dll_path)
{
    IPiPluginView* view = NULL;
    DWORD start_ms;
    int ok = 1;

    g_probe_mode     = 1;
    g_main_thread_id = (unsigned long)GetCurrentThreadId();
    PiTestMutexInit(&g_marshal_mutex);

    LogStatus("== piplugin worker-thread test host (W-04) ==");
    LogStatus("plugin = %s", dll_path);
    LogStatus("host main thread id = %lu", g_main_thread_id);

    g_slots[0] = PI_HOST_SESSION_INVALID_SLOT;
    if (CreateHostShell(1, 0) != 0) return 1;

    if (PI_FAILED(pi_host_session_load(g_session, dll_path, &g_slots[0]))) {
        Fail("load failed: %s", pi_host_session_last_error(g_session));
        TearDownHostShell();
        return 1;
    }
    if (PI_FAILED(pi_host_session_attach_view(g_session, g_slots[0],
                                              (PiNativeWindow)g_containers[0], 1))) {
        Fail("attach failed: %s", pi_host_session_last_error(g_session));
        TearDownHostShell();
        return 1;
    }
    view = pi_host_session_get_view(g_session, g_slots[0]);
    if (!view) {
        Fail("plugin published no view");
        TearDownHostShell();
        return 1;
    }

    /* Drive the host loop until the plugin's worker has reported, or time out.
     * DrainMarshalQueueOnMainThread() is the "host marshals to its own thread"
     * half of contract 1. */
    start_ms = GetTickCount();
    while (g_report_msgs == 0 && (GetTickCount() - start_ms) < PI_PROBE_TIMEOUT_MS) {
        if (!PumpOnce()) break;
        DrainMarshalQueueOnMainThread();
        ::Sleep(10);
    }

    /* 给子线程一点收尾时间（回调报过之后它就会退出循环），然后把队列搬空 */
    for (int i = 0; i < 30; ++i) {
        if (!PumpOnce()) break;
        DrainMarshalQueueOnMainThread();
        ::Sleep(10);
    }

    /* --- 契约 1：子线程的 post_message 到达宿主，宿主自己搬到主线程 --- */
    LogStatus("[host] worker post_message: received=%d (from a worker thread=%d), "
              "marshalled on the main thread=%d/%d",
              g_alive_msgs, g_alive_from_worker, g_marshal_delivered, g_marshal_queued);
    if (g_alive_msgs == 0) {
        Fail("the plugin's worker thread never reached the host message proc");
        ok = 0;
    } else {
        if (g_alive_from_worker == 0) {
            Fail("every post_message arrived on the host main thread - the plugin's "
                 "worker thread was not really posting (test premise broken)");
            ok = 0;
        }
        if (g_marshal_overflow != 0) {
            Fail("the host's marshal queue overflowed");
            ok = 0;
        }
        if (g_marshal_delivered != g_marshal_queued || g_marshal_wrong_thread != 0) {
            Fail("host marshal lost or misdelivered messages (delivered=%d queued=%d wrong-thread=%d)",
                 g_marshal_delivered, g_marshal_queued, g_marshal_wrong_thread);
            ok = 0;
        }
    }

    /* --- 契约 2：pi_qt_view_post 的回调跑在宿主 GUI 线程上 --- */
    LogStatus("[host] pi_qt_view_post: reports=%d, ran on the host GUI thread=%d, "
              "report itself arrived on the main thread=%d",
              g_report_msgs, g_report_on_ui_thread, g_report_from_main);
    if (g_report_msgs == 0) {
        Fail("pi_qt_view_post never ran the callback within %d ms "
             "(queued call was dropped instead of marshalled)", PI_PROBE_TIMEOUT_MS);
        ok = 0;
    } else if (g_report_on_ui_thread == 0) {
        Fail("pi_qt_view_post ran the callback on the CALLING thread (%d report(s), "
             "none on the host GUI thread) - it did not marshal", g_report_msgs);
        ok = 0;
    }

    /* --- 干净卸载：子线程已被插件收掉，视图与控制都拆干净 --- */
    if (PI_FAILED(pi_host_session_unload(g_session, g_slots[0]))) {
        Fail("unload failed: %s", pi_host_session_last_error(g_session));
        ok = 0;
    }
    g_slots[0] = PI_HOST_SESSION_INVALID_SLOT;

    TearDownHostShell();
    PiTestMutexDestroy(&g_marshal_mutex);

    if (!ok) ++g_failures;
    LogStatus("[host] worker-thread result: failures=%d", g_failures);
    LogStatus("RESULT: %s", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * Mode: W-02 - switching the embed container of a LIVE view
 *
 * `pi_host_default_set_ui_window()` can point the host services at another
 * container at runtime; the plugin's view then has to follow it. The scenario
 * the coverage matrix was missing is exactly this: load -> attach into
 * container A -> switch to container B -> switch back to A -> resize round trip
 * -> unload, with every step asserted (child-of-the-right-container, detach is
 * synchronous, geometry follows the container).
 *
 * Two things change on a switch, and both are checked:
 *   - what the host's IPiHostUI reports (the live value a plugin QIs), via
 *     pi_host_default_set_ui_window();
 *   - where the plugin's native window actually lives, via pi_view_detach() +
 *     pi_view_attach(new_container) on the live view. The L0 session has no
 *     "move" call (it tracks one attach per slot), so the host drives the view
 *     API directly here - which is what the framework's view contract is for.
 * -------------------------------------------------------------------------- */
#define PI_SWITCH_FRAMES_CALM 8

static void DriveFrames(int frames)
{
    for (int i = 0; i < frames; ++i) {
        if (!PumpOnce()) return;
        ::Sleep(10);
    }
}

static HWND PluginWindow(IPiPluginView* view)
{
    return (HWND)(uintptr_t)pi_view_get_native_window(view);
}

static int ClientWidth(HWND w)
{
    RECT r;
    if (!w || !GetClientRect(w, &r)) return 0;
    return (int)(r.right - r.left);
}

static int ClientHeight(HWND w)
{
    RECT r;
    if (!w || !GetClientRect(w, &r)) return 0;
    return (int)(r.bottom - r.top);
}

static void ResizeContainer(HWND container, int x, int y, int w, int h)
{
    if (container) ::SetWindowPos(container, NULL, x, y, w, h, SWP_NOZORDER);
}

/* 宿主的 IPiHostUI 现在报告哪个容器 —— 就是插件 QI 一次会看到的那个值
 * （每次 QI 都是新包装，但读的是宿主活值，所以这里读到的等价于插件读到的） */
static HWND QueryHostUiWindow(IPiHostServices* services)
{
    void* out = NULL;
    HWND  window = NULL;
    if (PI_FAILED(pi_iunknown_query_interface((IPiUnknown*)services, &PI_IID_HOST_UI, &out)) || !out)
        return NULL;
    window = (HWND)(uintptr_t)pi_host_ui_get_parent_window((IPiHostUI*)out);
    pi_iunknown_release((IPiUnknown*)out);
    return window;
}

/* 一步一断言：插件窗口必须活着、可见、是**指定容器**的子窗口，并且尺寸与容器
 * 客户区一致（±2 px：Qt 在 DPI 缩放下的取整，以及消息循环的一帧延迟）。 */
static void CheckEmbedded(const char* stage, IPiPluginView* view, HWND expected_container,
                          int expect_w, int expect_h)
{
    HWND window = PluginWindow(view);
    HWND parent;
    int  w, h;

    if (!window || !IsWindow(window)) {
        Fail("%s: the plugin has no live native window", stage);
        return;
    }
    parent = GetParent(window);
    if (parent != expected_container) {
        Fail("%s: plugin window 0x%p is not a child of the expected container (0x%p, got 0x%p)",
             stage, (void*)window, (void*)expected_container, (void*)parent);
        return;
    }
    if (!IsWindowVisible(window)) {
        Fail("%s: plugin window 0x%p is not visible", stage, (void*)window);
        return;
    }
    w = ClientWidth(window);
    h = ClientHeight(window);
    if (expect_w > 0 && expect_h > 0 &&
        (w < expect_w - 2 || w > expect_w + 2 || h < expect_h - 2 || h > expect_h + 2)) {
        Fail("%s: plugin window is %dx%d, expected the container's %dx%d",
             stage, w, h, expect_w, expect_h);
        return;
    }
    LogStatus("%s: plugin window=0x%p container=0x%p size=%dx%d - ok",
              stage, (void*)window, (void*)parent, w, h);
}

/* 一次完整的切换：关掉投递闸 -> detach（同步销毁控件）-> 改宿主报告窗口 ->
 * attach 到新容器 -> 泵几帧 */
static int SwitchContainerTo(IPiPluginView* view, HWND target, const char* stage)
{
    HWND before = PluginWindow(view);

    pi_view_detach(view);
    /* detach 的契约是**同步**的：返回时控件的原生窗口必须已经不在了。
     * 这条断言正是"切换容器"值得单独测的原因——旧窗口要是还在，新窗口就会
     * 和它抢同一个容器的绘制区域。 */
    if (before && IsWindow(before)) {
        Fail("%s: the old plugin window 0x%p survived pi_view_detach()", stage, (void*)before);
        return 1;
    }

    pi_host_default_set_ui_window(g_services, (PiNativeWindow)target);
    if (QueryHostUiWindow(g_services) != target) {
        Fail("%s: pi_host_default_set_ui_window() did not reach the host's IPiHostUI", stage);
        return 1;
    }

    if (PI_FAILED(pi_view_attach(view, (PiNativeWindow)target))) {
        Fail("%s: pi_view_attach(new container) failed", stage);
        return 1;
    }
    pi_view_set_visible(view, 1);
    DriveFrames(PI_SWITCH_FRAMES_CALM);
    return 0;
}

static int RunContainerSwitchMode(const char* dll_path)
{
    IPiPluginView* view = NULL;
    HWND container_a, container_b, last_window;
    int  ok = 1;

    LogStatus("== piplugin container-switch test host (W-02) ==");
    LogStatus("plugin = %s", dll_path);

    g_slots[0] = PI_HOST_SESSION_INVALID_SLOT;
    if (CreateHostShell(2, 0) != 0) return 1;   /* 两个容器；宿主服务先报告 A */

    container_a = g_containers[0];
    container_b = g_containers[1];
    /* 两个容器给**不同**的确定尺寸：切过去以后尺寸对得上才说明几何真的跟着走 */
    ResizeContainer(container_a, 0,   0, 420, 300);
    ResizeContainer(container_b, 440, 0, 640, 360);
    ::UpdateWindow(g_window);

    if (PI_FAILED(pi_host_session_load(g_session, dll_path, &g_slots[0]))) {
        Fail("load failed: %s", pi_host_session_last_error(g_session));
        TearDownHostShell();
        return 1;
    }
    view = pi_host_session_get_view(g_session, g_slots[0]);
    if (!view) {
        Fail("the plugin published no view");
        TearDownHostShell();
        return 1;
    }

    /* 1) attach 到 A（宿主服务此刻也报告 A） */
    if (QueryHostUiWindow(g_services) != container_a) {
        Fail("the host services do not report container A before the first attach");
        ok = 0;
    }
    if (PI_FAILED(pi_host_session_attach_view(g_session, g_slots[0],
                                              (PiNativeWindow)container_a, 1))) {
        Fail("attach into container A failed: %s", pi_host_session_last_error(g_session));
        TearDownHostShell();
        return 1;
    }
    DriveFrames(PI_SWITCH_FRAMES_CALM);
    CheckEmbedded("attach A", view, container_a, ClientWidth(container_a), ClientHeight(container_a));

    /* 2) 切到 B */
    if (SwitchContainerTo(view, container_b, "switch A->B") != 0) ok = 0;
    else CheckEmbedded("switch A->B", view, container_b,
                       ClientWidth(container_b), ClientHeight(container_b));

    /* 3) 再切回 A */
    if (SwitchContainerTo(view, container_a, "switch B->A") != 0) ok = 0;
    else CheckEmbedded("switch B->A", view, container_a,
                       ClientWidth(container_a), ClientHeight(container_a));

    /* 4) 尺寸往返：小 -> 大 -> 小，容器与 view 一起变，每一步都断言几何 */
    {
        static const int kSizes[3][2] = { { 320, 240 }, { 780, 500 }, { 360, 280 } };
        char stage[64];
        for (int i = 0; i < 3; ++i) {
            const int w = kSizes[i][0], h = kSizes[i][1];
            ResizeContainer(container_a, 0, 0, w, h);
            if (PI_FAILED(pi_view_on_resize(view, w, h))) {
                Fail("resize round trip %d: pi_view_on_resize failed", i);
                ok = 0;
                continue;
            }
            DriveFrames(PI_SWITCH_FRAMES_CALM);
            snprintf(stage, sizeof(stage), "resize %dx%d", w, h);
            CheckEmbedded(stage, view, container_a, ClientWidth(container_a), ClientHeight(container_a));
        }
    }

    /* 5) 卸载：控件必须随着七步序列一起消失（此后宿主才敢 FreeLibrary） */
    last_window = PluginWindow(view);
    if (PI_FAILED(pi_host_session_unload(g_session, g_slots[0]))) {
        Fail("unload failed: %s", pi_host_session_last_error(g_session));
        ok = 0;
    }
    g_slots[0] = PI_HOST_SESSION_INVALID_SLOT;
    if (last_window && IsWindow(last_window)) {
        Fail("the plugin window 0x%p still exists after unload", (void*)last_window);
        ok = 0;
    } else {
        LogStatus("unload: the plugin window is gone (detach on the way out was synchronous)");
    }

    TearDownHostShell();

    if (!ok) ++g_failures;
    LogStatus("[host] container-switch result: failures=%d", g_failures);
    LogStatus("RESULT: %s", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * Mode: W-05 - two imgui plugin modules in one process
 *
 * The coverage matrix already had "several Qt plugins in one process" (APP-08)
 * and "imgui plugin inside a non-imgui host", but not the combination that
 * shares process-level resources between two imgui plugins: each view registers
 * its own window class, creates its OWN ImGui context and its OWN D3D11 device,
 * and the kit swaps the current ImGui context in and out around every callback
 * (`with_own_context`). The kit is a STATIC library, so no state is shared
 * between the two modules - which is exactly why loading TWO DIFFERENT module
 * files (not one file twice) is the interesting case: their window classes,
 * contexts and devices must coexist without stepping on each other.
 *
 * Asserted per plugin: its own container, visible, non-empty geometry, frames
 * actually RENDERED (the draw callback reports its frame counter), that counter
 * advanced over the run, and that its window is still alive and still parented
 * to its own container after all those frames. Then both unload cleanly and
 * their windows are gone.
 * -------------------------------------------------------------------------- */
#define PI_IMGUI_PAIR_MAX_FRAMES 400   /* ~4 s at 10 ms/frame */
#define PI_IMGUI_PAIR_MIN_FRAMES 30    /* "several frames" the hard way */

static int RunImguiPairMode(const char* path_a, const char* path_b)
{
    HWND windows[PI_MULTI_PLUGIN_COUNT];
    uint32_t driven = 0;
    int ok = 1;

    g_paths[0] = path_a;
    g_paths[1] = path_b;
    g_slots[0] = PI_HOST_SESSION_INVALID_SLOT;
    g_slots[1] = PI_HOST_SESSION_INVALID_SLOT;
    windows[0] = windows[1] = NULL;

    LogStatus("== piplugin multi-plugin test host (W-05: two imgui plugins) ==");
    LogStatus("plugin[0] = %s", g_paths[0]);
    LogStatus("plugin[1] = %s", g_paths[1]);

    if (CreateHostShell(PI_MULTI_PLUGIN_COUNT, 0) != 0) return 1;

    /* 载入 + 各自嵌进自己的容器（容器由宿主创建并摆位） */
    for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
        PiResult hr = pi_host_session_load(g_session, g_paths[i], &g_slots[i]);
        if (PI_FAILED(hr)) {
            Fail("plugin[%d] load failed (hr=%d): %s", i, (int)hr,
                 pi_host_session_last_error(g_session));
            g_slots[i] = PI_HOST_SESSION_INVALID_SLOT;
            ok = 0;
            continue;
        }
        hr = pi_host_session_attach_view(g_session, g_slots[i],
                                         (PiNativeWindow)g_containers[i], 1);
        if (PI_FAILED(hr)) {
            Fail("plugin[%d] attach failed (hr=%d): %s", i, (int)hr,
                 pi_host_session_last_error(g_session));
            ok = 0;
            continue;
        }
        CheckPluginUi((unsigned)i);      /* 自己的容器 / 可见 / 非空矩形 */
        windows[i] = PluginWindow(pi_host_session_get_view(g_session, g_slots[i]));
    }

    /* imgui 套件在本进程里的形态：STATIC -> 每个插件模块各带一份，进程里没有
     * 独立的套件模块。打印出来是为了让日志能区分"没共享"和"共享了一份"。 */
    {
        HMODULE kit = ::GetModuleHandleA("piplugin_imgui.dll");
        LogStatus("imgui adapter kit: %s", kit ? "loaded as a separate module"
                                               : "statically linked into each plugin (one copy per module)");
    }

    /* 驱动帧，直到两个插件都报过心跳（= 各自的 draw 回调真的跑了） */
    while (driven < PI_IMGUI_PAIR_MAX_FRAMES) {
        if (!PumpOnce()) break;
        ++driven;
        if (g_slots[0] != PI_HOST_SESSION_INVALID_SLOT &&
            g_slots[1] != PI_HOST_SESSION_INVALID_SLOT &&
            g_imgui_frames[0] > 0 && g_imgui_frames[1] > 0 && driven >= PI_IMGUI_PAIR_MIN_FRAMES)
            break;
        ::Sleep(10);
    }
    LogStatus("driven %u frame(s)", driven);

    if (driven < PI_IMGUI_PAIR_MIN_FRAMES) {
        Fail("only %u frame(s) were driven (expected at least %d)",
             driven, PI_IMGUI_PAIR_MIN_FRAMES);
        ok = 0;
    }

    for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
        IPiPluginView* view;
        if (g_slots[i] == PI_HOST_SESSION_INVALID_SLOT) continue;
        view = pi_host_session_get_view(g_session, g_slots[i]);

        LogStatus("plugin[%d]: %u heartbeat(s), last frame counter=%llu",
                  i, (unsigned)g_imgui_frames[i], (unsigned long long)g_imgui_last_tick[i]);
        if (g_imgui_frames[i] == 0) {
            Fail("plugin[%d] never reported a rendered frame - its draw callback was not "
                 "driven while the other plugin was alive", i);
            ok = 0;
        }
        if (g_imgui_last_tick[i] < 2) {
            Fail("plugin[%d] reported only frame %llu - it rendered the first frame and "
                 "then stopped", i, (unsigned long long)g_imgui_last_tick[i]);
            ok = 0;
        }
        /* 跑了这么多帧之后，两套窗口/context/设备必须都还活着，且没有互相串容器 */
        CheckEmbedded("after the frame loop", view, g_containers[i],
                      ClientWidth(g_containers[i]), ClientHeight(g_containers[i]));
    }

    /* 一起干净卸载（逆序），控件必须随卸载序列消失 */
    for (int i = PI_MULTI_PLUGIN_COUNT - 1; i >= 0; --i) {
        if (g_slots[i] == PI_HOST_SESSION_INVALID_SLOT) continue;
        if (PI_FAILED(pi_host_session_unload(g_session, g_slots[i]))) {
            Fail("plugin[%d] failed to unload: %s", i, pi_host_session_last_error(g_session));
            ok = 0;
        }
        g_slots[i] = PI_HOST_SESSION_INVALID_SLOT;
    }
    for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
        if (windows[i] && IsWindow(windows[i])) {
            Fail("plugin[%d] window 0x%p still exists after unload", i, (void*)windows[i]);
            ok = 0;
        } else if (windows[i]) {
            LogStatus("plugin[%d]: window gone after unload", i);
        }
    }

    TearDownHostShell();

    if (!ok) ++g_failures;
    LogStatus("[host] imgui-pair result: failures=%d frames=%u/%u heartbeats=%u/%u",
              g_failures, (unsigned)g_imgui_last_tick[0], (unsigned)g_imgui_last_tick[1],
              (unsigned)g_imgui_frames[0], (unsigned)g_imgui_frames[1]);
    LogStatus("RESULT: %s", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    /* Qt drives the clipboard / drag-and-drop through OLE on Windows; the host
     * owns that lifetime (see tests/test_host for why). */
    ::OleInitialize(NULL);

    if (argc > 2 && strcmp(argv[1], "--post-thread") == 0)
        return RunPostThreadMode(argv[2]);
    if (argc > 2 && strcmp(argv[1], "--container-switch") == 0)
        return RunContainerSwitchMode(argv[2]);
    if (argc > 3 && strcmp(argv[1], "--imgui-pair") == 0)
        return RunImguiPairMode(argv[2], argv[3]);

    return RunMultiPluginMode((argc > 1) ? argv[1] : "pi_test_plugin_qt.dll",
                              (argc > 2) ? argv[2] : "pi_test_plugin_qt2.dll");
}
