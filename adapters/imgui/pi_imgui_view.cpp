#include "pi_imgui_view.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

/* Forward declaration (deliberately not declared in the backend header
 * to avoid dragging <windows.h> into it). */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* ==========================================================================
 * PiImGuiView - IPiPluginView implemented with Dear ImGui + D3D11
 *
 * Everything runs on the host's GUI thread; the object therefore needs
 * no locking. Lifecycle:
 *   attach   -> create child HWND (owned by the plugin DLL module),
 *               D3D11 device + swapchain, ImGui context + backends
 *   on_idle  -> one ImGui frame: NewFrame / draw / Render / Present
 *   detach   -> synchronous teardown of everything above
 *   release  -> detach-if-needed, then delete this
 * ======================================================================== */
namespace {

class PiImGuiView {
public:
    PiRefCountedBase base;              /* MUST be first data member */

    PiImGuiView(const PiImGuiViewDesc& desc)
        : m_desc(desc), m_attached(false),
          m_hwnd(NULL), m_device(NULL), m_context(NULL),
          m_swapChain(NULL), m_rtv(NULL), m_imguiCtx(NULL) {}

    PiImGuiViewDesc m_desc;
    bool            m_attached;
    HWND            m_hwnd;
    ID3D11Device*           m_device;
    ID3D11DeviceContext*    m_context;
    IDXGISwapChain*         m_swapChain;
    ID3D11RenderTargetView* m_rtv;
    ImGuiContext*   m_imguiCtx;
    /* Window class name for THIS view. Unique per view on purpose - see
     * create_resources(). */
    wchar_t         m_className[64];

    static const IPiPluginViewVtbl s_vtbl;
    static PiImGuiView* from_iface(void* self_ptr) { return (PiImGuiView*)self_ptr; }

    bool create_resources(PiNativeWindow parent);
    void destroy_resources();
    void resize_backbuffer();

    /* Runs the draw callback with our ImGui context current, restoring
     * whatever context (possibly the host's own) was set before. */
    template <typename F>
    void with_own_context(F fn) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        if (m_imguiCtx) ImGui::SetCurrentContext(m_imguiCtx);
        fn();
        ImGui::SetCurrentContext(previous);   /* NULL is a valid value */
    }
};

/* --------------------------------------------------------------------------
 * Child window plumbing.
 *
 * The window class is registered against THIS MODULE's instance, so its WndProc
 * code (mapped from the plugin DLL) cannot outlive the module - and the name is
 * UNIQUE PER VIEW and unregistered again in destroy_resources().
 *
 * Both of those exist because of a real crash: a fixed class name is a
 * process-wide resource, and Windows does not drop a class when the module that
 * registered it is unloaded. A second imgui plugin module (or a reload after an
 * unload) that registers the same name gets RegisterClassExW == FALSE - which
 * used to be ignored - and then creates its window with the PREVIOUS module's
 * WndProc. The window works until it dispatches a message into code whose data is
 * gone: found by running a second imgui plugin through the conformance harness,
 * with the fault inside the Win32 backend's DPI helper called from USER32's
 * window-procedure dispatch. Per-view names + unregister make the class die with
 * its window, whatever the host does with the module.
 * ------------------------------------------------------------------------ */

static LRESULT WINAPI PiImGuiViewWndProc(HWND hWnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    PiImGuiView* view = (PiImGuiView*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (view) {
        /* Feed input into the plugin's ImGui Win32 backend. */
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        if (msg == WM_SIZE && wParam != SIZE_MINIMIZED) {
            view->resize_backbuffer();
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static HMODULE pi_imgui_kit_module(void)
{
    HMODULE module = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&PiImGuiViewWndProc, &module);
    return module;
}

/* --------------------------------------------------------------------------
 * Resource creation / destruction (host GUI thread only)
 * ------------------------------------------------------------------------ */

bool PiImGuiView::create_resources(PiNativeWindow parent)
{
    HWND parentHwnd = (HWND)parent;
    HMODULE module = pi_imgui_kit_module();
    WNDCLASSEXW wc = { sizeof(wc) };

    /* Unique per view: it cannot collide with another plugin's class, and
     * destroy_resources() unregisters it again. */
    swprintf_s(m_className, _countof(m_className),
               L"PiImGuiViewWnd_%p", (void*)this);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = &PiImGuiViewWndProc;
    wc.hInstance     = module;
    wc.lpszClassName = m_className;
    if (!RegisterClassExW(&wc)) {
        /* A stale class of the same name (this module reloaded at the same
         * address): drop it and try once more - it has no windows left, because
         * a view unregisters its own class in destroy_resources(). */
        UnregisterClassW(m_className, module);
        if (!RegisterClassExW(&wc)) return false;
    }

    RECT rc; GetClientRect(parentHwnd, &rc);
    m_hwnd = CreateWindowExW(0, m_className, L"",
                             WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                             0, 0, rc.right, rc.bottom,
                             parentHwnd, NULL, module, NULL);
    if (!m_hwnd) return false;
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    /* D3D11 device + swapchain bound to the child window. */
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        levels, 2, D3D11_SDK_VERSION,
        &sd, &m_swapChain, &m_device, &level, &m_context);
    if (FAILED(hr)) {
        /* Software fallback for machines without a suitable GPU. */
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &m_swapChain, &m_device, &level, &m_context);
        if (FAILED(hr)) {
            DestroyWindow(m_hwnd);
            m_hwnd = NULL;
            return false;
        }
    }

    ID3D11Texture2D* backBuffer = NULL;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, NULL, &m_rtv);
        backBuffer->Release();
    }

    /* ImGui context + backends, bound to OUR context.
     *
     * Save the host's context first. ImGui::CreateContext() alone would restore
     * it (it keeps the previous context if there was one), but this function
     * deliberately switches to ours and must therefore switch BACK before it
     * returns: everything the host does after pi_attach() - its own ImGui frames,
     * its own backend calls - belongs to the HOST's context. Leaving ours current
     * makes the host render its UI through the plugin's backend and device, which
     * is undefined behaviour (found by running examples/minimal_plugin_imgui
     * through the conformance harness: the host crashed on its next frame). */
    ImGuiContext* previous = ImGui::GetCurrentContext();
    m_imguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_imguiCtx);
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device, m_context);
    if (m_desc.init)
        m_desc.init(m_desc.user_data);
    ImGui::SetCurrentContext(previous);   /* NULL is a valid value */

    return true;
}

void PiImGuiView::resize_backbuffer()
{
    if (!m_swapChain) return;
    if (m_rtv) { m_rtv->Release(); m_rtv = NULL; }
    m_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* backBuffer = NULL;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, NULL, &m_rtv);
        backBuffer->Release();
    }
}

void PiImGuiView::destroy_resources()
{
    if (!m_hwnd) return;   /* already torn down */

    /* Detach the WndProc first: DestroyWindow re-enters it. */
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)NULL);

    with_own_context([this]() {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(m_imguiCtx);
        m_imguiCtx = NULL;
    });

    if (m_rtv)       { m_rtv->Release();       m_rtv = NULL; }
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = NULL; }
    if (m_context)   { m_context->Release();   m_context = NULL; }
    if (m_device)    { m_device->Release();    m_device = NULL; }

    DestroyWindow(m_hwnd);
    m_hwnd = NULL;

    /* The class dies with its window: no process-wide name survives this module. */
    if (m_className[0])
        UnregisterClassW(m_className, pi_imgui_kit_module());
}

/* --------------------------------------------------------------------------
 * vtable slots (host GUI thread)
 * ------------------------------------------------------------------------ */

PiResult PI_CALL piimgui_qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_VIEW)) {
        *out = me;
        me->base.unk.lpVtbl->pi_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

PiResult PI_CALL piimgui_attach(void* self_ptr, PiNativeWindow parent)
{
    if (!PI_IS_VALID_WINDOW(parent)) return PI_E_INVALIDARG;
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    if (me->m_attached) return PI_FAIL;

    if (!me->create_resources(parent))
        return PI_FAIL;

    me->m_attached = true;
    if (me->m_desc.retain)
        me->m_desc.retain(me->m_desc.user_data);
    return PI_OK;
}

PiResult PI_CALL piimgui_detach(void* self_ptr)
{
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    if (!me->m_attached) return PI_OK;

    me->m_attached = false;
    if (me->m_desc.release)
        me->m_desc.release(me->m_desc.user_data);
    me->destroy_resources();
    return PI_OK;
}

PiNativeWindow PI_CALL piimgui_get_native_window(void* self_ptr)
{
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    return (PiNativeWindow)me->m_hwnd;
}

PiResult PI_CALL piimgui_on_resize(void* self_ptr, int32_t w, int32_t h)
{
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    if (!me->m_attached || !me->m_hwnd) return PI_OK;
    SetWindowPos(me->m_hwnd, NULL, 0, 0, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return PI_OK;
}

PiResult PI_CALL piimgui_on_idle(void* self_ptr)
{
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    if (!me->m_attached || !me->m_imguiCtx) return PI_OK;

    me->with_own_context([me]() {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        me->m_desc.draw(me->m_desc.user_data);

        ImGui::Render();
        const float clear[4] = { 0.16f, 0.16f, 0.18f, 1.0f };
        me->m_context->OMSetRenderTargets(1, &me->m_rtv, NULL);
        me->m_context->ClearRenderTargetView(me->m_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    });

    me->m_swapChain->Present(1, 0);
    return PI_OK;
}

PiResult PI_CALL piimgui_get_preferred_size(void* self_ptr, int32_t* w, int32_t* h)
{
    (void)self_ptr;
    if (w) *w = 400;
    if (h) *h = 300;
    return PI_OK;
}

PiResult PI_CALL piimgui_set_visible(void* self_ptr, int32_t visible)
{
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    if (!me->m_hwnd) return PI_OK;
    ShowWindow(me->m_hwnd, visible ? SW_SHOW : SW_HIDE);
    return PI_OK;
}

const IPiPluginViewVtbl PiImGuiView::s_vtbl = {
    { &piimgui_qi, &pi_refcounted_add_ref, &pi_refcounted_release },
    &piimgui_attach,
    &piimgui_detach,
    &piimgui_get_native_window,
    &piimgui_on_resize,
    &piimgui_on_idle,
    &piimgui_get_preferred_size,
    &piimgui_set_visible
};

static void piimgui_view_destroy(void* self_ptr)
{
    PiImGuiView* me = PiImGuiView::from_iface(self_ptr);
    /* Synchronous model: same thread, no pending work - just tear down
     * whatever is still alive and free the object. */
    if (me->m_attached) {
        me->m_attached = false;
        if (me->m_desc.release)
            me->m_desc.release(me->m_desc.user_data);
    }
    me->destroy_resources();
    delete me;
}

} // namespace

/* ==========================================================================
 * Public API
 * ======================================================================== */

PiResult pi_imgui_view_create(const PiImGuiViewDesc* desc, IPiPluginView** out_view)
{
    if (!desc || !desc->draw || !out_view)
        return PI_E_INVALIDARG;
    *out_view = NULL;

    PiImGuiView* view = new PiImGuiView(*desc);
    if (!view) return PI_E_OUTOFMEMORY;
    pi_refcounted_init_with_destroy(&view->base,
                                    (const IPiUnknownVtbl*)&PiImGuiView::s_vtbl,
                                    &piimgui_view_destroy);
    *out_view = (IPiPluginView*)&view->base;
    return PI_OK;
}
