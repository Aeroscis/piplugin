/*
 * piplugin example - a minimal Qt plugin
 *
 * Same shape as the imgui example: descriptor + factory + one widget factory.
 * The Qt adapter kit owns the QApplication, the embedding and the event pumping,
 * so this file contains only UI code.
 *
 * The kit is a SHARED library (roadmap APP-08): running the built plugin needs
 * piplugin_qt<debug-suffix>.dll next to it (the build deploys it into bin/<CONFIG>).
 *
 * Run it with examples/minimal_host:
 *     pi_plugin_example_minimal_host.exe pi_plugin_example_plugin_qt.dll
 */
#include <stdio.h>
#include <string.h>

#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include "pi_qt_view.h"
#include "piplugin/pi_plugin.h"

static PiGuid const EXAMPLE_CLASS_GUID =
    PI_GUID(0x5D18A3F4, 0x2E60, 0x4B79, 0x93, 0x47, 0x0A, 0xD8, 0x61, 0x2C, 0xBE, 0x05);

class ExampleQtPlugin
{
public:
    ExampleQtPlugin()
        : m_clicks(0)
        , m_host(nullptr)
        , m_hostUI(nullptr)
    {
        pi_refcounted_init_with_destroy(&m_base, (IPiUnknownVtbl const*)&s_vtbl, &Destroy);
    }

    ~ExampleQtPlugin()
    {
        if (m_hostUI)
        {
            pi_iunknown_release((IPiUnknown*)m_hostUI);
            m_hostUI = nullptr;
        }
        if (m_host)
        {
            pi_iunknown_release((IPiUnknown*)m_host);
            m_host = nullptr;
        }
    }

    PiResult Initialize(IPiPluginHostServices* host)
    {
        if (m_host)
        {
            return PI_OK; /* idempotent */
        }
        if (!host)
        {
            return PI_OK;
        }
        m_host = host;
        pi_iunknown_add_ref((IPiUnknown*)host);
        IPiPluginHostUI* ui = nullptr;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host, &PI_PLUGIN_IID_HOST_UI,
                                                     (void**)&ui)))
        {
            m_hostUI = ui;
        }
        return PI_OK;
    }

    /* 造一个视图。0.6 起它由 IPiPluginViewProvider 调用（见文件末尾），
     * 而不是 IPiPluginBase 的一个槽位。 */
    PiResult CreateView(IPiPluginView** out)
    {
        if (!out)
        {
            return PI_E_INVALIDARG;
        }
        if (!m_hostUI)
        {
            *out = nullptr;
            return PI_E_NOINTERFACE;
        } /* headless host */

        PiPluginQtViewDesc desc = {};
        desc.create_widget      = &CreateUi;
        desc.retain             = &Retain;
        desc.release            = &Release;
        desc.user_data          = this;
        desc.title              = "Qt example";
        return pi_plugin_qt_view_create(&desc, out);
    }

    PiResult Terminate()
    {
        /* The kit must finish destroying OUR widgets before the host unloads this
         * module. The host detaching the view first is normally enough; this makes
         * it true even for a host that just drops the module.
         *
         * _owner(this): the kit is shared by every Qt plugin in the process, so the
         * unscoped pi_plugin_qt_view_shutdown() would tear down other plugins' widgets too. */
        pi_plugin_qt_view_shutdown_owner(this);
        return PI_OK;
    }

    /* ---- widget factory (host GUI thread) ---- */
    static QWidget* CreateUi(void* user_data)
    {
        ExampleQtPlugin* me = (ExampleQtPlugin*)user_data;

        QWidget*     root   = new QWidget();
        QVBoxLayout* layout = new QVBoxLayout(root);

        QLabel* label = new QLabel(QString::fromUtf8("Hello from a piplugin Qt example!"));
        layout->addWidget(label);

        QPushButton* button = new QPushButton(QString::fromUtf8("Send to host"));
        QObject::connect(button, &QPushButton::clicked, [me, label]()
                         {
            ++me->m_clicks;
            label->setText(QString::fromUtf8("button pressed %1 time(s)").arg(me->m_clicks));
            if (me->m_host){
                pi_plugin_host_post_message(me->m_host, 0x8000u, (uintptr_t)me->m_clicks, 0);
} });
        layout->addWidget(button);
        layout->addStretch();
        return root;
    }

    static void Retain(void* user_data)
    {
        ExampleQtPlugin* me = (ExampleQtPlugin*)user_data;
        if (me)
        {
            me->m_base.unk.lpVtbl->pi_add_ref(me);
        }
    }

    static void Release(void* user_data)
    {
        ExampleQtPlugin* me = (ExampleQtPlugin*)user_data;
        if (me)
        {
            me->m_base.unk.lpVtbl->pi_release(me);
        }
    }

public:
    PiRefCountedBase m_base; /* MUST be first data member */

private:
    static void Destroy(void* self) { delete static_cast<ExampleQtPlugin*>(self); }

    static PiResult PI_CALL Qi(void* self_ptr, PiGuid const* iid, void** out)
    {
        ExampleQtPlugin* me = (ExampleQtPlugin*)self_ptr;
        if (!out)
        {
            return PI_E_INVALIDARG;
        }
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE))
        {
            *out = me;
            me->m_base.unk.lpVtbl->pi_add_ref(self_ptr);
            return PI_OK;
        }
        if (pi_guid_equal(iid, &PI_PLUGIN_IID_VIEW_PROVIDER))
        {
            /* headless 宿主上不暴露界面：能力声明里 GUI 是 OPTIONAL。 */
            if (!me->m_hostUI)
            {
                *out = nullptr;
                return PI_E_NOINTERFACE;
            }
            return CreateProvider(me, out);
        }
        *out = nullptr;
        return PI_E_NOINTERFACE;
    }
    static PiResult PI_CALL Init(void* self_ptr, IPiPluginHostServices* host)
    {
        return ((ExampleQtPlugin*)self_ptr)->Initialize(host);
    }
    static PiResult PI_CALL Term(void* self_ptr)
    {
        return ((ExampleQtPlugin*)self_ptr)->Terminate();
    }
    static PiResult PI_CALL CreateProvider(void* self_ptr, void** out); /* defined below */

    static IPiPluginBaseVtbl const s_vtbl;

    int                    m_clicks;
    IPiPluginHostServices* m_host;   /* add-ref'd */
    IPiPluginHostUI*       m_hostUI; /* add-ref'd, NULL on a headless host */
};

IPiPluginBaseVtbl const ExampleQtPlugin::s_vtbl = {
    {&ExampleQtPlugin::Qi, &pi_refcounted_add_ref, &pi_refcounted_release},
    &ExampleQtPlugin::Init,
    &ExampleQtPlugin::Term
};

/* --------------------------------------------------------------------------
 * IPiPluginViewProvider (0.6)
 *
 * 一个对象不能有两个 vtbl，所以界面能力是一个独立的薄对象：它持有一份插件
 * 引用，宿主 QI 一次拿一个、用完 release。单面板插件只有一个 id "main"。
 * -------------------------------------------------------------------------- */
struct ExampleQtViewProvider
{
    PiRefCountedBase base;  /* MUST be first */
    ExampleQtPlugin* owner; /* add-ref'd */
};

static uint32_t PI_CALL ExampleProvider_AddRef(void* self)
{
    return pi_refcounted_add_ref(self);
}
static uint32_t PI_CALL ExampleProvider_Release(void* self)
{
    return pi_refcounted_release(self);
}
static void ExampleProvider_Destroy(void* self)
{
    ExampleQtViewProvider* provider = (ExampleQtViewProvider*)self;
    if (provider->owner)
    {
        pi_iunknown_release((IPiUnknown*)&provider->owner->m_base);
        provider->owner = nullptr;
    }
    delete provider;
}
static PiResult PI_CALL ExampleProvider_Qi(void* self, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_VIEW_PROVIDER))
    {
        *out = self;
        pi_refcounted_add_ref(self);
        return PI_OK;
    }
    *out = nullptr;
    return PI_E_NOINTERFACE;
}
static PiResult PI_CALL ExampleProvider_Views(void* self, PiPluginViewInfo const** out_views,
                                              uint32_t* out_count)
{
    static PiPluginViewInfo const kViews[] = {{"main"}};
    (void)self;
    if (!out_views || !out_count)
    {
        return PI_E_INVALIDARG;
    }
    *out_views = kViews;
    *out_count = 1;
    return PI_OK;
}
static PiResult PI_CALL ExampleProvider_GetView(void* self, char const* view_id,
                                                IPiPluginView** out)
{
    ExampleQtViewProvider* provider = (ExampleQtViewProvider*)self;
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    *out = nullptr;
    if (!view_id || view_id[0] == '\0')
    {
        return PI_E_INVALIDARG;
    }
    if (strcmp(view_id, "main") != 0)
    {
        return PI_E_NOTFOUND;
    }
    return provider->owner->CreateView(out);
}

static IPiPluginViewProviderVtbl const s_view_provider_vtbl = {
    {&ExampleProvider_Qi, &ExampleProvider_AddRef, &ExampleProvider_Release},
    &ExampleProvider_Views,
    &ExampleProvider_GetView
};

PiResult PI_CALL ExampleQtPlugin::CreateProvider(void* self_ptr, void** out)
{
    ExampleQtPlugin*       me       = (ExampleQtPlugin*)self_ptr;
    ExampleQtViewProvider* provider = new ExampleQtViewProvider();
    if (!provider)
    {
        *out = nullptr;
        return PI_E_OUTOFMEMORY;
    }
    provider->owner = me;
    pi_refcounted_init_with_destroy(&provider->base, (IPiUnknownVtbl const*)&s_view_provider_vtbl,
                                    &ExampleProvider_Destroy);
    pi_refcounted_add_ref(&me->m_base);
    *out = (IPiPluginViewProvider*)&provider->base;
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */
class ExampleQtFactory
{
public:
    ExampleQtFactory()
    {
        pi_refcounted_init_with_destroy(&m_base, (IPiUnknownVtbl const*)&s_vtbl, &Destroy);

        /* Zero first: appended optional fields (properties) must read as "absent",
         * not as whatever this heap block happened to contain.
         * See pi_plugin_descriptor_init() in pi_plugin_types.h. */
        pi_plugin_descriptor_init(&m_desc);

        m_caps[0].iid   = PI_PLUGIN_IID_VIEW_PROVIDER;
        m_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;
        m_caps[1].iid   = PI_PLUGIN_IID_HOST_UI;
        m_caps[1].flags = PI_PLUGIN_CAP_OPTIONAL;

        m_desc.name             = "Example Qt Plugin";
        m_desc.vendor           = "piplugin examples";
        m_desc.version          = "1.0.0";
        m_desc.category         = "Example/UI";
        m_desc.api_version      = PI_PLUGIN_API_VERSION;
        m_desc.capabilities     = m_caps;
        m_desc.capability_count = 2;
    }

    static PiResult PI_CALL Qi(void* self_ptr, PiGuid const* iid, void** out)
    {
        ExampleQtFactory* me = (ExampleQtFactory*)self_ptr;
        if (!out)
        {
            return PI_E_INVALIDARG;
        }
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY))
        {
            *out = me;
            me->m_base.unk.lpVtbl->pi_add_ref(self_ptr);
            return PI_OK;
        }
        *out = nullptr;
        return PI_E_NOINTERFACE;
    }
    static uint32_t PI_CALL                  AddRef(void* self) { return pi_refcounted_add_ref(self); }
    static uint32_t PI_CALL                  Release(void* self) { return pi_refcounted_release(self); }
    static PiPluginDescriptor const* PI_CALL GetDescriptor(void* self)
    {
        return &((ExampleQtFactory*)self)->m_desc;
    }
    static uint32_t PI_CALL GetClassCount(void* self)
    {
        (void)self;
        return 1;
    }
    static PiResult PI_CALL GetClassGuid(void* self, uint32_t index, PiGuid* guid)
    {
        (void)self;
        if (index != 0 || !guid)
        {
            return PI_E_INVALIDARG;
        }
        *guid = EXAMPLE_CLASS_GUID;
        return PI_OK;
    }
    static PiResult PI_CALL CreateInstance(void* self, PiGuid const* guid,
                                           IPiPluginHostServices* host, IPiPluginBase** out)
    {
        (void)self;
        if (!guid || !out)
        {
            return PI_E_INVALIDARG;
        }
        *out = nullptr;
        if (!pi_guid_equal(guid, &EXAMPLE_CLASS_GUID))
        {
            return PI_E_NOINTERFACE;
        }

        ExampleQtPlugin* plugin = new ExampleQtPlugin();
        if (!plugin)
        {
            return PI_E_OUTOFMEMORY;
        }
        PiResult hr = plugin->Initialize(host);
        if (PI_FAILED(hr))
        {
            delete plugin;
            return hr;
        }
        *out = (IPiPluginBase*)&plugin->m_base;
        return PI_OK;
    }

public:
    PiRefCountedBase m_base; /* MUST be first data member */

private:
    static void Destroy(void* self) { delete static_cast<ExampleQtFactory*>(self); }

    static IPiPluginFactoryVtbl const s_vtbl;
    PiPluginDescriptor                m_desc;
    PiPluginCapability                m_caps[2];
};

IPiPluginFactoryVtbl const ExampleQtFactory::s_vtbl = {
    {&ExampleQtFactory::Qi, &ExampleQtFactory::AddRef, &ExampleQtFactory::Release},
    &ExampleQtFactory::GetDescriptor,
    &ExampleQtFactory::GetClassCount,
    &ExampleQtFactory::GetClassGuid,
    &ExampleQtFactory::CreateInstance
};

PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory)
    {
        return PI_E_INVALIDARG;
    }
    *out_factory = (IPiPluginFactory*)&(new ExampleQtFactory())->m_base;
    return PI_OK;
}
