/*
 * piplugin - Host kit L1: DX11 嵌入胶水 (piplugin_host_dx11)
 *
 * L1 的职责（见 host_kits/README.md「三层结构」）：把**宿主自己创建的**容器变成 embed host，
 * 并把"能正确嵌入子窗口的 D3D11 设备 + flip-model 交换链"的创建参数固化下来。
 *
 * 本层只固化知识，不占有决策权：
 *   - 窗口由宿主创建、宿主摆位，本层不创建顶层窗口；
 *   - 容器矩形由宿主给定（pi_plugin_host_dx11_create_embed_container 只负责
 *     "以正确的方式成为 embed host"：子窗口风格 + WS_CLIPCHILDREN/WS_CLIPSIBLINGS）；
 *   - 画什么、什么时候画，全在宿主（本层不调用任何渲染代码）。
 *
 * 固化了哪些坑（都是 docs/design/d3d-window-resizing.md 里那些）：
 *   1. 交换链必须是 flip model（FLIP_DISCARD）：DWM 把 flip 链当作窗口内容合成，
 *      别的模块创建的宿主子窗口（内嵌插件）才能叠在上面；老的 bitblt 模型会被
 *      GDI 直接糊过去 —— 就是"插件看不见"那个症状。
 *   2. DXGI_SCALING_NONE：缓冲与窗口尺寸不一致时 1:1 左上对齐裁剪，而不是被 DWM
 *      拉伸 —— 拖拽期间陈旧帧只会被裁剪，面板形状永不变化。驱动拒绝时降级 STRETCH，
 *      再不行降级 bitblt 老路径（保证永远有交换链可用）。
 *   3. 缓冲"只增不减"：尺寸循环期间不需要每步 ResizeBuffers，一次拖拽最多增长一次。
 *      该策略只在 SCALING_NONE 成立时启用；否则退回"精确跟随"的旧行为。
 *   4. ResizeBuffers 必须传回创建时的 flags（含 FRAME_LATENCY_WAITABLE_OBJECT），
 *      否则 E_INVALIDARG。
 *   5. FRAME_LATENCY_WAITABLE_OBJECT + SetMaximumFrameLatency(1)：让宿主能把拖拽
 *      步进与合成器对齐，从而 Present() 不阻塞。句柄由宿主按需取用。
 *   6. SetBackgroundColor = 宿主清屏色：窗口大于缓冲的那一帧露出的边角与画面同色。
 *   7. 宿主自己的顶层窗口必须带 WS_CLIPCHILDREN（用 pi_plugin_host_dx11_top_level_style()），
 *      否则每次 Present 都会擦掉内嵌插件的像素。
 */
#ifndef PI_PLUGIN_HOST_DX11_H
#define PI_PLUGIN_HOST_DX11_H

#include <d3d11.h>
#include <dxgi.h>

#include "piplugin/pi_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PiPluginHostDx11Device PiPluginHostDx11Device;

/* 日志回调：本层把交换链创建与 resize 的关键事实经此上报，去向由宿主决定
 * （测试宿主写文件，脚本按日志模式断言）。 */
typedef void (*PiPluginHostDx11LogProc)(void* user_data, char const* message);

typedef struct PiPluginHostDx11Desc {
    /* 窗口大于后台缓冲时（只增不减策略下可能有一帧如此）露出的填充色。
     * 宿主应传自己的清屏色，否则那一帧的边角会露出不同颜色。 */
    float background[4];

    /* 可空：创建过程中的日志（含 SCALING_NONE 降级）走这里 */
    PiPluginHostDx11LogProc log;
    void*                   log_user_data;
} PiPluginHostDx11Desc;

/* --------------------------------------------------------------------------
 * 生命周期
 * -------------------------------------------------------------------------- */

/* 为宿主自己的窗口 hwnd 创建"可嵌入子窗口"的 D3D11 设备 + flip-model 交换链。
 * 失败时 *out_device 为 NULL。 */
PiResult pi_plugin_host_dx11_create(PiNativeWindow hwnd, PiPluginHostDx11Desc const* desc,
                                    PiPluginHostDx11Device** out_device);

/* 释放设备 / 交换链 / 渲染目标 / 等待对象。NULL 安全。 */
void pi_plugin_host_dx11_destroy(PiPluginHostDx11Device* dx);

/* --------------------------------------------------------------------------
 * 宿主渲染时要用到的东西（借用，所有权在本层）
 * -------------------------------------------------------------------------- */
ID3D11Device*           pi_plugin_host_dx11_device(PiPluginHostDx11Device* dx);
ID3D11DeviceContext*    pi_plugin_host_dx11_context(PiPluginHostDx11Device* dx);
IDXGISwapChain*         pi_plugin_host_dx11_swap_chain(PiPluginHostDx11Device* dx);
ID3D11RenderTargetView* pi_plugin_host_dx11_render_target(PiPluginHostDx11Device* dx);

/* 实际拿到的呈现模型（诊断用）。present_model 形如
 * "FLIP_DISCARD+latency+scale:none" / "...scale:stretch" / "DISCARD(bitblt)"。 */
char const* pi_plugin_host_dx11_present_model(PiPluginHostDx11Device* dx);
int         pi_plugin_host_dx11_is_flip_model(PiPluginHostDx11Device* dx);
int         pi_plugin_host_dx11_has_scaling_none(PiPluginHostDx11Device* dx);

/* --------------------------------------------------------------------------
 * resize 转发（策略在本层；"容器想变成多大"由宿主决定）
 * -------------------------------------------------------------------------- */

/* 把后台缓冲调整到适配 width x height：
 *   - SCALING_NONE：只增不减（拖拽期间缓冲永不小于窗口，这是嵌入正确性的地基）
 *   - 否则：精确跟随
 * 返回非 0 表示渲染目标可用（宿主可以继续画这一帧）。 */
int pi_plugin_host_dx11_prepare_size(PiPluginHostDx11Device* dx, unsigned width, unsigned height);

/* 呈现一帧。sync_interval 传 0 用于尺寸循环（尽早入队，配合 SCALING_NONE
 * 让画面上的陈旧帧只会被裁剪）；常规帧传 1。 */
void pi_plugin_host_dx11_present(PiPluginHostDx11Device* dx, unsigned sync_interval);

/* resize 失败计数（诊断）：宿主在自己的拖拽结束日志里报出来。 */
unsigned pi_plugin_host_dx11_resize_failures(PiPluginHostDx11Device* dx);
void     pi_plugin_host_dx11_reset_resize_stats(PiPluginHostDx11Device* dx);

/* --------------------------------------------------------------------------
 * 容器与窗口风格（宿主给矩形与父窗口，本层给"正确的做法"）
 * -------------------------------------------------------------------------- */

/* 宿主创建自己的顶层窗口时 OR 上本返回值（WS_CLIPCHILDREN）。 */
unsigned long pi_plugin_host_dx11_top_level_style(void);

/* 创建一个适合承载插件子窗口的容器窗口（WS_CHILD|WS_VISIBLE|
 * WS_CLIPCHILDREN|WS_CLIPSIBLINGS）。位置与大小完全由宿主给定。
 * 失败返回 PI_INVALID_WINDOW。 */
PiNativeWindow pi_plugin_host_dx11_create_embed_container(PiNativeWindow parent,
                                                          int x, int y, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_HOST_DX11_H */
