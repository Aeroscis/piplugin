/*
 * piplugin — Test Host (imgui + Win32 + DirectX 11)
 *
 * Demonstrates:
 *   1. Loading a plugin DLL at runtime
 *   2. Creating an IPiHostServices object (with optional IPiHostUI
 *      capability) and handing it to the plugin
 *   3. Inspecting the plugin's LV2-style capability declarations
 *   4. Embedding the plugin Qt widget inside the host window
 *   5. Pumping the plugin event loop via pi_on_idle() each frame
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"
#include "pi_host_dx11.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
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

/* 宿主 kit L1（piplugin_host_dx11）持有"可嵌入子窗口的 D3D11 设备 +
 * flip-model 交换链"的创建参数与 resize 策略（含 DXGI_SCALING_NONE 降级路径）。
 * 本宿主只决定"窗口长什么样、画什么、什么时候画"。 */
static PiHostDx11Device* g_dx = NULL;

/* 宿主 kit L0（piplugin_host）持有全部插件生命周期机制：
 * 加载 / 双向能力门禁 / 实例化 / 七步卸载序列。 */
static IPiHostServices*     g_hostServices = NULL;
static PiPluginHostSession* g_session      = NULL;
static uint32_t             g_slot         = PI_HOST_SESSION_INVALID_SLOT;
static char                 g_status[256]  = "No plugin loaded";
static char                 g_lastMessage[128] = "(none)";

static HWND g_embedContainer = NULL;  /* child window to hold plugin（宿主自己创建、自己摆位） */

/* Frames rendered since startup (self-test pacing / diagnostics). */
static unsigned    g_frameCount   = 0;

/* --------------------------------------------------------------------------
 * Interactive sizing, run by the host itself
 *
 * Why Windows' own modal size loop cannot be used: it resizes the window and
 * only then sends WM_SIZE, so the first thing DWM can show for the new size is
 * the previous frame - and with a stretch swap chain DWM *rescales* that
 * frame, which squashes the panel by one composited frame per drag step.
 *
 * Running the loop ourselves lets us order the steps as
 *   render for the NEW size -> Present -> SetWindowPos(window, new rect)
 * but that alone does NOT close the race: DWM composites independently of us,
 * and whichever frame it picks can disagree with the current window size -
 *   - it composites after our Present but before our SetWindowPos:
 *     NEW-sized frame in an OLD-sized window  -> scaled,
 *   - it composites after SetWindowPos but before it picks up the queued
 *     frame: OLD-sized frame in a NEW-sized window -> scaled.
 * No ordering can make one frame size match the window before AND after the
 * geometry change. The only real fix is to DECOUPLE the two sizes:
 *
 *   1. The swap chain is created with DXGI_SCALING_NONE. Documented for
 *      CreateSwapChainForHwnd + FLIP_SEQUENTIAL/FLIP_DISCARD since Win8:
 *      when the back buffer and the window disagree in size, the buffer is
 *      shown 1:1, top-left aligned, and the rest of the target is filled
 *      with the swap-chain background colour. A stale frame is therefore
 *      CLIPPED, never rescaled - the panel cannot change shape at any point
 *      of the race.
 *   2. The back buffer is grown on demand and NEVER shrunk, so during a drag
 *      the buffer always covers the window and no per-step ResizeBuffers is
 *      needed at all; only the layout follows the client size (io.DisplaySize
 *      is overridden), the extra buffer area is cleared to the clear colour,
 *      so whatever a stale frame exposes at the margins is uniform grey.
 *      Growing happens at most once per drag (the first step that exceeds
 *      the current buffer), and even in that moment SCALING_NONE keeps the
 *      shape stable.
 *   3. SetBackgroundColor() equals the clear colour, so the "window exceeds
 *      the buffer" corner case paints the same grey instead of stretching.
 *
 * What is left of the race is one composited frame whose freshly exposed
 * margin shows the (uniform) clear colour instead of the new layout - far
 * below the threshold of perception, and exactly what the right-hand plugin
 * area covers anyway.
 *
 * Title-bar moves are left to Windows - they never change the size, so Aero
 * Snap by dragging the title bar keeps working; snapping a *sizing* drag is
 * the one thing this gives up.
 * -------------------------------------------------------------------------- */
static bool     g_sizeLoopActive = false;  /* inside RunSizeLoop() */
static bool     g_sizeMoveActive = false;  /* a drag is in progress (logging/state) */
static bool     g_renderingFrame = false;  /* re-entrancy guard for RenderFrame */
static unsigned g_resizeFrames   = 0;      /* steps rendered during one drag */
static unsigned g_clientW = 0, g_clientH = 0;      /* client size last applied */
/* 后台缓冲的大小与"只增不减"策略、SCALING_NONE 是否生效，都在 L1 kit 里；
 * 宿主只报"客户区想变成多大"。 */
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
/* --------------------------------------------------------------------------
 * Self-test / conformance driver
 *
 *   --cycles N [--plugin a.dll[,b.dll,...]] [--idle-frames N]
 *
 * 对**每一个**列出的插件跑 N 轮真实生命周期循环：
 *   load -> attach -> idle 若干帧 -> 拉伸窗口 -> 恢复尺寸 -> detach -> unload
 * 全部通过则退出码 0，任一环节失败则退出码 2。这是 ECO-02 的 conformance
 * harness：任何插件 DLL（含社区适配器产出的）都能拿官方宿主验收一次。
 * -------------------------------------------------------------------------- */
#define PI_SELFTEST_MAX_PLUGINS  16
#define PI_SELFTEST_RESIZE_FRAMES 4

static int      g_selfTestCycles      = 0;   /* 0 = disabled */
static int      g_selfTestCycleIndex  = 0;   /* 当前插件内的轮次 */
static unsigned g_selfTestIdleFrames  = 20;
static const char* g_pluginOverride   = NULL;
static bool     g_selfTestFailed      = false;

static char     g_selfTestPlugins[PI_SELFTEST_MAX_PLUGINS][MAX_PATH];
static int      g_selfTestPluginCount = 0;
static int      g_selfTestPluginIndex = 0;
static bool     g_selfTestDone        = false;   /* 全部跑完（用于退出码） */
static bool     g_selfTestAborted     = false;   /* 出现失败：跑完当前轮就收工 */

/* 尺寸往返用的原始窗口矩形，以及宿主自己的窗口（resize 要真的作用在它上面） */
static HWND     g_hostWindow   = NULL;
static RECT     g_selfTestRect = { 0, 0, 0, 0 };
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
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void LoadPlugin(const char* dllPath);
static void UnloadPlugin();
static void CreateEmbedContainer(HWND parent);
static std::string ExeDirPath(const char* dllName);
static void LogStatus(const char* fmt, ...);
static void SelfTestStep();
static void SelfTestParsePlugins(const char* list);
static DWORD WINAPI DetachWatchdog(LPVOID param);
static bool CaptureWindowBmp(HWND hwnd, const char* path);
static bool CaptureHwndClientBmp(HWND hwnd, const char* path);
static IPiPluginView* CurrentView(void);

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
 * Host kit L0 bridge
 *
 * kit 把自身步骤（load / attach / unload 各步）经日志回调上报，去向由本宿主
 * 决定 —— 这里就写进同一个日志文件，保持既有的诊断契约（脚本按日志模式断言）。
 * -------------------------------------------------------------------------- */
static void SessionLogProc(void* user_data, const char* message)
{
    (void)user_data;
    LogStatus("%s", message);
}

/* L1 dx11 kit 的日志（交换链创建、SCALING_NONE 降级、缓冲增长）也进同一个日志
 * 文件 —— 脚本正是按这些模式断言的（见 scripts/verify_resize_fix.ps1）。 */
static void Dx11LogProc(void* user_data, const char* message)
{
    (void)user_data;
    LogStatus("%s", message);
}

/* 当前槽位的 view（借用指针：所有权在 session，卸载也由 session 负责）。
 * 没有插件、插件是 headless、或槽位已卸载时返回 NULL。 */
static IPiPluginView* CurrentView(void)
{
    return pi_host_session_get_view(g_session, g_slot);
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
    ID3D11RenderTargetView* rtv = g_dx ? pi_host_dx11_render_target(g_dx) : NULL;
    ID3D11DeviceContext*    ctx = g_dx ? pi_host_dx11_context(g_dx) : NULL;
    if (g_renderingFrame || !g_dx || !rtv || !ctx)
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

        if (g_session && g_slot != PI_HOST_SESSION_INVALID_SLOT) {
            const PiPluginDescriptor* desc = pi_host_session_get_descriptor(g_session, g_slot);
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

            ImGui::Separator();
            ImGui::Text("Has GUI view:   %s",
                        pi_host_session_get_view(g_session, g_slot) ? "yes" : "no");
            ImGui::Text("Has service:    %s",
                        pi_host_session_get_service(g_session, g_slot) ? "yes" : "no");

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
    ctx->OMSetRenderTargets(1, &rtv, NULL);
    ctx->ClearRenderTargetView(rtv, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    /* The flip model is composited by DWM, so presenting without waiting for
     * vsync cannot tear. resizeFrame uses interval 0: the size loop wants the
     * frame queued as early as possible; while it is in flight the screen keeps
     * the previous frame, which SCALING_NONE clips 1:1 - shape-safe by
     * construction. */
    if (present)
        pi_host_dx11_present(g_dx, resizeFrame ? 0u : 1u);

    g_renderingFrame = false;
    return true;
}

/* --------------------------------------------------------------------------
 * Lay out and render one host frame for client size w x h WITHOUT presenting
 * it. Used by the size loop: at this point the window still has its previous
 * size, the frame on screen still matches it, and nothing on screen changes
 * yet.
 *
 * 后台缓冲怎么调（SCALING_NONE 时"只增不减"、否则精确跟随、ResizeBuffers 失败
 * 重试与计数）全部归 L1 kit —— 那是嵌入正确性的地基，不该由每个宿主重写一遍。
 * 本函数只负责"按即将生效的新尺寸渲染一帧、但不呈现"这个宿主侧动作与时序。
 * -------------------------------------------------------------------------- */
static void PrepareFrameFor(unsigned w, unsigned h)
{
    if (!g_dx || w == 0 || h == 0)
        return;

    double t0 = NowMs();
    ++g_resizeFrames;

    if (!pi_host_dx11_prepare_size(g_dx, w, h))
        return;   /* 渲染目标不可用：这一帧画不出来 */

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
    {
        /* resize 转发：容器归宿主、尺寸也归宿主，故由宿主发起 */
        IPiPluginView* view = CurrentView();
        if (view && w > 0 && h > 0)
            pi_view_on_resize(view, w, h);
    }
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
    pi_host_dx11_reset_resize_stats(g_dx);   /* 计数在 L1 kit 里，这里清零 */
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

        /* 1. frame ready for the size the window is about to get (the buffer
         *    keeps its size - SCALING_NONE clips what exceeds the window, so
         *    only genuine growth costs a ResizeBuffers, at most once a drag) */
        PrepareFrameFor(cw, ch);
        /* 2. queue it while the window still has the old size: a blocking
         *    Present waits here, and every frame the screen shows in the
         *    meantime - old or new - is clipped 1:1, never scaled */
        double p0 = NowMs();
        pi_host_dx11_present(g_dx, 0);
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
              g_resizeFrames, pi_host_dx11_resize_failures(g_dx),
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
     * WS_CLIPCHILDREN 是必需的（否则每次 Present 都会擦掉内嵌插件的像素）；
     * 这个"必须带上的风格位"由 L1 kit 提供，宿主仍旧自己创建、自己摆位。 */
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                       NULL, NULL, NULL, NULL, L"PiTestHost", NULL };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"piplugin — Test Host (imgui)",
                                WS_OVERLAPPEDWINDOW | pi_host_dx11_top_level_style(),
                                100, 100, 1280, 720,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;

    /* Initialize Direct3D。
     * 交换链的创建参数（flip model / DXGI_SCALING_NONE / 帧延迟等待对象 / 背景色）
     * 全部由 L1 kit 固化；宿主只提供自己的 HWND 与清屏色。 */
    {
        PiHostDx11Desc dxdesc;
        memset(&dxdesc, 0, sizeof(dxdesc));
        dxdesc.background[0] = 0.15f;   /* 与 RenderFrame 的清屏色一致 */
        dxdesc.background[1] = 0.15f;
        dxdesc.background[2] = 0.15f;
        dxdesc.background[3] = 1.0f;
        dxdesc.log           = &Dx11LogProc;
        dxdesc.log_user_data = NULL;
        if (PI_FAILED(pi_host_dx11_create((PiNativeWindow)hwnd, &dxdesc, &g_dx))) {
            ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return 1;
        }
    }
    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    /* Setup Dear ImGui */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(pi_host_dx11_device(g_dx), pi_host_dx11_context(g_dx));

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

    /* 宿主 kit L0：会话对象持有加载 / 双向门禁 / 实例化 / 七步卸载序列。
     * 本宿主只负责"容器是哪个窗口、怎么渲染、尺寸策略如何"。 */
    if (g_hostServices) {
        if (PI_FAILED(pi_host_session_create(g_hostServices, &g_session))) {
            g_session = NULL;
            LogStatus("startup: host session unavailable (load/unload disabled)");
        } else {
            pi_host_session_set_logger(g_session, &SessionLogProc, NULL);
        }
    }

    /* Command line.
     *   <plugin.dll>                     auto-load once (legacy behaviour)
     *   --plugin <dll>[,<dll>...]        plugin(s) the self-test cycles through
     *   --cycles <n>                     n load/attach/resize/unload cycles PER plugin
     *   --idle-frames <n>                frames to let each view live per cycle
     *
     * --cycles 模式即 conformance harness（roadmap ECO-02）：对列表里每个插件
     * 都跑完整生命周期 + 一次尺寸往返，全过 -> 退出码 0，否则 2。 */
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
    /* 尺寸往返要真的作用在宿主窗口上，故把 HWND 交给自检驱动 */
    g_hostWindow = hwnd;
    SelfTestParsePlugins(g_pluginOverride);

    if (g_selfTestCycles <= 0 && __argc > 1 && __argv[1] && __argv[1][0] != '-') {
        LogStatus("auto-load: %s", __argv[1]);
        LoadPlugin(__argv[1]);
    }
    /* --skip-detach 是诊断开关，交给 kit 的卸载序列执行 */
    if (g_skipDetach && g_session)
        pi_host_session_set_skip_detach(g_session, 1);

    LogStatus("startup: present-model=%s ffi=%s", pi_host_dx11_present_model(g_dx),
              pi_host_dx11_is_flip_model(g_dx) ? "yes" : "no");
    LogStatus("startup: self-test-cycles=%d idle-frames=%u plugins=%d",
              g_selfTestCycles, g_selfTestIdleFrames,
              g_selfTestCycles > 0 ? g_selfTestPluginCount : 0);
    for (int i = 0; i < (g_selfTestCycles > 0 ? g_selfTestPluginCount : 0); ++i)
        LogStatus("startup:   plugin[%d] = %s", i, g_selfTestPlugins[i]);

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

        /* Pump plugin events each frame（每帧 pump 的时机归宿主，kit 只提供机制） */
        if (g_session) {
            pi_host_session_drive_idle(g_session);
        }

        if (g_selfTestCycles > 0)
            SelfTestStep();

        if (g_screenshotPath && CurrentView() && ++g_frameCount >= g_screenshotAfter) {
            LogStatus("screenshot: capturing after %u frames", g_frameCount);
            if (CaptureWindowBmp(hwnd, g_screenshotPath))
                LogStatus("screenshot: wrote %s", g_screenshotPath);
            else
                LogStatus("screenshot: FAILED for %s", g_screenshotPath);

            /* Also grab the plugin's own HWND: proves what Qt itself painted,
             * independent of DWM composition. */
            std::string pluginShot = std::string(g_screenshotPath) + ".plugin.bmp";
            HWND pluginHwnd = (HWND)(uintptr_t)pi_view_get_native_window(CurrentView());
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
    if (g_session) { pi_host_session_destroy(g_session); g_session = NULL; }
    if (g_hostServices) { pi_iunknown_release((IPiUnknown*)g_hostServices); g_hostServices = NULL; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    pi_host_dx11_destroy(g_dx); g_dx = NULL;
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::OleUninitialize();

    LogStatus("exit: plugins=%d cycles/plugin=%d failed=%d aborted=%d done=%d",
              g_selfTestPluginCount, g_selfTestCycles,
              g_selfTestFailed ? 1 : 0, g_selfTestAborted ? 1 : 0, g_selfTestDone ? 1 : 0);
    if (g_selfTestCycles > 0)
        return (g_selfTestFailed || !g_selfTestDone) ? 2 : 0;
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

    if (!g_session) {
        SetStatus("Host session unavailable");
        return;
    }

    /* 加载 + 双向能力门禁 + 实例化 + 初始化：机制全在 kit 里，
     * 门禁在 create_instance 之前跑（顺序由 kit 保证，不再由宿主手抄）。 */
    uint32_t slot = PI_HOST_SESSION_INVALID_SLOT;
    PiResult hr = pi_host_session_load(g_session, dllPath, &slot);
    if (PI_FAILED(hr)) {
        /* 失败原因（pi_module_load 的错误描述、双向门禁的拒绝理由）由 kit 给出 */
        if (hr == PI_E_MISSINGCAPABILITY)
            SetStatus("Rejected: %s", pi_host_session_last_error(g_session));
        else
            SetStatus("Load failed: %s", pi_host_session_last_error(g_session));
        return;
    }
    g_slot = slot;

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

    /* 嵌入：容器是本宿主创建并摆位的，kit 只接收它并记账 */
    if (CurrentView() && g_embedContainer) {
        PiResult ahr = pi_host_session_attach_view(g_session, g_slot,
                                                  (PiNativeWindow)g_embedContainer, 1);
        if (PI_FAILED(ahr))
            LogStatus("attach failed (hr=%d): %s", (int)ahr,
                      pi_host_session_last_error(g_session));
    }

    if (g_selfTestCycles > 0 && !CurrentView()) {
        LogStatus("selftest: FAIL - plugin published no view");
        g_selfTestFailed = true;
    }

    {
        const PiPluginDescriptor* desc = pi_host_session_get_descriptor(g_session, g_slot);
        if (desc) {
            SetStatus("Loaded: %s %s (view:%s service:%s)",
                      desc->name, desc->version,
                      CurrentView() ? "Y" : "N",
                      pi_host_session_get_service(g_session, g_slot) ? "Y" : "N");
        } else {
            SetStatus("Loaded (no descriptor)");
        }
    }
}

static void UnloadPlugin()
{
    LogStatus("unload: begin (thread=%lu)", (unsigned long)GetCurrentThreadId());
    if (g_session && g_slot != PI_HOST_SESSION_INVALID_SLOT) {
        DWORD t0 = GetTickCount();
        /* Watchdog：整个卸载序列（含 kit 内部的 detach/terminate）超出预算就
         * break 进调试器，而不是无声卡死。kit 每一步都写日志，故"卡在哪一步"
         * 由最后一行日志定位 —— 这正是把序列内化后保留诊断能力的办法。 */
        if (!g_skipDetach)
            CreateThread(NULL, 0, DetachWatchdog, (LPVOID)(uintptr_t)t0, 0, NULL);

        pi_host_session_unload(g_session, g_slot);   /* 七步序列，顺序由 kit 保证 */
        g_slot = PI_HOST_SESSION_INVALID_SLOT;
        LogStatus("unload: sequence returned after %lu ms",
                  (unsigned long)(GetTickCount() - t0));
    }
    SetStatus("No plugin loaded");
    LogStatus("unload: done");
}

static void CreateEmbedContainer(HWND parent)
{
    RECT rect;
    GetClientRect(parent, &rect);

    /* 位置与大小由本宿主决定（左侧 410px 留给控制面板，其余给插件）；
     * "以正确的方式成为 embed host"（子窗口风格 + WS_CLIPCHILDREN/WS_CLIPSIBLINGS）
     * 由 L1 kit 固化 —— 少了它宿主自己的 D3D 帧会盖掉插件的像素。 */
    g_embedContainer = (HWND)pi_host_dx11_create_embed_container(
        (PiNativeWindow)parent, 410, 0, rect.right - 410, rect.bottom);
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
 * Self-test / conformance driver
 * -------------------------------------------------------------------------- */

/* 解析 --plugin 的逗号列表；未给则用默认插件。裁掉空格，忽略空项。 */
static void SelfTestParsePlugins(const char* list)
{
    const char* p = (list && *list) ? list : "pi_test_plugin_qt.dll";

    g_selfTestPluginCount = 0;
    g_selfTestPluginIndex = 0;
    g_selfTestCycleIndex  = 0;

    for (;;) {
        const char* comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);

        while (n > 0 && (p[0] == ' ' || p[0] == '\t')) { ++p; --n; }
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) --n;

        if (n > 0 && g_selfTestPluginCount < PI_SELFTEST_MAX_PLUGINS) {
            if (n >= MAX_PATH) n = MAX_PATH - 1;
            memcpy(g_selfTestPlugins[g_selfTestPluginCount], p, n);
            g_selfTestPlugins[g_selfTestPluginCount][n] = 0;
            ++g_selfTestPluginCount;
        }
        if (!comma || !comma[1]) break;
        p = comma + 1;
    }

    if (g_selfTestPluginCount == 0) {
        /* 列表里全是空项：退回默认插件，保证至少有东西可测 */
        memcpy(g_selfTestPlugins[0], "pi_test_plugin_qt.dll", sizeof("pi_test_plugin_qt.dll"));
        g_selfTestPluginCount = 1;
    }
}

/* 绝对路径原样使用（conformance 场景常给定完整路径），否则按可执行文件目录解析 */
static std::string ResolvePluginPath(const char* p)
{
    if (p && (p[0] == '\\' || p[0] == '/' || (p[0] && p[1] == ':')))
        return std::string(p);
    return ExeDirPath(p);
}

/* 尺寸往返：真的改宿主窗口尺寸，从而走完整的
 * WM_SIZE -> L1 交换链策略 -> 容器 resize -> 插件 resize 转发 链路 */
static void SelfTestResize(int grow)
{
    if (!g_hostWindow) return;

    if (grow) {
        int w, h;
        GetWindowRect(g_hostWindow, &g_selfTestRect);
        w = g_selfTestRect.right - g_selfTestRect.left;
        h = g_selfTestRect.bottom - g_selfTestRect.top;
        SetWindowPos(g_hostWindow, NULL, g_selfTestRect.left, g_selfTestRect.top,
                     (int)(w * 1.25), (int)(h * 1.25), SWP_NOZORDER | SWP_NOACTIVATE);
        LogStatus("selftest: resize grow -> %dx%d", (int)(w * 1.25), (int)(h * 1.25));
    } else {
        SetWindowPos(g_hostWindow, NULL, g_selfTestRect.left, g_selfTestRect.top,
                     g_selfTestRect.right - g_selfTestRect.left,
                     g_selfTestRect.bottom - g_selfTestRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LogStatus("selftest: resize restore");
    }
}

/* 自检主体，每渲染帧调用一次。
 * 状态机：0 起始 -> 1 加载后 idle -> 2 拉伸 -> 3 恢复 -> 4 卸载后冷却 -> 0 */
static void SelfTestStep()
{
    static int      s_state = 0;
    static unsigned s_framesLeft = 0;

    if (s_framesLeft > 0) {
        if (--s_framesLeft > 0) return;

        switch (s_state) {
        case 1:   /* idle 结束 -> 拉伸窗口，验证 resize 转发 */
            SelfTestResize(/*grow=*/1);
            s_state = 2;
            s_framesLeft = PI_SELFTEST_RESIZE_FRAMES;
            return;
        case 2:   /* 拉伸结束 -> 恢复原尺寸 */
            SelfTestResize(/*grow=*/0);
            s_state = 3;
            s_framesLeft = PI_SELFTEST_RESIZE_FRAMES;
            return;
        case 3:   /* 尺寸往返结束 -> 卸载 */
            LogStatus("selftest: [%d/%d] cycle %d/%d unload",
                      g_selfTestPluginIndex + 1, g_selfTestPluginCount,
                      g_selfTestCycleIndex, g_selfTestCycles);
            UnloadPlugin();
            s_state = 4;
            s_framesLeft = 3;
            return;
        case 4:   /* 卸载冷却结束 -> 下一轮或下一个插件 */
            s_state = 0;
            return;
        default:
            s_state = 0;
            return;
        }
    }

    if (s_state != 0) {
        /* 防御：状态与帧数不同步时重新起跳，不要卡死 */
        s_framesLeft = 1;
        return;
    }

    /* ---- idle：开新一轮、或换下一个插件、或收工 ---- */
    if (g_selfTestCycleIndex >= g_selfTestCycles) {
        g_selfTestPluginIndex++;
        g_selfTestCycleIndex = 0;

        if (g_selfTestAborted || g_selfTestPluginIndex >= g_selfTestPluginCount) {
            if (g_selfTestFailed)
                LogStatus("selftest: FAIL - aborted at plugin %d/%d",
                          g_selfTestPluginIndex, g_selfTestPluginCount);
            else
                LogStatus("selftest: PASS (%d plugin(s) x %d cycles)",
                          g_selfTestPluginCount, g_selfTestCycles);
            g_selfTestDone = true;
            ::PostQuitMessage(0);
            return;
        }

        LogStatus("selftest: plugin %d/%d -> %s", g_selfTestPluginIndex + 1,
                  g_selfTestPluginCount, g_selfTestPlugins[g_selfTestPluginIndex]);
    }

    ++g_selfTestCycleIndex;
    {
        const char* dll = g_selfTestPlugins[g_selfTestPluginIndex];
        LogStatus("selftest: [%d/%d] cycle %d/%d load %s",
                  g_selfTestPluginIndex + 1, g_selfTestPluginCount,
                  g_selfTestCycleIndex, g_selfTestCycles, dll);

        LoadPlugin(ResolvePluginPath(dll).c_str());
        if (!CurrentView()) {
            LogStatus("selftest: FAIL - no view after load");
            g_selfTestFailed  = true;
            g_selfTestAborted = true;      /* 跑完本轮的卸载后收工 */
        }
    }
    s_state = 1;
    s_framesLeft = g_selfTestIdleFrames;
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
            if (!g_sizeLoopActive && g_dx) {
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
        /* 缓冲/渲染目标的调整策略在 L1 kit 里：SCALING_NONE 生效时缓冲只增不减
         * （下一帧按新客户区布局，DWM 在缓冲覆盖之前 1:1 裁剪），否则精确跟随。 */
        if (g_dx && wParam != SIZE_MINIMIZED) {
            pi_host_dx11_prepare_size(g_dx, g_clientW, g_clientH);
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

