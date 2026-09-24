/*
 * piplugin example - a minimal host (~150 lines of actual logic)
 *
 * What it demonstrates:
 *   1. create a window and ONE container of your own (the kit never makes one);
 *   2. create the host services object, hand it the container, create a session;
 *   3. load a plugin, run the capability gate, instantiate it;
 *   4. if the plugin has a view: attach it to the container and pump it;
 *      if it has a service: start / poll / stop it (headless plugins work too);
 *   5. unload in the order the kit guarantees, then exit.
 *
 * Everything host-side that is mechanism (loading, gates, the seven-step unload
 * sequence) comes from the L0 host kit (`pi_host_session`), so this file only
 * contains decisions a host has to make anyway: what the window looks like,
 * where the container is, and when to pump.
 *
 * Build:  see README.md (three steps)
 * Run:    pi_example_minimal_host.exe [plugin.dll]
 *         defaults to pi_example_plugin_imgui.dll (built by CI as well)
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PI_PLUGIN_EXAMPLE_CONTAINER_LEFT 16
#define PI_PLUGIN_EXAMPLE_RUN_MS         1500   /* how long to drive the plugin */

static IPiPluginHostServices*     g_services = NULL;
static PiPluginHostSession* g_session  = NULL;
static HWND                 g_window   = NULL;
static HWND                 g_container = NULL;
static uint32_t             g_slot     = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
static int                  g_messages = 0;

/* The plugin posts messages here (any thread). A real host would marshal them
 * onto its own loop; for an example, printing is enough. */
static void OnHostMessage(void* user_data, uint32_t msg, uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    ++g_messages;
    printf("  [plugin message] msg=0x%04X wparam=%llu\n", msg, (unsigned long long)wparam);
}

static void OnSessionLog(void* user_data, const char* message)
{
    (void)user_data;
    printf("  [kit] %s\n", message);
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_SIZE:
        /* The container is ours to place: keep it filling the client area. */
        if (g_container)
            SetWindowPos(g_container, NULL, PI_PLUGIN_EXAMPLE_CONTAINER_LEFT, PI_PLUGIN_EXAMPLE_CONTAINER_LEFT,
                         LOWORD(lparam) - 2 * PI_PLUGIN_EXAMPLE_CONTAINER_LEFT,
                         HIWORD(lparam) - 2 * PI_PLUGIN_EXAMPLE_CONTAINER_LEFT,
                         SWP_NOZORDER);
        /* Forward the size to the plugin's view (the view is not ours to resize). */
        if (g_session && g_slot != PI_PLUGIN_HOST_SESSION_INVALID_SLOT) {
            IPiPluginView* view = pi_plugin_host_session_get_view(g_session, g_slot);
            if (view && LOWORD(lparam) > 0 && HIWORD(lparam) > 0)
                pi_plugin_view_on_resize(view, (int32_t)LOWORD(lparam), (int32_t)HIWORD(lparam));
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int main(int argc, char** argv)
{
    const char* dll = (argc > 1) ? argv[1] : "pi_example_plugin_imgui.dll";
    WNDCLASSEXW wc;
    PiResult hr;
    DWORD start;
    int exit_code = 0;

    printf("== piplugin minimal host ==\n");
    printf("plugin: %s\n", dll);

    /* --- 1) window + container (both created and placed by US) ------------ */
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"PiPluginExampleMinimalHost";
    RegisterClassExW(&wc);

    /* WS_CLIPCHILDREN: without it our own painting would erase the plugin. */
    g_window = CreateWindowW(wc.lpszClassName, L"piplugin - minimal host",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g_window) { printf("FATAL: no window\n"); return 1; }

    g_container = CreateWindowExW(0, L"STATIC", NULL,
                                  WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                  PI_PLUGIN_EXAMPLE_CONTAINER_LEFT, PI_PLUGIN_EXAMPLE_CONTAINER_LEFT,
                                  100, 100, g_window, NULL, wc.hInstance, NULL);
    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);

    /* --- 2) host services + session (the plugin gets this object) ---------- */
    hr = pi_plugin_host_services_create_default(&OnHostMessage, NULL,
                                         (PiNativeWindow)g_container, &g_services);
    if (PI_FAILED(hr)) { printf("FATAL: host services (hr=%d)\n", (int)hr); return 1; }

    hr = pi_plugin_host_session_create(g_services, &g_session);
    if (PI_FAILED(hr)) { printf("FATAL: session (hr=%d)\n", (int)hr); return 1; }
    pi_plugin_host_session_set_logger(g_session, &OnSessionLog, NULL);

    /* A real app would gate on what IT needs before loading anything:
     *   pi_plugin_host_session_require(g_session, &MY_APP_PROTOCOL_IID);
     * see examples/specialized_app for that in practice. */

    /* --- 3) load: module + capability gate + instantiate + initialize ------ */
    hr = pi_plugin_host_session_load(g_session, dll, &g_slot);
    if (PI_FAILED(hr)) {
        printf("load failed (hr=%d): %s\n", (int)hr, pi_plugin_host_session_last_error(g_session));
        exit_code = 1;
    } else {
        const PiPluginDescriptor* desc = pi_plugin_host_session_get_descriptor(g_session, g_slot);
        printf("loaded: %s %s (%s)\n",
               desc && desc->name ? desc->name : "?", desc && desc->version ? desc->version : "",
               desc && desc->category ? desc->category : "-");

        /* --- 4a) a view: attach it to OUR container and pump it ------------- */
        {
            IPiPluginView* view = pi_plugin_host_session_get_view(g_session, g_slot);
            if (view) {
                hr = pi_plugin_host_session_attach_view(g_session, g_slot,
                                                 (PiNativeWindow)g_container, /*set_visible=*/1);
                printf("view: %s (native window %p)\n",
                       PI_SUCCEEDED(hr) ? "attached to our container" : "attach FAILED",
                       (void*)(uintptr_t)pi_plugin_view_get_native_window(view));
                if (PI_FAILED(hr)) exit_code = 1;
            } else {
                printf("view: none (headless plugin)\n");
            }
        }

        /* --- 4b) a service: drive it from the loop below -------------------- */
        {
            IPiPluginService* service = pi_plugin_host_session_get_service(g_session, g_slot);
            if (service) {
                hr = pi_plugin_service_start(service, NULL, 0);
                printf("service: start -> %d\n", (int)hr);
                if (PI_FAILED(hr)) exit_code = 1;
            }
        }

        /* --- 5) our loop: pump messages, drive idle, poll the service ------- */
        printf("running for %d ms...\n", PI_PLUGIN_EXAMPLE_RUN_MS);
        start = GetTickCount();
        while (GetTickCount() - start < PI_PLUGIN_EXAMPLE_RUN_MS) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { start = 0; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            /* Exactly one call drives every view in the session; the cadence is
             * the host's decision. */
            if (g_slot != PI_PLUGIN_HOST_SESSION_INVALID_SLOT)
                pi_plugin_host_session_drive_idle(g_session);
            {
                IPiPluginService* service = pi_plugin_host_session_get_service(g_session, g_slot);
                if (service) pi_plugin_service_poll(service);
            }
            Sleep(10);
        }

        if (g_slot != PI_PLUGIN_HOST_SESSION_INVALID_SLOT) {
            IPiPluginService* service = pi_plugin_host_session_get_service(g_session, g_slot);
            if (service) {
                int32_t status = -1;
                pi_plugin_service_get_status(service, &status);
                printf("service: status=%d before stop\n", (int)status);
            }
            /* The seven-step unload sequence (service stop -> view detach ->
             * terminate -> factory -> module) is the kit's job; getting the order
             * wrong is how hosts crash on unload. */
            pi_plugin_host_session_unload(g_session, g_slot);
            g_slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
        }
        printf("plugin messages received: %d\n", g_messages);
    }

    pi_plugin_host_session_destroy(g_session); g_session = NULL;
    if (g_services) { pi_iunknown_release((IPiUnknown*)g_services); g_services = NULL; }
    if (g_window) { DestroyWindow(g_window); g_window = NULL; }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    printf("RESULT: %s\n", exit_code ? "FAIL" : "PASS");
    return exit_code;
}
