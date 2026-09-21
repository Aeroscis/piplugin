/*
 * pipluginframework — Test Host (imgui + Win32 + DirectX 11)
 *
 * Demonstrates:
 *   1. Loading a plugin DLL at runtime
 *   2. Creating an IPiHostServices object (with optional IPiHostUI
 *      capability) and handing it to the plugin
 *   3. Inspecting the plugin's LV2-style capability declarations
 *   4. Embedding the plugin Qt widget inside the host window
 *   5. Pumping the plugin event loop via pi_on_idle() each frame
 */
#include "pipluginframework/pi_plugin.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <d3d11.h>
/* IDXGIFactory2 / DXGI_SWAP_CHAIN_DESC1 / IDXGISwapChain2 (frame-latency
 * waitable object) live in the newer DXGI headers. */
#include <dxgi1_3.h>
#include <tchar.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <cstdarg>

/* Forward declarations */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */
static ID3D11Device*           g_pd3dDevice       = NULL;
static ID3D11DeviceContext*    g_pd3dDeviceContext = NULL;
static IDXGISwapChain*         g_pSwapChain        = NULL;
static ID3D11RenderTargetView* g_mainRenderTargetView = NULL;
/* Signalled by DXGI when the app may queue another frame. Waiting on it before
 * letting Windows apply a drag step is what keeps Present() non-blocking. */
static HANDLE                  g_frameLatencyWaitable = NULL;
/* Flags the swap chain was created with: ResizeBuffers() rejects the call with
 * E_INVALIDARG if they do not match (notably the frame-latency waitable flag). */
static UINT                    g_swapChainFlags = 0;

static IPiPluginBase*     g_plugin       = NULL;
static IPiPluginView*     g_pluginView   = NULL;
static IPiService*        g_pluginService = NULL;  /* demonstrates headless capability query */
static PiPluginModule*    g_pluginModule = NULL;
static IPiHostServices*   g_hostServices = NULL;
static char               g_status[256]  = "No plugin loaded";
static char               g_lastMessage[128] = "(none)";

static HWND g_embedContainer = NULL;  /* child window to hold plugin */

/* Swap-chain presentation model actually in use. The flip model is what
 * makes the embedded plugin window survive our Present() calls: with the
 * legacy bitblt models DXGI blits the back buffer straight over the whole
 * client area, i.e. right across the plugin's child HWND. */
static const char* g_presentModel = "unknown";
static bool        g_flipModel    = false;

/* Frames rendered since startup (self-test pacing / diagnostics). */
static unsigned    g_frameCount   = 0;

/* --------------------------------------------------------------------------
 * Interactive sizing, run by the host itself
 *
 * The reason is ordering. Windows' own modal size loop resizes the window and
 * only then sends WM_SIZE, so the first thing DWM can show for the new size is
 * the previous frame, scaled - and Present() on a flip-model chain can block
 * for a whole refresh (measured 6.2-6.4 ms), which is exactly how long that
 * scaled frame stays on screen. Every attempt to shorten the interval inside
 * that loop still leaves the window resized before any matching frame exists.
 *
 * So the host runs the loop and orders the steps the only way that has no such
 * moment:
 *
 *     ResizeBuffers + render for the NEW size   (the window still has the old
 *                                                size, and the frame on screen
 *                                                still matches that old size)
 *  -> Present()                                 (blocking here is harmless, for
 *                                                the same reason)
 *  -> SetWindowPos(window, new rectangle)       (queued frame == window)
 *  -> container/plugin resize                   (Qt repaints synchronously)
 *
 * Title-bar moves are left to Windows - they never change the size, so Aero
 * Snap by dragging the title bar keeps working; snapping a *sizing* drag is the
 * one thing this gives up.
 * -------------------------------------------------------------------------- */
static bool     g_sizeLoopActive = false;  /* inside RunSizeLoop() */
static bool     g_sizeMoveActive = false;  /* a drag is in progress (logging/state) */
static bool     g_renderingFrame = false;  /* re-entrancy guard for RenderFrame */
static unsigned g_resizeFrames   = 0;      /* steps rendered during one drag */
static unsigned g_resizeFailures = 0;      /* ResizeBuffers returned a failure */
static unsigned g_clientW = 0, g_clientH = 0;      /* client size last applied */
/* Non-client size (window minus client): needed to turn a dragged window
 * rectangle into a client size, and vice versa. */
static int      g_frameDW = 0, g_frameDH = 0;
/* Drag-path timing, written to the log when a drag ends (diagnostics). */
static double   g_presentTotalMs = 0.0, g_presentMaxMs = 0.0;
static double   g_prepareTotalMs = 0.0, g_prepareMaxMs = 0.0;
static bool     RenderFrame(bool resizeFrame, bool present, unsigned overrideW, unsigned overrideH);
static void     PrepareFrameFor(unsigned w, unsigned h);
static void     ResizeContainerAndPlugin(unsigned clientW, unsigned clientH);

/* Milliseconds from a monotonic counter (diagnostics only). */
static double NowMs()
{
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/* --------------------------------------------------------------------------
 * Self-test driver (--cycles N [--plugin a.dll,b.dll] [--idle-frames N])
 *
 * Runs real load -> attach -> idle-frame -> detach -> release -> unload
 * cycles through exactly the same code path the buttons use, but paced by
 * the real render loop. That makes plugin lifetime bugs (Qt widget teardown
 * racing the module unload) reproducible without a human clicking, and it
 * keeps the process alive long enough to catch crashes that only show up
 * once the Qt event loop is running.
 * -------------------------------------------------------------------------- */
static int      g_selfTestCycles     = 0;   /* 0 = disabled */
static int      g_selfTestCycleIndex = 0;
static unsigned g_selfTestIdleFrames = 20;
static const char* g_pluginOverride  = NULL;
static bool     g_selfTestFailed     = false;
/* Self-test option: skip pi_view_detach() and let the plugin's own
 * pi_terminate() do the teardown - exercises the "host just drops the
 * module" path that used to crash. */
static bool     g_skipDetach         = false;
/* --screenshot <file>: after a few rendered frames with the plugin loaded,
 * capture the composited window to a .bmp and exit. Lets an automated run
 * prove that the embedded plugin is actually visible over the D3D frame. */
static const char* g_screenshotPath  = NULL;
static unsigned g_screenshotAfter    = 45;   /* frames to settle first */

/* Message log shared between plugin threads and the UI thread */
#include <mutex>
static std::mutex g_logMutex;
static char       g_log[2048];

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */
static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void LoadPlugin(const char* dllPath);
static void UnloadPlugin();
static void CreateEmbedContainer(HWND parent);
static std::string ExeDirPath(const char* dllName);
static void LogStatus(const char* fmt, ...);
static void SelfTestStep();
static DWORD WINAPI DetachWatchdog(LPVOID param);
static bool CaptureWindowBmp(HWND hwnd, const char* path);
static bool CaptureHwndClientBmp(HWND hwnd, const char* path);

/* Set the UI status line and mirror it to the debug log. */
static void SetStatus(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    LogStatus("%s", g_status);
}

/* Debug log next to the exe (also captures load results for automated
 * verification without touching the UI). */
static void LogStatus(const char* fmt, ...)
{
    static FILE* f = NULL;
    if (!f) {
        char logPath[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, logPath, MAX_PATH);
        std::string s(logPath, len > 0 && len < MAX_PATH ? len : 0);
        size_t slash = s.find_last_of("\\/");
        if (slash != std::string::npos) s = s.substr(0, slash + 1);
        s += "pi_test_host.log";
        fopen_s(&f, s.c_str(), "a");
        if (!f) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    va_end(ap);
}

/* --------------------------------------------------------------------------
 * Host message callback (may be called from any plugin thread)
 * -------------------------------------------------------------------------- */
static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    std::lock_guard<std::mutex> lock(g_logMutex);
    snprintf(g_lastMessage, sizeof(g_lastMessage), "msg=0x%04X wparam=%llu",
             msg, (unsigned long long)wparam);
    size_t len = strlen(g_log);
    if (len + 64 < sizeof(g_log))
        snprintf(g_log + len, sizeof(g_log) - len, "[0x%04X] wparam=%llu\n",
                 msg, (unsigned long long)wparam);
}

/* --------------------------------------------------------------------------
 * One host frame: ImGui UI + optional Present.
 *
 * resizeFrame: this frame belongs to an interactive drag (counted in the log).
 * present:     false leaves the rendered frame in the back buffer for a later
 *              Present (see PrepareFrameFor()).
 * overrideW/H: lay the frame out for this client size instead of the window's
 *              current one - the caller knows the window is about to get it.
 *
 * Nothing in here dispatches messages, so a resize frame cannot re-enter
 * itself; the guard only covers a nested WM_SIZE while a frame is presented.
 * -------------------------------------------------------------------------- */
static bool RenderFrame(bool resizeFrame, bool present, unsigned overrideW, unsigned overrideH)
{
    if (g_renderingFrame || !g_pSwapChain || !g_mainRenderTargetView)
        return false;
    g_renderingFrame = true;
    if (resizeFrame)
        ++g_resizeFrames;

    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (overrideW && overrideH) {
        /* The window still has another size; lay this frame out for the one it
         * is about to get (the projection and the layout both follow this). */
        io.DisplaySize = ImVec2((float)overrideW, (float)overrideH);
    }
    ImGui::NewFrame();

    /* -------------------- ImGui UI -------------------- */
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(400, (float)io.DisplaySize.y));
        ImGui::Begin("Plugin Control", NULL,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::Text("Status: %s", g_status);

        ImGui::Separator();

        if (ImGui::Button("Load Qt Plugin")) {
            LoadPlugin(ExeDirPath("pi_test_plugin_qt.dll").c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Unload Plugin")) {
            UnloadPlugin();
        }

        ImGui::Separator();

        if (g_pluginModule) {
            IPiPluginFactory* factory = NULL;
            if (PI_SUCCEEDED(pi_module_get_factory(g_pluginModule, &factory))) {
                const PiPluginDescriptor* desc = NULL;
                pi_factory_get_descriptor(factory, &desc);
                if (desc) {
                    ImGui::Text("Plugin: %s", desc->name);
                    ImGui::Text("Vendor: %s", desc->vendor);
                    ImGui::Text("Version: %s", desc->version);
                    ImGui::Text("API: 0x%08X", desc->api_version);

                    /* Capability declarations (LV2-style) */
                    if (desc->capability_count > 0) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Capabilities:");
                        for (uint32_t i = 0; i < desc->capability_count; ++i) {
                            const PiPluginCapability* cap = &desc->capabilities[i];
                            const char* kind =
                                (cap->flags & PI_CAP_PROVIDES) ? "provides" :
                                (cap->flags & PI_CAP_REQUIRED) ? "requires" : "optional";
                            uint32_t id = cap->iid.data1;
                            const char* what =
                                pi_guid_equal(&cap->iid, &PI_IID_PLUGIN_VIEW) ? "PLUGIN_VIEW" :
                                pi_guid_equal(&cap->iid, &PI_IID_HOST_UI)    ? "HOST_UI"    :
                                pi_guid_equal(&cap->iid, &PI_IID_SERVICE)    ? "SERVICE"    : "?";
                            ImGui::BulletText("%s %s (iid=%08X)", kind, what, id);
                        }
                    }
                }
                pi_iunknown_release((IPiUnknown*)factory);
            }

            ImGui::Separator();
            ImGui::Text("Has GUI view:   %s", g_pluginView ? "yes" : "no");
            ImGui::Text("Has service:    %s", g_pluginService ? "yes" : "no");

            ImGui::Separator();
            ImGui::Text("Last plugin message: %s", g_lastMessage);
            ImGui::TextUnformatted("Log:");
            {
                std::lock_guard<std::mutex> lock(g_logMutex);
                ImGui::TextUnformatted(g_log);
            }
        }

        ImGui::End();
    }
    /* -------------------------------------------------- */

    ImGui::Render();

    const float clear_color[4] = { 0.15f, 0.15f, 0.15f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    /* The flip model is composited by DWM, so presenting without waiting for
     * vsync cannot tear. resizeFrame uses interval 0: the size loop wants the
     * frame queued as early as possible (a frame that is still in flight keeps
     * the *previous* frame - which matches the window as it is at that moment -
     * on screen, and that is fine by construction). */
    if (present)
        g_pSwapChain->Present(resizeFrame ? 0 : 1, 0);

    g_renderingFrame = false;
    return true;
}

/* --------------------------------------------------------------------------
 * Resize the back buffer to w x h and render one host frame into it, laid out
 * for that size, WITHOUT presenting it. Used by the size loop: at this point
 * the window still has its previous size and the frame on screen still matches
 * it, so nothing on screen changes yet.
 * -------------------------------------------------------------------------- */
static void PrepareFrameFor(unsigned w, unsigned h)
{
    if (!g_pSwapChain || !g_pd3dDeviceContext || w == 0 || h == 0)
        return;

    double t0 = NowMs();
    ++g_resizeFrames;

    CleanupRenderTarget();
    HRESULT hr = g_pSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_swapChainFlags);
    if (FAILED(hr)) {
        if (g_resizeFailures == 0)
            LogStatus("resize: ResizeBuffers(%ux%u) FAILED hr=0x%08X", w, h, (unsigned)hr);
        ++g_resizeFailures;
        hr = g_pSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_swapChainFlags);
        if (FAILED(hr)) {
            LogStatus("resize: retry FAILED hr=0x%08X", (unsigned)hr);
            CreateRenderTarget();
            return;
        }
    }
    CreateRenderTarget();
    if (!g_mainRenderTargetView)
        return;

    RenderFrame(/*resizeFrame=*/true, /*present=*/false, w, h);

    double ms = NowMs() - t0;
    g_prepareTotalMs += ms;
    if (ms > g_prepareMaxMs) g_prepareMaxMs = ms;
}

/* --------------------------------------------------------------------------
 * Resize the plugin's container (and let Qt repaint its child window). Runs
 * after the frame for the new size has been presented, so Qt's synchronous
 * repaint cannot delay the pixels.
 * -------------------------------------------------------------------------- */
static void ResizeContainerAndPlugin(unsigned clientW, unsigned clientH)
{
    if (!g_embedContainer) return;
    int w = (int)clientW - 410;
    int h = (int)clientH;
    if (w < 0) w = 0;
    SetWindowPos(g_embedContainer, NULL, 410, 0, w, h, SWP_NOZORDER);
    if (g_pluginView && w > 0 && h > 0)
        pi_view_on_resize(g_pluginView, w, h);
}

/* --------------------------------------------------------------------------
 * The host's own interactive sizing loop. Started from WM_NCLBUTTONDOWN when
 * the click landed on a sizing border (see the comment block near the globals
 * for why Windows' own loop cannot be used). Returns when the button is
 * released; ESC cancels the drag and restores the original rectangle.
 * -------------------------------------------------------------------------- */
static void RunSizeLoop(HWND hWnd, UINT hitTest)
{
    RECT startRect;
    if (!GetWindowRect(hWnd, &startRect)) return;

    RECT wr, cr;
    if (GetWindowRect(hWnd, &wr) && GetClientRect(hWnd, &cr)) {
        g_frameDW = (wr.right - wr.left) - cr.right;
        g_frameDH = (wr.bottom - wr.top) - cr.bottom;
        g_clientW = (unsigned)cr.right;
        g_clientH = (unsigned)cr.bottom;
    }

    POINT startCursor;
    GetCursorPos(&startCursor);

    const int minClientW = 520, minClientH = 360;
    g_resizeFrames = 0;
    g_resizeFailures = 0;
    g_presentTotalMs = g_presentMaxMs = 0.0;
    g_prepareTotalMs = g_prepareMaxMs = 0.0;
    g_sizeMoveActive = true;
    SetCapture(hWnd);
    LogStatus("resize: host size loop started (hit=0x%04X client=%ux%u)",
              (unsigned)hitTest, g_clientW, g_clientH);

    bool cancelled = false;
    for (;;) {
        /* Wait for input (or a few ms) rather than spinning: the size only
         * changes when the cursor moves or the button changes. */
        MsgWaitForMultipleObjectsEx(0, NULL, 5, QS_ALLINPUT, 0);
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage(0); cancelled = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (cancelled) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { cancelled = true; break; }
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000) || GetCapture() != hWnd)
            break;   /* button released or capture lost: the drag is over */

        POINT cur;
        GetCursorPos(&cur);
        int dx = cur.x - startCursor.x;
        int dy = cur.y - startCursor.y;
        RECT r = startRect;
        if (hitTest == HTLEFT || hitTest == HTTOPLEFT || hitTest == HTBOTTOMLEFT)
            r.left += dx;
        if (hitTest == HTRIGHT || hitTest == HTTOPRIGHT || hitTest == HTBOTTOMRIGHT)
            r.right += dx;
        if (hitTest == HTTOP || hitTest == HTTOPLEFT || hitTest == HTTOPRIGHT)
            r.top += dy;
        if (hitTest == HTBOTTOM || hitTest == HTBOTTOMLEFT || hitTest == HTBOTTOMRIGHT)
            r.bottom += dy;
        /* Keep a usable minimum, measured in client pixels. */
        if (r.right - r.left < minClientW + g_frameDW) r.left = r.right - (minClientW + g_frameDW);
        if (r.bottom - r.top < minClientH + g_frameDH) r.top = r.bottom - (minClientH + g_frameDH);

        unsigned cw = (unsigned)((r.right - r.left) - g_frameDW);
        unsigned ch = (unsigned)((r.bottom - r.top) - g_frameDH);
        if (cw == g_clientW && ch == g_clientH)
            continue;   /* nothing moved far enough to change the size */

        /* 1. frame ready for the size the window is about to get */
        PrepareFrameFor(cw, ch);
        /* 2. queue it while the window still has the old size: a blocking
         *    Present waits here, and while it waits the screen still shows a
         *    frame that matches the window */
        double p0 = NowMs();
        g_pSwapChain->Present(0, 0);
        double pms = NowMs() - p0;
        g_presentTotalMs += pms;
        if (pms > g_presentMaxMs) g_presentMaxMs = pms;
        /* 3. now make the window match the frame that is already queued */
        SetWindowPos(hWnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        g_clientW = cw;
        g_clientH = ch;
        /* 4. and only now the plugin/container (Qt repaints synchronously) */
        ResizeContainerAndPlugin(cw, ch);
    }

    if (cancelled)
        SetWindowPos(hWnd, NULL, startRect.left, startRect.top,
                     startRect.right - startRect.left, startRect.bottom - startRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    ReleaseCapture();
    g_sizeMoveActive = false;
    LogStatus("resize: host size loop ended after %u steps (failures=%u) "
              "present avg=%.2fms max=%.2fms  prepare avg=%.2fms max=%.2fms",
              g_resizeFrames, g_resizeFailures,
              g_resizeFrames ? g_presentTotalMs / g_resizeFrames : 0.0, g_presentMaxMs,
              g_resizeFrames ? g_prepareTotalMs / g_resizeFrames : 0.0, g_prepareMaxMs);
}

/* --------------------------------------------------------------------------
 * WinMain
 * -------------------------------------------------------------------------- */
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow)
{
    /* DPI awareness MUST be established before the first window exists.
     * Plugins bring their own GUI toolkit (Qt in this repo) and that toolkit
     * may change the process DPI awareness when it starts up. Changing it
     * after our window exists makes Windows re-fit and re-scale every window
     * we own - the host visibly jumps to a different size at plugin load
     * time. Setting it up front keeps the geometry stable. */
    ImGui_ImplWin32_EnableDpiAwareness();

    /* Qt on Windows drives the clipboard / drag-and-drop through OLE and
     * expects the process' GUI thread to have OLE initialised. When it is
     * not, QWindowsContext calls OleInitialize()/OleUninitialize() itself -
     * in the test setup that happens on the Qt runtime thread at plugin
     * unload time, which tears process-wide COM state down under our feet.
     * Initialising it here (main thread, before any Qt code runs) keeps the
     * lifetime owned by the host. */
    ::OleInitialize(NULL);

    /* Create window.
     * WS_CLIPCHILDREN is essential: without it the window's own painting
     * region includes the embedded plugin HWND, so every frame we present
     * erases the plugin's pixels. */
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                       NULL, NULL, NULL, NULL, L"PiTestHost", NULL };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"pipluginframework — Test Host (imgui)",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                100, 100, 1280, 720,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;

    /* Initialize Direct3D */
    if (!CreateDeviceD3D(hwnd)) { ::UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    /* Setup Dear ImGui */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    /* Create embed container for plugin views */
    CreateEmbedContainer(hwnd);

    /* Create the host services object. Because we pass a valid embed
     * window it exposes IPiHostUI; a headless host would pass
     * PI_INVALID_WINDOW instead. */
    if (PI_FAILED(pi_host_services_create_default(&HostMessageProc, NULL,
                                                  (PiNativeWindow)g_embedContainer,
                                                  &g_hostServices))) {
        g_hostServices = NULL;
    }

    /* Command line.
     *   <plugin.dll>                auto-load once (legacy behaviour)
     *   --plugin <dll>[,<dll>...]   plugin(s) used by the self-test
     *   --cycles <n>                run n load/unload cycles, then exit
     *   --idle-frames <n>           frames to let the view live per cycle */
    for (int i = 1; i < __argc; ++i) {
        const char* a = __argv[i];
        if (a && strcmp(a, "--cycles") == 0 && i + 1 < __argc) {
            g_selfTestCycles = atoi(__argv[++i]);
        } else if (a && strcmp(a, "--plugin") == 0 && i + 1 < __argc) {
            g_pluginOverride = __argv[++i];
        } else if (a && strcmp(a, "--idle-frames") == 0 && i + 1 < __argc) {
            int n = atoi(__argv[++i]);
            g_selfTestIdleFrames = (n > 0) ? (unsigned)n : 1u;
        } else if (a && strcmp(a, "--skip-detach") == 0) {
            g_skipDetach = true;
        } else if (a && strcmp(a, "--screenshot") == 0 && i + 1 < __argc) {
            g_screenshotPath = __argv[++i];
        } else if (a && strcmp(a, "--screenshot-frames") == 0 && i + 1 < __argc) {
            int n = atoi(__argv[++i]);
            g_screenshotAfter = (n > 0) ? (unsigned)n : 1u;
        } else if (a && a[0] == '-') {
            LogStatus("warning: unknown option '%s'", a);
        }
    }
    if (g_selfTestCycles <= 0 && __argc > 1 && __argv[1] && __argv[1][0] != '-') {
        LogStatus("auto-load: %s", __argv[1]);
        LoadPlugin(__argv[1]);
    }

    LogStatus("startup: present-model=%s ffi=%s", g_presentModel,
              g_flipModel ? "yes" : "no");
    LogStatus("startup: self-test-cycles=%d idle-frames=%u plugin=%s",
              g_selfTestCycles, g_selfTestIdleFrames,
              g_pluginOverride ? g_pluginOverride : "(default)");

    /* Main loop */
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        /* Pump plugin events each frame */
        if (g_pluginView) {
            pi_view_on_idle(g_pluginView);
        }

        if (g_selfTestCycles > 0)
            SelfTestStep();

        if (g_screenshotPath && g_pluginView && ++g_frameCount >= g_screenshotAfter) {
            LogStatus("screenshot: capturing after %u frames", g_frameCount);
            if (CaptureWindowBmp(hwnd, g_screenshotPath))
                LogStatus("screenshot: wrote %s", g_screenshotPath);
            else
                LogStatus("screenshot: FAILED for %s", g_screenshotPath);

            /* Also grab the plugin's own HWND: proves what Qt itself painted,
             * independent of DWM composition. */
            std::string pluginShot = std::string(g_screenshotPath) + ".plugin.bmp";
            HWND pluginHwnd = (HWND)(uintptr_t)pi_view_get_native_window(g_pluginView);
            unsigned tries = 0;
            while (pluginHwnd && ++tries < 30) {
                if (CaptureHwndClientBmp(pluginHwnd, pluginShot.c_str())) {
                    LogStatus("screenshot: wrote %s (plugin hwnd)", pluginShot.c_str());
                    break;
                }
                Sleep(50);
            }
            done = true;
        }

        RenderFrame(false, true, 0, 0);
    }

    /* Cleanup */
    UnloadPlugin();
    if (g_hostServices) { pi_iunknown_release((IPiUnknown*)g_hostServices); g_hostServices = NULL; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::OleUninitialize();

    LogStatus("exit: cycles=%d failed=%d", g_selfTestCycleIndex,
              g_selfTestFailed ? 1 : 0);
    if (g_selfTestCycles > 0)
        return (g_selfTestFailed || g_selfTestCycleIndex < g_selfTestCycles) ? 2 : 0;
    return 0;
}

/* --------------------------------------------------------------------------
 * Plugin load / unload
 * -------------------------------------------------------------------------- */
static std::string ExeDirPath(const char* dllName)
{
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string s(exePath, len > 0 && len < MAX_PATH ? len : 0);
    size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s.resize(slash + 1);
    return s + dllName;
}

static void LoadPlugin(const char* dllPath)
{
    UnloadPlugin();

    if (!g_hostServices) {
        SetStatus("Host services unavailable");
        return;
    }

    PiPluginModule* module = pi_module_load(dllPath);
    if (!module) {
        SetStatus("Load failed: %s", pi_module_get_load_error());
        return;
    }

    /* Diagnostics: report exactly which image the loader mapped, so a stale
     * copy shadowing our build output cannot hide. */
    {
        HMODULE hmod = GetModuleHandleA(dllPath);
        if (!hmod) {
            const char* base = strrchr(dllPath, '\\');
            hmod = GetModuleHandleA(base ? base + 1 : dllPath);
        }
        char full[MAX_PATH] = { 0 };
        if (hmod && GetModuleFileNameA(hmod, full, MAX_PATH))
            LogStatus("load: mapped %s", full);
        else
            LogStatus("load: mapped (unknown) for %s", dllPath);
    }

    IPiPluginFactory* factory = NULL;
    if (PI_FAILED(pi_module_get_factory(module, &factory))) {
        SetStatus("No factory in plugin");
        pi_module_unload(module);
        return;
    }

    const PiPluginDescriptor* desc = NULL;
    pi_factory_get_descriptor(factory, &desc);

    /* Capability gate (LV2-style): check requirements BEFORE instantiating */
    if (desc && pi_descriptor_requires(desc, &PI_IID_HOST_UI)) {
        void* dummy = NULL;
        if (PI_FAILED(pi_iunknown_query_interface((IPiUnknown*)g_hostServices,
                                                  &PI_IID_HOST_UI, &dummy))) {
            snprintf(g_status, sizeof(g_status),
                     "Rejected: plugin requires GUI host (we are headless)");
            pi_iunknown_release((IPiUnknown*)factory);
            pi_module_unload(module);
            return;
        }
        pi_iunknown_release((IPiUnknown*)dummy);
    }

    uint32_t count = pi_factory_get_class_count(factory);
    if (count == 0) {
        SetStatus("Plugin has no classes");
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        return;
    }

    PiGuid classGuid;
    pi_factory_get_class_guid(factory, 0, &classGuid);

    if (PI_FAILED(pi_factory_create_instance(factory, &classGuid, g_hostServices, &g_plugin))) {
        SetStatus("Failed to create instance");
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        return;
    }

    if (PI_FAILED(pi_plugin_initialize(g_plugin, g_hostServices))) {
        SetStatus("Plugin initialize failed");
        pi_iunknown_release((IPiUnknown*)g_plugin); g_plugin = NULL;
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        return;
    }

    /* Keep the module loaded for the plugin's lifetime */
    g_pluginModule = module;

    /* GUI view comes through the base interface contract */
    IPiPluginView* view = NULL;
    if (PI_SUCCEEDED(pi_plugin_get_view(g_plugin, &view)) && view) {
        g_pluginView = view;
    }
    /* Headless capabilities are discovered via QueryInterface */
    IPiService* service = NULL;
    if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)g_plugin,
                                                 &PI_IID_SERVICE, (void**)&service))) {
        g_pluginService = service;
    }

    if (g_pluginView && g_embedContainer) {
        if (PI_SUCCEEDED(pi_view_attach(g_pluginView, (PiNativeWindow)g_embedContainer))) {
            pi_view_set_visible(g_pluginView, 1);
            LogStatus("attach: plugin hwnd=%llx host container=%llx",
                      (unsigned long long)(uintptr_t)pi_view_get_native_window(g_pluginView),
                      (unsigned long long)(uintptr_t)g_embedContainer);
        }
    }

    if (g_selfTestCycles > 0 && !g_pluginView) {
        LogStatus("selftest: FAIL - plugin published no view");
        g_selfTestFailed = true;
    }

    pi_iunknown_release((IPiUnknown*)factory);

    if (desc) {
        SetStatus("Loaded: %s %s (view:%s service:%s)",
                 desc->name, desc->version,
                 g_pluginView ? "Y" : "N",
                 g_pluginService ? "Y" : "N");
    } else {
        SetStatus("Loaded (no descriptor)");
    }
}

static void UnloadPlugin()
{
    LogStatus("unload: begin (thread=%lu)", (unsigned long)GetCurrentThreadId());
    if (g_pluginService) {
        pi_service_stop(g_pluginService);
        pi_iunknown_release((IPiUnknown*)g_pluginService);
        g_pluginService = NULL;
    }
    if (g_pluginView) {
        DWORD t0 = GetTickCount();
        if (g_skipDetach) {
            LogStatus("unload: SKIPPING detach (plugin must tear itself down)");
        } else {
            LogStatus("unload: detaching view");
            /* Watchdog: if a teardown step wedges, break into an attached
             * debugger instead of hanging silently - a stack at the exact
             * blocking call is worth more than any amount of log spelunking. */
            CreateThread(NULL, 0, DetachWatchdog, (LPVOID)(uintptr_t)t0, 0, NULL);
            pi_view_detach(g_pluginView);
            LogStatus("unload: detach returned after %lu ms", (unsigned long)(GetTickCount() - t0));
        }
        LogStatus("unload: releasing view");
        pi_iunknown_release((IPiUnknown*)g_pluginView);
        g_pluginView = NULL;
        LogStatus("unload: view released (total %lu ms)", (unsigned long)(GetTickCount() - t0));
    }
    /* Release the plugin BEFORE unloading the module: the module's code
     * must stay mapped while plugin objects are alive. */
    if (g_plugin) {
        LogStatus("unload: terminating plugin");
        pi_plugin_terminate(g_plugin);
        LogStatus("unload: releasing plugin");
        pi_iunknown_release((IPiUnknown*)g_plugin);
        g_plugin = NULL;
        LogStatus("unload: plugin released");
    }
    if (g_pluginModule) {
        LogStatus("unload: unloading module");
        pi_module_unload(g_pluginModule);
        g_pluginModule = NULL;
        LogStatus("unload: module unloaded");
    }
    SetStatus("No plugin loaded");
    LogStatus("unload: done");
}

static void CreateEmbedContainer(HWND parent)
{
    RECT rect;
    GetClientRect(parent, &rect);

    /* WS_CLIPCHILDREN so our own painting (the D3D frame) never claims the
     * area the plugin's HWND lives in. */
    g_embedContainer = CreateWindowExW(
        0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        410, 0, rect.right - 410, rect.bottom,
        parent, NULL, GetModuleHandle(NULL), NULL
    );
}

/* Breaks into an attached debugger if a plugin teardown step exceeds the
 * budget, so the wedged stacks can be inspected in place. */
static DWORD WINAPI DetachWatchdog(LPVOID param)
{
    DWORD t0 = (DWORD)(uintptr_t)param;
    for (int i = 0; i < 200; ++i) {          /* <= 20 s */
        Sleep(100);
        if (GetTickCount() - t0 > 3000) break;
    }
    if (GetTickCount() - t0 <= 3000) return 0;   /* finished in time */
    if (!IsDebuggerPresent()) {
        LogStatus("watchdog: teardown stuck (>3 s) with no debugger attached");
        return 0;
    }
    LogStatus("watchdog: teardown stuck >3 s - breaking into debugger");
    DebugBreak();
    return 0;
}

/* --------------------------------------------------------------------------
 * Window capture: grab the whole window (DWM-composited content plus the
 * embedded plugin's child window) into a .bmp.
 * -------------------------------------------------------------------------- */
static bool CaptureHwndClientBmp(HWND hwnd, const char* path)
{
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return false;
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return false;

    HDC hdcWindow = GetDC(hwnd);
    if (!hdcWindow) return false;
    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    HBITMAP hbm = CreateCompatibleBitmap(hdcWindow, w, h);
    HGDIOBJ old = SelectObject(hdcMem, hbm);

    /* Ask the child window to paint itself into the memory DC. */
    SendMessageW(hwnd, WM_PRINTCLIENT, (WPARAM)hdcMem,
                 PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN | PRF_OWNED);
    BitBlt(hdcMem, 0, 0, w, h, hdcWindow, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    DWORD imageSize = (DWORD)w * 4u * (DWORD)h;
    unsigned char* pixels = (unsigned char*)malloc(imageSize);
    bool saved = false;
    if (pixels && GetDIBits(hdcMem, hbm, 0, (UINT)h, pixels, (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
        BITMAPFILEHEADER fh = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + imageSize;
        FILE* fp = NULL;
        if (fopen_s(&fp, path, "wb") == 0 && fp) {
            fwrite(&fh, sizeof(fh), 1, fp);
            fwrite(&bi, sizeof(bi), 1, fp);
            fwrite(pixels, 1, imageSize, fp);
            fclose(fp);
            saved = true;
        }
    }
    free(pixels);
    SelectObject(hdcMem, old);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);
    return saved;
}

static bool CaptureWindowBmp(HWND hwnd, const char* path)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return false;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ old = SelectObject(hdcMem, hbm);

    /* PW_RENDERFULLCONTENT asks DWM for the composited window, which is what
     * we want with a flip-model swap chain. */
    BOOL ok = PrintWindow(hwnd, hdcMem, 0x00000002 /* PW_RENDERFULLCONTENT */);
    if (!ok) {
        /* Fall back to a screen blit. */
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        UpdateWindow(hwnd);
        BitBlt(hdcMem, 0, 0, w, h, hdcScreen, rc.left, rc.top, SRCCOPY);
    }

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;                 /* top-down */
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    DWORD stride = (DWORD)w * 4;
    DWORD imageSize = stride * (DWORD)h;
    unsigned char* pixels = (unsigned char*)malloc(imageSize);
    bool saved = false;
    if (pixels && GetDIBits(hdcMem, hbm, 0, (UINT)h, pixels, (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
        BITMAPFILEHEADER fh = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + imageSize;
        FILE* fp = NULL;
        if (fopen_s(&fp, path, "wb") == 0 && fp) {
            fwrite(&fh, sizeof(fh), 1, fp);
            fwrite(&bi, sizeof(bi), 1, fp);
            fwrite(pixels, 1, imageSize, fp);
            fclose(fp);
            saved = true;
        }
    }
    free(pixels);
    SelectObject(hdcMem, old);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    (void)ok;
    return saved;
}

/* --------------------------------------------------------------------------
 * Self-test driver body - called once per rendered frame.
 * -------------------------------------------------------------------------- */static void SelfTestStep()
{
    static int      s_state = 0;        /* 0 idle, 1 idle-after-load, 2 idle-after-unload */
    static unsigned s_framesLeft = 0;

    if (s_framesLeft > 0) {
        --s_framesLeft;
        if (s_framesLeft == 0 && s_state == 1) {
            LogStatus("selftest: cycle %d/%d unload", g_selfTestCycleIndex, g_selfTestCycles);
            UnloadPlugin();
            s_state = 2;
            s_framesLeft = 3;
        }
        return;
    }

    switch (s_state) {
    case 0: {
        if (g_selfTestCycleIndex >= g_selfTestCycles) {
            LogStatus("selftest: PASS (%d cycles)", g_selfTestCycles);
            ::PostQuitMessage(0);
            return;
        }
        ++g_selfTestCycleIndex;
        const char* dll = g_pluginOverride ? g_pluginOverride : "pi_test_plugin_qt.dll";
        char first[MAX_PATH];
        const char* comma = strchr(dll, ',');
        size_t n = comma ? (size_t)(comma - dll) : strlen(dll);
        if (n >= sizeof(first)) n = sizeof(first) - 1;
        memcpy(first, dll, n);
        first[n] = 0;

        LogStatus("selftest: cycle %d/%d load %s", g_selfTestCycleIndex, g_selfTestCycles, first);
        LoadPlugin(ExeDirPath(first).c_str());
        if (!g_pluginView) {
            LogStatus("selftest: FAIL - no view after load");
            g_selfTestFailed = true;
            g_selfTestCycles = g_selfTestCycleIndex;   /* stop after this cycle */
        }
        s_state = 1;
        s_framesLeft = g_selfTestIdleFrames;
        break;
    }
    case 1:
        /* Frames ran out but the unload was skipped - defensive. */
        s_framesLeft = 1;
        break;
    case 2:
        s_state = 0;
        break;
    default:
        s_state = 0;
        break;
    }
}

/* --------------------------------------------------------------------------
 * WndProc
 * -------------------------------------------------------------------------- */
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_NCLBUTTONDOWN:
        /* A click on a sizing border starts OUR loop instead of the one Windows
         * would run: that one resizes the window before any matching frame can
         * exist, which is what makes DWM scale the previous frame. See the
         * comment block above RunSizeLoop(). Everything else (HTCLIENT,
         * HTCAPTION, ...) keeps the normal handling, so moving the window and
         * snapping it by its title bar still work. */
        switch (wParam) {
        case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
        case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
            if (!g_sizeLoopActive && g_pSwapChain) {
                g_sizeLoopActive = true;
                RunSizeLoop(hWnd, (UINT)wParam);
                g_sizeLoopActive = false;
            }
            return 0;   /* do not let DefWindowProc start its modal loop */
        default:
            break;
        }
        break;

    case WM_SIZE:
        g_clientW = (unsigned)LOWORD(lParam);
        g_clientH = (unsigned)HIWORD(lParam);
        if (g_sizeLoopActive)
            return 0;   /* the size loop already rendered and presented for it */
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_clientW, g_clientH,
                                        DXGI_FORMAT_UNKNOWN, g_swapChainFlags);
            CreateRenderTarget();
        }
        /* Container/plugin after the D3D resize: Qt repaints its child window
         * synchronously and that costs milliseconds (the plugin is composited
         * as a child window, so the order does not affect it). */
        ResizeContainerAndPlugin(g_clientW, g_clientH);
        return 0;

    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        /* Only reached for title-bar moves now (sizing drags are handled above):
         * nothing to do, the size never changes. */
        g_sizeMoveActive = (msg == WM_ENTERSIZEMOVE);
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

/* --------------------------------------------------------------------------
 * D3D11 helpers
 * -------------------------------------------------------------------------- */
static bool CreateDeviceD3D(HWND hWnd)
{
    /* The flip model is mandatory here: DWM composites a flip-model swap chain
     * as the window's content, so any child HWND owned by another module (our
     * embedded Qt plugin) is composed on top of it and stays visible. The legacy
     * bitblt models (DISCARD/SEQUENTIAL) hand the frame to the GDI blitter
     * instead, which paints over the child region - that is exactly the "plugin
     * is invisible unless the host blocks its own render loop" symptom.
     *
     * The swap chain is created explicitly through CreateSwapChainForHwnd
     * instead of D3D11CreateDeviceAndSwapChain for one reason: that is the only
     * call that accepts DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT. That
     * waitable object is what lets the host pace a border drag to the compositor
     * BEFORE Windows applies the new size, so the Present() in the WM_SIZE that
     * follows cannot block. A blocking Present is expensive in a way that shows:
     * while it blocks, the window already has the new size and DWM still only
     * has the previous (previous-sized, hence scaled) frame - the panel visibly
     * stretches or shrinks for that whole time. */
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        featureLevels, 2, D3D11_SDK_VERSION,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
            featureLevels, 2, D3D11_SDK_VERSION,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (FAILED(hr)) return false;

    IDXGIDevice*  dxgiDevice = NULL;
    IDXGIAdapter* adapter    = NULL;
    IDXGIFactory2* factory   = NULL;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width  = 0;
        sd.Height = 0;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        /* Three buffers: a drag presents once per size step and even with the
         * waitable object below a spare buffer costs nothing. */
        sd.BufferCount = 3;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        g_swapChainFlags = sd.Flags;
        IDXGISwapChain1* swapChain1 = NULL;
        hr = factory->CreateSwapChainForHwnd(g_pd3dDevice, hWnd, &sd, NULL, NULL,
                                             &swapChain1);
        if (SUCCEEDED(hr) && swapChain1) {
            /* Keep the base interface: everything else uses IDXGISwapChain. */
            swapChain1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain));
            factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
            g_flipModel    = true;
            g_presentModel = "FLIP_DISCARD+latency";
            IDXGISwapChain2* sc2 = NULL;
            if (SUCCEEDED(swapChain1->QueryInterface(IID_PPV_ARGS(&sc2)))) {
                sc2->SetMaximumFrameLatency(1);
                g_frameLatencyWaitable = sc2->GetFrameLatencyWaitableObject();
                sc2->Release();
            }
            LogStatus("d3d: swap chain %s, latency-waitable=%s",
                      g_presentModel, g_frameLatencyWaitable ? "yes" : "NO");
            swapChain1->Release();
        }
    }
    if (dxgiDevice) dxgiDevice->Release();
    if (adapter)    adapter->Release();
    if (factory)    factory->Release();

    if (!g_pSwapChain) {
        /* No flip model available on this system: fall back to the legacy
         * creation path (bitblt model), where the embedded child window is only
         * visible where the blitter does not overwrite it. The device created
         * above is dropped because that call creates its own. */
        if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
        if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = NULL; }
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 3;
        sd.BufferDesc.Width = 0;
        sd.BufferDesc.Height = 0;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        g_swapChainFlags = sd.Flags;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            featureLevels, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice,
            &featureLevel, &g_pd3dDeviceContext);
        if (FAILED(hr) || !g_pSwapChain) {
            CleanupDeviceD3D();
            return false;
        }
        g_flipModel    = false;
        g_presentModel = "DISCARD(bitblt)";
    }

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_frameLatencyWaitable) { CloseHandle(g_frameLatencyWaitable); g_frameLatencyWaitable = NULL; }
    if (g_pSwapChain)      { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice)      { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = NULL;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}
