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
#include <tchar.h>
#include <stdio.h>
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

static IPiPluginBase*     g_plugin       = NULL;
static IPiPluginView*     g_pluginView   = NULL;
static IPiService*        g_pluginService = NULL;  /* demonstrates headless capability query */
static PiPluginModule*    g_pluginModule = NULL;
static IPiHostServices*   g_hostServices = NULL;
static char               g_status[256]  = "No plugin loaded";
static char               g_lastMessage[128] = "(none)";

static HWND g_embedContainer = NULL;  /* child window to hold plugin */

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
 * WinMain
 * -------------------------------------------------------------------------- */
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow)
{
    /* Create window */
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                       NULL, NULL, NULL, NULL, L"PiTestHost", NULL };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"pipluginframework — Test Host (imgui)",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720,
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

    /* Auto-load for automated testing: pass the plugin DLL path on the
     * command line to skip clicking the button. */
    if (__argc > 1 && __argv[1]) {
        LogStatus("auto-load: %s", __argv[1]);
        LoadPlugin(__argv[1]);
    }

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
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

        g_pSwapChain->Present(1, 0); /* vsync on */
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
        }
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
    LogStatus("unload: begin");
    if (g_pluginService) {
        pi_service_stop(g_pluginService);
        pi_iunknown_release((IPiUnknown*)g_pluginService);
        g_pluginService = NULL;
    }
    if (g_pluginView) {
        LogStatus("unload: detaching view");
        pi_view_detach(g_pluginView);
        LogStatus("unload: releasing view");
        pi_iunknown_release((IPiUnknown*)g_pluginView);
        g_pluginView = NULL;
        LogStatus("unload: view released");
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

    g_embedContainer = CreateWindowExW(
        0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        410, 0, rect.right - 410, rect.bottom,
        parent, NULL, GetModuleHandle(NULL), NULL
    );
}

/* --------------------------------------------------------------------------
 * WndProc
 * -------------------------------------------------------------------------- */
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_embedContainer) {
            int w = LOWORD(lParam) - 410;
            int h = HIWORD(lParam);
            if (w < 0) w = 0;
            SetWindowPos(g_embedContainer, NULL, 410, 0, w, h, SWP_NOZORDER);
            if (g_pluginView && w > 0 && h > 0)
                pi_view_on_resize(g_pluginView, w, h);
        }
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                         DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
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
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        featureLevels, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice,
        &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
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
