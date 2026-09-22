/*
 * piplugin - multi-plugin test host (roadmap APP-08)
 *
 * The acceptance case for the Qt adapter kit being SHARED: ONE process loads
 * TWO different Qt plugin modules at the same time, gives each of them its own
 * container window, drives both of their event loops, and unloads both.
 *
 * Why two different DLLs and not one DLL twice: a module's static data is
 * per-module, so loading the same file twice shares the adapter's state even
 * when the kit is a static library - that setup cannot see the bug. Two
 * distinct modules each used to get their own copy of `QApplication*`, so the
 * second `pi_attach()` tried to build a second QApplication in one process.
 * With the kit built as a DLL there is one copy of that state, and the second
 * plugin reuses the QApplication the first one created.
 *
 * What this host asserts (any failure -> exit code 1, printed to stdout and to
 * pi_multi_host.log):
 *   1. both modules load and instantiate through the normal L0 session;
 *   2. each plugin publishes a view, and the native window that view created is
 *      a real, visible child of ITS OWN container (not of the other one);
 *   3. both plugins keep posting their heartbeat while the host drives idle -
 *      i.e. two live Qt widget trees in one QApplication, not one frozen;
 *   4. both unload cleanly, and the process exits 0.
 *
 * It is deliberately a plain Win32 host with no D3D and no resize loop: the
 * delicate rendering machinery lives in tests/test_host and must not be
 * perturbed by this test. The L0 session kit is what makes a low-effort second
 * host possible at all.
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"

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

static IPiHostServices*     g_services = NULL;
static PiPluginHostSession* g_session  = NULL;
static HWND                 g_window   = NULL;
static HWND                 g_containers[PI_MULTI_PLUGIN_COUNT];
static uint32_t             g_slots[PI_MULTI_PLUGIN_COUNT];
static uint32_t             g_heartbeats[PI_MULTI_PLUGIN_COUNT];
static const char*          g_paths[PI_MULTI_PLUGIN_COUNT];
static int                  g_failures = 0;

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

/* 心跳计数：0x2000 -> 变体 A，0x2001 -> 变体 B，其它消息只记录。 */
static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    if (msg == PI_QT_HEARTBEAT_A) ++g_heartbeats[0];
    else if (msg == PI_QT_HEARTBEAT_B) ++g_heartbeats[1];
    LogStatus("  [plugin message] msg=0x%04X wparam=%llu",
              msg, (unsigned long long)wparam);
}

/* --------------------------------------------------------------------------
 * Layout: the host decides where the two containers are (kit discipline: the
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
 * Per-plugin checks
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

int main(int argc, char** argv)
{
    WNDCLASSEXW wc;
    int frame;

    /* Qt drives the clipboard / drag-and-drop through OLE on Windows; the host
     * owns that lifetime (see tests/test_host for why). */
    ::OleInitialize(NULL);

    g_paths[0] = (argc > 1) ? argv[1] : "pi_test_plugin_qt.dll";
    g_paths[1] = (argc > 2) ? argv[2] : "pi_test_plugin_qt2.dll";
    g_slots[0] = PI_HOST_SESSION_INVALID_SLOT;
    g_slots[1] = PI_HOST_SESSION_INVALID_SLOT;

    LogStatus("== piplugin multi-plugin test host (APP-08) ==");
    LogStatus("plugin[0] = %s", g_paths[0]);
    LogStatus("plugin[1] = %s", g_paths[1]);

    /* 1) window + two containers, both created and positioned by the host */
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

    for (int i = 0; i < PI_MULTI_PLUGIN_COUNT; ++i) {
        g_containers[i] = ::CreateWindowExW(0, L"STATIC", NULL,
                                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                            0, 0, 10, 10, g_window, NULL, wc.hInstance, NULL);
        if (!g_containers[i]) { LogStatus("FATAL: cannot create container %d", i); return 1; }
    }
    LayoutContainers();
    ::ShowWindow(g_window, SW_SHOW);
    ::UpdateWindow(g_window);

    /* 2) host services + L0 session (loading / gates / unload order) */
    if (PI_FAILED(pi_host_services_create_default(&HostMessageProc, NULL,
                                                  (PiNativeWindow)g_containers[0],
                                                  &g_services))) {
        LogStatus("FATAL: cannot create host services");
        return 1;
    }
    if (PI_FAILED(pi_host_session_create(g_services, &g_session))) {
        LogStatus("FATAL: cannot create the host session");
        return 1;
    }
    pi_host_session_set_logger(g_session, &SessionLogProc, NULL);

    /* 3) load BOTH modules at the same time - the point of this host. They go
     *    into two slots of the same session; the kit is multi-slot by design. */
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
        MSG msg;
        while (::PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { frame = PI_MULTI_FRAMES; break; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        pi_host_session_drive_idle(g_session);   /* 一个 session 驱动全部槽位 */
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

    pi_host_session_destroy(g_session); g_session = NULL;
    if (g_services) { pi_iunknown_release((IPiUnknown*)g_services); g_services = NULL; }

    LogStatus("[host] multi-plugin result: failures=%d heartbeats=%u/%u",
              g_failures, (unsigned)g_heartbeats[0], (unsigned)g_heartbeats[1]);
    LogStatus("RESULT: %s", g_failures ? "FAIL" : "PASS");

    if (g_failures) return 1;
    return 0;
}
