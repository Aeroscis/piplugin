/*
 * piplugin example - a minimal imgui plugin
 *
 * The whole plugin: a descriptor, a factory, and ONE draw callback. Everything
 * about embedding, contexts and event pumping lives in the imgui adapter kit
 * (piplugin_imgui), so the plugin never touches a window handle.
 *
 * It advertises PI_PLUGIN_IID_PLUGIN_VIEW (PROVIDES) and PI_PLUGIN_IID_HOST_UI (OPTIONAL):
 * a headless host loads it fine and simply never gets a view.
 *
 * Run it with examples/minimal_host:
 *     pi_plugin_example_minimal_host.exe pi_plugin_example_plugin_imgui.dll
 */
#include "piplugin/pi_plugin.h"
#include "pi_imgui_view.h"

#include "imgui.h"

#include <stdio.h>

static const PiGuid EXAMPLE_CLASS_GUID =
    PI_GUID(0x2C41F8B6, 0x7D05, 0x4E63, 0xA8, 0x19, 0x64, 0xF2, 0x38, 0x0B, 0xC7, 0x51);

class ExamplePlugin {
public:
    ExamplePlugin()
        : m_counter(0), m_slider(50), m_host(nullptr), m_hostUI(nullptr)
    {
        pi_refcounted_init_with_destroy(&m_base, (const IPiUnknownVtbl*)&s_vtbl, &Destroy);
    }

    ~ExamplePlugin()
    {
        if (m_hostUI) { pi_iunknown_release((IPiUnknown*)m_hostUI); m_hostUI = nullptr; }
        if (m_host)   { pi_iunknown_release((IPiUnknown*)m_host);   m_host = nullptr; }
    }

    PiResult Initialize(IPiPluginHostServices* host)
    {
        if (m_host) return PI_OK;                 /* idempotent, see write-plugin.md */
        if (!host) return PI_OK;

        /* The host pointer is borrowed for this call: keep a reference, and ask
         * whether it is a GUI host (absent IPiPluginHostUI = headless). */
        m_host = host;
        pi_iunknown_add_ref((IPiUnknown*)host);
        IPiPluginHostUI* ui = nullptr;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host, &PI_PLUGIN_IID_HOST_UI,
                                                     (void**)&ui))) {
            m_hostUI = ui;
        }
        return PI_OK;
    }

    PiResult GetView(IPiPluginView** out)
    {
        if (!out) return PI_E_INVALIDARG;
        if (!m_hostUI) { *out = nullptr; return PI_E_NOINTERFACE; }   /* headless host */

        PiPluginImGuiViewDesc desc = {};
        desc.init      = &SetupUi;
        desc.draw      = &DrawUi;
        desc.retain    = &Retain;
        desc.release   = &Release;
        desc.user_data = this;
        return pi_plugin_imgui_view_create(&desc, out);
    }

    /* ---- adapter callbacks (host GUI thread) ---- */
    static void SetupUi(void* user_data)
    {
        (void)user_data;
        ImGui::GetStyle().WindowRounding = 4.0f;
    }

    static void DrawUi(void* user_data)
    {
        ExamplePlugin* me = (ExamplePlugin*)user_data;
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##example", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("Hello from a piplugin example plugin!");
        ImGui::Separator();
        ImGui::SliderInt("Value", &me->m_slider, 0, 100);
        if (ImGui::Button("Send to host")) {
            ++me->m_counter;
            /* Talking to the host is one call; the host decides what it means. */
            if (me->m_host)
                pi_plugin_host_post_message(me->m_host, 0x8000u, (uintptr_t)me->m_counter, 0);
        }
        ImGui::Text("button pressed %d time(s)", me->m_counter);
        ImGui::End();
    }

    static void Retain(void* user_data)
    {
        ExamplePlugin* me = (ExamplePlugin*)user_data;
        if (me) me->m_base.unk.lpVtbl->pi_add_ref(me);
    }

    static void Release(void* user_data)
    {
        ExamplePlugin* me = (ExamplePlugin*)user_data;
        if (me) me->m_base.unk.lpVtbl->pi_release(me);
    }

public:
    PiRefCountedBase m_base;      /* MUST be first data member */

private:
    static void Destroy(void* self) { delete static_cast<ExamplePlugin*>(self); }

    static PiResult PI_CALL Qi(void* self_ptr, const PiGuid* iid, void** out)
    {
        ExamplePlugin* me = (ExamplePlugin*)self_ptr;
        if (!out) return PI_E_INVALIDARG;
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE)) {
            *out = me; me->m_base.unk.lpVtbl->pi_add_ref(self_ptr); return PI_OK;
        }
        *out = nullptr; return PI_E_NOINTERFACE;
    }
    static PiResult PI_CALL Init(void* self_ptr, IPiPluginHostServices* host)
    {
        return ((ExamplePlugin*)self_ptr)->Initialize(host);
    }
    static PiResult PI_CALL Term(void* self_ptr) { (void)self_ptr; return PI_OK; }
    static PiResult PI_CALL GetViewThunk(void* self_ptr, IPiPluginView** out)
    {
        return ((ExamplePlugin*)self_ptr)->GetView(out);
    }

    static const IPiPluginBaseVtbl s_vtbl;

    int              m_counter;
    int              m_slider;
    IPiPluginHostServices* m_host;      /* add-ref'd */
    IPiPluginHostUI*       m_hostUI;    /* add-ref'd, NULL on a headless host */
};

const IPiPluginBaseVtbl ExamplePlugin::s_vtbl = {
    { &ExamplePlugin::Qi, &pi_refcounted_add_ref, &pi_refcounted_release },
    &ExamplePlugin::Init, &ExamplePlugin::Term, &ExamplePlugin::GetViewThunk
};

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */
class ExampleFactory {
public:
    ExampleFactory()
    {
        pi_refcounted_init_with_destroy(&m_base, (const IPiUnknownVtbl*)&s_vtbl, &Destroy);

        /* Fill the descriptor in ZEROED: it has optional fields that get appended
         * over time, and this one lives on the heap (so it starts as 0xCDCDCDCD).
         * See pi_plugin_descriptor_init() in pi_plugin_types.h. */
        pi_plugin_descriptor_init(&m_desc);

        m_caps[0].iid = PI_PLUGIN_IID_PLUGIN_VIEW; m_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;
        m_caps[1].iid = PI_PLUGIN_IID_HOST_UI;     m_caps[1].flags = PI_PLUGIN_CAP_OPTIONAL;

        m_desc.name = "Example ImGui Plugin";
        m_desc.vendor = "piplugin examples";
        m_desc.version = "1.0.0";
        m_desc.category = "Example/UI";
        m_desc.api_version = PI_PLUGIN_API_VERSION;
        m_desc.capabilities = m_caps;
        m_desc.capability_count = 2;
    }

    static PiResult PI_CALL Qi(void* self_ptr, const PiGuid* iid, void** out)
    {
        ExampleFactory* me = (ExampleFactory*)self_ptr;
        if (!out) return PI_E_INVALIDARG;
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY)) {
            *out = me; me->m_base.unk.lpVtbl->pi_add_ref(self_ptr); return PI_OK;
        }
        *out = nullptr; return PI_E_NOINTERFACE;
    }
    static uint32_t PI_CALL AddRef(void* self) { return pi_refcounted_add_ref(self); }
    static uint32_t PI_CALL Release(void* self) { return pi_refcounted_release(self); }
    static const PiPluginDescriptor* PI_CALL GetDescriptor(void* self)
    {
        return &((ExampleFactory*)self)->m_desc;
    }
    static uint32_t PI_CALL GetClassCount(void* self) { (void)self; return 1; }
    static PiResult PI_CALL GetClassGuid(void* self, uint32_t index, PiGuid* guid)
    {
        (void)self;
        if (index != 0 || !guid) return PI_E_INVALIDARG;
        *guid = EXAMPLE_CLASS_GUID;
        return PI_OK;
    }
    static PiResult PI_CALL CreateInstance(void* self, const PiGuid* guid,
                                           IPiPluginHostServices* host, IPiPluginBase** out)
    {
        (void)self;
        if (!guid || !out) return PI_E_INVALIDARG;
        *out = nullptr;
        if (!pi_guid_equal(guid, &EXAMPLE_CLASS_GUID)) return PI_E_NOINTERFACE;

        ExamplePlugin* plugin = new ExamplePlugin();
        if (!plugin) return PI_E_OUTOFMEMORY;
        PiResult hr = plugin->Initialize(host);
        if (PI_FAILED(hr)) { delete plugin; return hr; }
        *out = (IPiPluginBase*)&plugin->m_base;
        return PI_OK;
    }

public:
    PiRefCountedBase      m_base;      /* MUST be first data member */

private:
    static void Destroy(void* self) { delete static_cast<ExampleFactory*>(self); }

    static const IPiPluginFactoryVtbl s_vtbl;
    PiPluginDescriptor    m_desc;
    PiPluginCapability    m_caps[2];
};

const IPiPluginFactoryVtbl ExampleFactory::s_vtbl = {
    { &ExampleFactory::Qi, &ExampleFactory::AddRef, &ExampleFactory::Release },
    &ExampleFactory::GetDescriptor, &ExampleFactory::GetClassCount,
    &ExampleFactory::GetClassGuid, &ExampleFactory::CreateInstance
};

PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory) return PI_E_INVALIDARG;
    *out_factory = (IPiPluginFactory*)&(new ExampleFactory())->m_base;
    return PI_OK;
}
