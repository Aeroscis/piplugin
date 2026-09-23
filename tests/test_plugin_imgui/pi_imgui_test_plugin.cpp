#include "pi_imgui_test_plugin.h"
#include "pi_imgui_view.h"
#include "pi_test_host_service.h"

#include "imgui.h"
#include <stdio.h>

static const PiGuid IMGUI_PLUGIN_CLASS_GUID =
    PI_GUID(0x7F83B200, 0x5C4D, 0x4E2A,
            0x91, 0xD3, 0x8A, 0xFC, 0x2E, 0xB1, 0x44, 0x00);

/* ==========================================================================
 * Factory
 * ======================================================================== */
const IPiPluginFactoryVtbl ImGuiPluginFactory::s_factory_vtbl = {
    { &ImGuiPluginFactory::Qi_Factory, &pi_refcounted_add_ref, &pi_refcounted_release },
    &ImGuiPluginFactory::GetDescriptor,
    &ImGuiPluginFactory::GetClassCount,
    &ImGuiPluginFactory::GetClassGuid,
    &ImGuiPluginFactory::CreateInstance
};

ImGuiPluginFactory::ImGuiPluginFactory()
{
    pi_refcounted_init_with_destroy(&m_base,
                                    (const IPiUnknownVtbl*)&s_factory_vtbl,
                                    &pi_cpp_destroy<ImGuiPluginFactory>);

    m_descriptor.name = "ImGui Test Plugin";
    m_descriptor.vendor = "piplugin";
    m_descriptor.version = "1.0.0";
    m_descriptor.category = "UI/Test";
    m_descriptor.api_version = PI_PLUGIN_API_VERSION;

    m_capabilities[0].iid = PI_IID_PLUGIN_VIEW;
    m_capabilities[0].flags = PI_CAP_PROVIDES;
    m_capabilities[1].iid = PI_IID_HOST_UI;
    m_capabilities[1].flags = PI_CAP_OPTIONAL;

    m_descriptor.capabilities = m_capabilities;
    m_descriptor.capability_count = 2;

    /* 自由元数据（roadmap APP-04）：宿主用 pi_descriptor_find_property() 读。
     * `pi.` 前缀留给框架，app / 插件用自有前缀。 */
    m_properties[0].key   = "com.example.kind";
    m_properties[0].value = "imgui-plugin";
    m_properties[1].key   = "com.example.ui.toolkit";
    m_properties[1].value = "imgui";

    m_descriptor.properties = m_properties;
    m_descriptor.property_count = 2;
}

uint32_t PI_CALL ImGuiPluginFactory::AddRef(void* self_ptr) { return pi_refcounted_add_ref(self_ptr); }
uint32_t PI_CALL ImGuiPluginFactory::Release(void* self_ptr) { return pi_refcounted_release(self_ptr); }
PiResult PI_CALL ImGuiPluginFactory::Qi_Factory(void* self_ptr, const PiGuid* iid, void** out) {
    if (!out) return PI_E_INVALIDARG;
    ImGuiPluginFactory* me = (ImGuiPluginFactory*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_FACTORY)) {
        *out = me; me->m_base.unk.lpVtbl->pi_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}
const PiPluginDescriptor* PI_CALL ImGuiPluginFactory::GetDescriptor(void* self_ptr) {
    return &((ImGuiPluginFactory*)self_ptr)->m_descriptor;
}
uint32_t PI_CALL ImGuiPluginFactory::GetClassCount(void* self_ptr) { (void)self_ptr; return 1; }
PiResult PI_CALL ImGuiPluginFactory::GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid) {
    (void)self_ptr; if (index != 0 || !guid) return PI_E_INVALIDARG; *guid = IMGUI_PLUGIN_CLASS_GUID; return PI_OK;
}
PiResult PI_CALL ImGuiPluginFactory::CreateInstance(void* self_ptr, const PiGuid* guid,
                                                    IPiHostServices* host, IPiPluginBase** out) {
    (void)self_ptr;
    if (!guid || !out) return PI_E_INVALIDARG;
    /* 终审约定 2.4：失败时一律把 *out 置 NULL（调用方不必自带预置）。
     * 这里原本漏了，被 tests/unit 的 ECO-08 负向用例抓出来。 */
    *out = nullptr;
    if (!pi_guid_equal(guid, &IMGUI_PLUGIN_CLASS_GUID)) return PI_E_NOINTERFACE;
    ImGuiPlugin* plugin = new ImGuiPlugin();
    if (!plugin) return PI_E_OUTOFMEMORY;
    PiResult hr = plugin->Initialize(host);
    if (PI_FAILED(hr)) { delete plugin; return hr; }
    *out = (IPiPluginBase*)&plugin->m_base;
    return PI_OK;
}

/* ==========================================================================
 * Plugin
 * ======================================================================== */
const IPiPluginBaseVtbl ImGuiPlugin::s_base_vtbl = {
    { &ImGuiPlugin::Qi_PluginBase, &pi_refcounted_add_ref, &pi_refcounted_release },
    &ImGuiPlugin::Init, &ImGuiPlugin::Term, &ImGuiPlugin::GetView
};

ImGuiPlugin::ImGuiPlugin()
{
    /* m_host / m_hostUI 默认构造即空句柄（PiPtr 的默认构造）。 */
    pi_refcounted_init_with_destroy(&m_base,
                                    (const IPiUnknownVtbl*)&s_base_vtbl,
                                    &ImGuiPlugin::Destroy);
}

ImGuiPlugin::~ImGuiPlugin()
{
    /* PiPtr（pi_cpp.h）负责释放 m_host / m_hostUI，没有手写 release。 */
}

PiResult ImGuiPlugin::Initialize(IPiHostServices* host)
{
    /* 幂等：见 Qt 测试插件里同一处的说明 —— create_instance 里已经初始化过，
     * 宿主随后还会调 pi_initialize，不设闸就会重复 add-ref 并漏掉一份
     * IPiHostUI 包装引用。 */
    if (m_host) return PI_OK;

    if (host) {
        m_host   = PiPtr<IPiHostServices>::add_ref(host);   /* 借用 -> 自己持有 */
        m_hostUI = m_host.qi_to<IPiHostUI>();               /* 失败 = 空句柄 = headless */

        /* 通道 B（roadmap APP-01），与 Qt 测试插件逐字同构：对宿主对象 QI
         * 一次，拿到了就用，拿不到照常跑（descriptor 里该能力是 OPTIONAL）。 */
        PiPtr<IPiTestHostService> svc =
            m_host.qi_to<IPiTestHostService>(PI_TEST_IID_HOST_SERVICE);
        if (svc) {
            pi_host_post_message(m_host.get(), PI_TEST_MSG_HOST_SERVICE, 1u, 0);
            printf("[imgui plugin] host-provided service: host=%s, "
                   "the host has seen %u plugin message(s)\n",
                   pi_test_host_service_name(svc.get()),
                   (unsigned)pi_test_host_service_messages_seen(svc.get()));
        } else {
            printf("[imgui plugin] host provides no app-defined service "
                   "(framework services only)\n");
        }
    }
    return PI_OK;
}

PiResult ImGuiPlugin::Terminate()
{
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * ImGui adapter kit callbacks - pure UI code, no host knowledge.
 * ------------------------------------------------------------------------ */
void ImGuiPlugin::SetupUi(void* user_data)
{
    (void)user_data;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]        = ImVec4(0.13f, 0.14f, 0.17f, 1.0f);
    colors[ImGuiCol_TitleBgActive]   = ImVec4(0.20f, 0.35f, 0.60f, 1.0f);
    colors[ImGuiCol_Button]          = ImVec4(0.20f, 0.35f, 0.60f, 1.0f);
    colors[ImGuiCol_ButtonHovered]   = ImVec4(0.28f, 0.48f, 0.80f, 1.0f);
    colors[ImGuiCol_SliderGrab]      = ImVec4(0.28f, 0.48f, 0.80f, 1.0f);
}

void ImGuiPlugin::DrawUi(void* user_data)
{
    ImGuiPlugin* me = (ImGuiPlugin*)user_data;

    /* Main window fills the embed area. */
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##plugin_root", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    ImGui::TextUnformatted("ImGui Plugin - running inside the Qt host!");
    ImGui::Separator();

    static int sliderValue = 50;
    if (ImGui::SliderInt("Value", &sliderValue, 0, 100)) {
        if (me->m_host)
            pi_host_post_message(me->m_host.get(), 0x1000, (uintptr_t)sliderValue, 0);
    }
    if (ImGui::Button("Reset"))
        sliderValue = 50;

    ImGui::Text("Application average %.1f ms/frame (%.1f FPS)",
                1000.0f / io.Framerate, io.Framerate);

    ImGui::End();

    /* Floating heartbeat window (proof that frames are continuously
     * driven by the host's pi_on_idle). */
    static int tick = 0;
    tick++;
    int p = tick % 100, r = 0, g = 0, b = 0;
    if (p < 33)      { r = 255 - p*7;  g = p*7;        b = 0; }
    else if (p < 66) { r = 0;          g = 255-(p-33)*7; b = (p-33)*7; }
    else             { r = (p-66)*7;   g = 0;          b = 255-(p-66)*7; }
    ImGui::SetNextWindowPos(ImVec2(24, 64), ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(r/255.0f, g/255.0f, b/255.0f, 0.85f));
    ImGui::Begin("ImGui Heartbeat", NULL, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("ImGui frame: tick %d", tick);
    ImGui::End();
    ImGui::PopStyleColor();
}

void ImGuiPlugin::Retain(void* user_data)
{
    ImGuiPlugin* me = (ImGuiPlugin*)user_data;
    if (me) me->m_base.unk.lpVtbl->pi_add_ref(me);
}

void ImGuiPlugin::Release(void* user_data)
{
    ImGuiPlugin* me = (ImGuiPlugin*)user_data;
    if (me) me->m_base.unk.lpVtbl->pi_release(me);
}

PiResult PI_CALL ImGuiPlugin::Qi_PluginBase(void* self_ptr, const PiGuid* iid, void** out) {
    if (!out) return PI_E_INVALIDARG;
    ImGuiPlugin* me = (ImGuiPlugin*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_BASE)) {
        *out = me; me->m_base.unk.lpVtbl->pi_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}
PiResult PI_CALL ImGuiPlugin::Init(void* self_ptr, IPiHostServices* host) {
    return ((ImGuiPlugin*)self_ptr)->Initialize(host);
}
PiResult PI_CALL ImGuiPlugin::Term(void* self_ptr) {
    return ((ImGuiPlugin*)self_ptr)->Terminate();
}
PiResult PI_CALL ImGuiPlugin::GetView(void* self_ptr, IPiPluginView** out) {
    if (!out) return PI_E_INVALIDARG;
    ImGuiPlugin* me = (ImGuiPlugin*)self_ptr;
    if (!me->m_hostUI) { *out = NULL; return PI_E_NOINTERFACE; }

    PiImGuiViewDesc desc = {};
    desc.init      = &ImGuiPlugin::SetupUi;
    desc.draw      = &ImGuiPlugin::DrawUi;
    desc.retain    = &ImGuiPlugin::Retain;
    desc.release   = &ImGuiPlugin::Release;
    desc.user_data = me;
    return pi_imgui_view_create(&desc, out);
}
