/*
 * piplugin - Host kit L1: DX11 嵌入胶水（实现）
 *
 * 这里搬的是 tests/test_host 里那段"能正确嵌入子窗口的 D3D11 设备 + flip-model
 * 交换链"的创建与 resize 策略，逐条保持原行为（含日志文本，脚本按模式断言）。
 * 为什么不变量、哪些坑对应哪行代码，见 pi_host_dx11.h 的说明。
 */
#include "pi_host_dx11.h"

#include <windows.h>
#include <dxgi1_3.h>   /* IDXGIFactory2 / DXGI_SWAP_CHAIN_DESC1 / IDXGISwapChain2 */
#include <objbase.h>   /* IID_PPV_ARGS */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

struct PiPluginHostDx11Device {
    ID3D11Device*           device;
    ID3D11DeviceContext*    context;
    IDXGISwapChain*         swap_chain;
    ID3D11RenderTargetView* render_target;
    /* 由 DXGI 在可以排队下一帧时置位。句柄归本层持有，宿主按需取用。 */
    HANDLE                  frame_latency_waitable;
    /* 创建交换链时用的 flags：ResizeBuffers 必须传回同一组值，否则 E_INVALIDARG。 */
    UINT                    swap_chain_flags;
    unsigned                swap_w, swap_h;
    int                     scaling_none;
    int                     flip_model;
    const char*             present_model;
    unsigned                resize_failures;
    float                   background[4];
    PiPluginHostDx11LogProc       log;
    void*                   log_user;
};

/* --------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------- */
static void Dx11Log(PiPluginHostDx11Device* dx, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!dx || !dx->log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dx->log(dx->log_user, buf);
}

/* --------------------------------------------------------------------------
 * Render target
 * -------------------------------------------------------------------------- */
static void Dx11CleanupRenderTarget(PiPluginHostDx11Device* dx)
{
    if (dx->render_target) {
        dx->render_target->Release();
        dx->render_target = NULL;
    }
}

static void Dx11CreateRenderTarget(PiPluginHostDx11Device* dx)
{
    ID3D11Texture2D* back_buffer = NULL;
    if (!dx->swap_chain || !dx->device) return;
    dx->swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        dx->device->CreateRenderTargetView(back_buffer, NULL, &dx->render_target);
        back_buffer->Release();
    }
}

/* --------------------------------------------------------------------------
 * Resize policy
 * -------------------------------------------------------------------------- */

/* 只增不减：整条修复的地基是"拖拽期间缓冲永不小于窗口"（陈旧帧只会被
 * SCALING_NONE 裁剪）。所以增长是拖拽期间唯一可能发生的 resize，且一次拖拽最多一次。
 * 返回非 0 表示渲染目标可用。 */
static int Dx11EnsureAtLeast(PiPluginHostDx11Device* dx, unsigned w, unsigned h)
{
    unsigned nw, nh;
    HRESULT hr;

    if (!dx->swap_chain) return 0;
    if (w <= dx->swap_w && h <= dx->swap_h)
        return dx->render_target != NULL;

    nw = (w > dx->swap_w) ? w : dx->swap_w;
    nh = (h > dx->swap_h) ? h : dx->swap_h;

    Dx11CleanupRenderTarget(dx);
    hr = dx->swap_chain->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, dx->swap_chain_flags);
    if (FAILED(hr)) {
        if (dx->resize_failures == 0)
            Dx11Log(dx, "resize: ResizeBuffers(%ux%u) FAILED hr=0x%08X", nw, nh, (unsigned)hr);
        ++dx->resize_failures;
        hr = dx->swap_chain->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, dx->swap_chain_flags);
        if (FAILED(hr)) {
            Dx11Log(dx, "resize: retry FAILED hr=0x%08X", (unsigned)hr);
            Dx11CreateRenderTarget(dx);
            return dx->render_target != NULL;
        }
    }
    Dx11CreateRenderTarget(dx);
    dx->swap_w = nw;
    dx->swap_h = nh;
    Dx11Log(dx, "resize: swap chain grows to %ux%u (client was %ux%u)", nw, nh, w, h);
    return 1;
}

/* 精确跟随：SCALING_NONE 拿不到时的旧行为 —— 缓冲必须与窗口严格一致，
 * 否则会被拉伸。 */
static int Dx11ResizeExact(PiPluginHostDx11Device* dx, unsigned w, unsigned h)
{
    HRESULT hr;

    Dx11CleanupRenderTarget(dx);
    hr = dx->swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, dx->swap_chain_flags);
    if (FAILED(hr)) {
        if (dx->resize_failures == 0)
            Dx11Log(dx, "resize: ResizeBuffers(%ux%u) FAILED hr=0x%08X", w, h, (unsigned)hr);
        ++dx->resize_failures;
        hr = dx->swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, dx->swap_chain_flags);
        if (FAILED(hr)) {
            Dx11Log(dx, "resize: retry FAILED hr=0x%08X", (unsigned)hr);
            Dx11CreateRenderTarget(dx);
            return dx->render_target != NULL;
        }
    }
    Dx11CreateRenderTarget(dx);
    return dx->render_target != NULL;
}

int pi_plugin_host_dx11_prepare_size(PiPluginHostDx11Device* dx, unsigned width, unsigned height)
{
    if (!dx || !dx->swap_chain || !dx->context || width == 0 || height == 0) return 0;
    if (dx->scaling_none) return Dx11EnsureAtLeast(dx, width, height);
    return Dx11ResizeExact(dx, width, height);
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
PiResult pi_plugin_host_dx11_create(PiNativeWindow hwnd, const PiPluginHostDx11Desc* desc,
                             PiPluginHostDx11Device** out_device)
{
    HWND hWnd = (HWND)hwnd;
    PiPluginHostDx11Device* dx;
    const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr;

    if (!hwnd || !out_device) return PI_E_INVALIDARG;
    *out_device = NULL;

    dx = (PiPluginHostDx11Device*)calloc(1, sizeof(*dx));
    if (!dx) return PI_E_OUTOFMEMORY;

    dx->present_model = "unknown";
    if (desc) {
        dx->log      = desc->log;
        dx->log_user = desc->log_user_data;
        dx->background[0] = desc->background[0];
        dx->background[1] = desc->background[1];
        dx->background[2] = desc->background[2];
        dx->background[3] = desc->background[3];
    } else {
        dx->background[0] = 0.15f;
        dx->background[1] = 0.15f;
        dx->background[2] = 0.15f;
        dx->background[3] = 1.0f;
    }

    /* 设备：硬件优先，失败退 WARP */
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                           featureLevels, 2, D3D11_SDK_VERSION,
                           &dx->device, &featureLevel, &dx->context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                               featureLevels, 2, D3D11_SDK_VERSION,
                               &dx->device, &featureLevel, &dx->context);
    }
    if (FAILED(hr)) {
        Dx11Log(dx, "d3d: device creation failed hr=0x%08X", (unsigned)hr);
        pi_plugin_host_dx11_destroy(dx);
        return PI_E_UNEXPECTED;
    }

    /* 交换链必须显式走 CreateSwapChainForHwnd：只有这个调用接受
     * DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT。 */
    {
        IDXGIDevice*   dxgiDevice = NULL;
        IDXGIAdapter*  adapter    = NULL;
        IDXGIFactory2* factory    = NULL;

        if (SUCCEEDED(dx->device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            DXGI_SWAP_CHAIN_DESC1 sd = {};
            IDXGISwapChain1* swap_chain1 = NULL;

            sd.Width  = 0;
            sd.Height = 0;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            /* 三个缓冲：拖拽每步 Present 一次，配合下面的等待对象，多一个缓冲不花钱 */
            sd.BufferCount = 3;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            /* 修复的核心：缓冲与窗口尺寸不一致时 1:1 左上裁剪，绝不重缩放 */
            sd.Scaling = DXGI_SCALING_NONE;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            dx->swap_chain_flags = sd.Flags;

            hr = factory->CreateSwapChainForHwnd(dx->device, hWnd, &sd, NULL, NULL, &swap_chain1);
            if (FAILED(hr) && sd.Scaling == DXGI_SCALING_NONE) {
                /* 防御：Win8+flip 上不该发生，但宁可降级到"拉伸"也不要没有交换链 */
                Dx11Log(dx, "d3d: SCALING_NONE rejected hr=0x%08X - falling back to STRETCH",
                        (unsigned)hr);
                sd.Scaling = DXGI_SCALING_STRETCH;
                hr = factory->CreateSwapChainForHwnd(dx->device, hWnd, &sd, NULL, NULL, &swap_chain1);
            }

            if (SUCCEEDED(hr) && swap_chain1) {
                /* 只保留基接口：其余代码都用 IDXGISwapChain */
                swap_chain1->QueryInterface(IID_PPV_ARGS(&dx->swap_chain));
                factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
                dx->flip_model    = 1;
                dx->scaling_none  = (sd.Scaling == DXGI_SCALING_NONE) ? 1 : 0;
                dx->present_model = dx->scaling_none ? "FLIP_DISCARD+latency+scale:none"
                                                     : "FLIP_DISCARD+latency+scale:stretch";
                /* "窗口大于缓冲"那一帧（只增不减策略下可能出现）露出的部分
                 * 用与清屏色相同的颜色填充 */
                {
                    const DXGI_RGBA bg = { dx->background[0], dx->background[1],
                                           dx->background[2], dx->background[3] };
                    swap_chain1->SetBackgroundColor(&bg);
                }
                /* Width/Height 传的是 0（"跟随窗口"）：记录它解析成了多少，
                 * 这是"只增不减"的下限 */
                {
                    DXGI_SWAP_CHAIN_DESC1 got = {};
                    if (SUCCEEDED(swap_chain1->GetDesc1(&got))) {
                        dx->swap_w = got.Width;
                        dx->swap_h = got.Height;
                    }
                }
                {
                    IDXGISwapChain2* sc2 = NULL;
                    if (SUCCEEDED(swap_chain1->QueryInterface(IID_PPV_ARGS(&sc2)))) {
                        sc2->SetMaximumFrameLatency(1);
                        dx->frame_latency_waitable = sc2->GetFrameLatencyWaitableObject();
                        sc2->Release();
                    }
                }
                Dx11Log(dx, "d3d: swap chain %s buffer=%ux%u, latency-waitable=%s",
                        dx->present_model, dx->swap_w, dx->swap_h,
                        dx->frame_latency_waitable ? "yes" : "NO");
                swap_chain1->Release();
            }
        }
        if (dxgiDevice) dxgiDevice->Release();
        if (adapter)    adapter->Release();
        if (factory)    factory->Release();
    }

    if (!dx->swap_chain) {
        /* 系统拿不到 flip model：退到老的 bitblt 路径。此时内嵌子窗口只在
         * blitter 没盖到的位置可见 —— 能用，但不是正确嵌入。 */
        if (dx->context) { dx->context->Release(); dx->context = NULL; }
        if (dx->device)  { dx->device->Release();  dx->device = NULL; }

        {
            DXGI_SWAP_CHAIN_DESC sd = {};
            sd.BufferCount = 3;
            sd.BufferDesc.Width = 0;
            sd.BufferDesc.Height = 0;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferDesc.RefreshRate.Numerator = 60;
            sd.BufferDesc.RefreshRate.Denominator = 1;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            dx->swap_chain_flags = sd.Flags;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hWnd;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                               featureLevels, 2, D3D11_SDK_VERSION,
                                               &sd, &dx->swap_chain, &dx->device,
                                               &featureLevel, &dx->context);
            if (FAILED(hr) || !dx->swap_chain) {
                Dx11Log(dx, "d3d: no swap chain available hr=0x%08X", (unsigned)hr);
                pi_plugin_host_dx11_destroy(dx);
                return PI_E_UNEXPECTED;
            }
        }
        dx->flip_model    = 0;
        dx->scaling_none  = 0;
        dx->present_model = "DISCARD(bitblt)";
        {
            DXGI_SWAP_CHAIN_DESC gotDesc = {};
            if (SUCCEEDED(dx->swap_chain->GetDesc(&gotDesc))) {
                dx->swap_w = gotDesc.BufferDesc.Width;
                dx->swap_h = gotDesc.BufferDesc.Height;
            }
        }
    }

    Dx11CreateRenderTarget(dx);
    if (!dx->render_target)
        Dx11Log(dx, "d3d: render target unavailable at startup");

    *out_device = dx;
    return PI_OK;
}

void pi_plugin_host_dx11_destroy(PiPluginHostDx11Device* dx)
{
    if (!dx) return;

    Dx11CleanupRenderTarget(dx);
    if (dx->frame_latency_waitable) {
        CloseHandle(dx->frame_latency_waitable);
        dx->frame_latency_waitable = NULL;
    }
    if (dx->swap_chain)     { dx->swap_chain->Release();     dx->swap_chain = NULL; }
    if (dx->context)        { dx->context->Release();        dx->context = NULL; }
    if (dx->device)         { dx->device->Release();         dx->device = NULL; }
    free(dx);
}

/* --------------------------------------------------------------------------
 * Accessors
 * -------------------------------------------------------------------------- */
ID3D11Device*           pi_plugin_host_dx11_device(PiPluginHostDx11Device* dx)         { return dx ? dx->device : NULL; }
ID3D11DeviceContext*    pi_plugin_host_dx11_context(PiPluginHostDx11Device* dx)        { return dx ? dx->context : NULL; }
IDXGISwapChain*         pi_plugin_host_dx11_swap_chain(PiPluginHostDx11Device* dx)     { return dx ? dx->swap_chain : NULL; }
ID3D11RenderTargetView* pi_plugin_host_dx11_render_target(PiPluginHostDx11Device* dx)  { return dx ? dx->render_target : NULL; }
const char*             pi_plugin_host_dx11_present_model(PiPluginHostDx11Device* dx)  { return (dx && dx->present_model) ? dx->present_model : "unknown"; }
int                     pi_plugin_host_dx11_is_flip_model(PiPluginHostDx11Device* dx)  { return dx ? dx->flip_model : 0; }
int                     pi_plugin_host_dx11_has_scaling_none(PiPluginHostDx11Device* dx) { return dx ? dx->scaling_none : 0; }
unsigned                pi_plugin_host_dx11_resize_failures(PiPluginHostDx11Device* dx) { return dx ? dx->resize_failures : 0u; }

void pi_plugin_host_dx11_reset_resize_stats(PiPluginHostDx11Device* dx)
{
    if (dx) dx->resize_failures = 0;
}

void pi_plugin_host_dx11_present(PiPluginHostDx11Device* dx, unsigned sync_interval)
{
    if (!dx || !dx->swap_chain) return;
    dx->swap_chain->Present(sync_interval, 0);
}

/* --------------------------------------------------------------------------
 * Container / window style
 * -------------------------------------------------------------------------- */
unsigned long pi_plugin_host_dx11_top_level_style(void)
{
    /* WS_CLIPCHILDREN：没有它，宿主自己的绘制区域包含内嵌插件窗口，
     * 于是每次 Present 都会擦掉插件的像素。 */
    return (unsigned long)WS_CLIPCHILDREN;
}

PiNativeWindow pi_plugin_host_dx11_create_embed_container(PiNativeWindow parent,
                                                   int x, int y, int width, int height)
{
    HWND child;
    if (!parent) return PI_INVALID_WINDOW;

    child = CreateWindowExW(
        0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, width, height,
        (HWND)parent, NULL, GetModuleHandle(NULL), NULL);
    return (PiNativeWindow)child;
}
