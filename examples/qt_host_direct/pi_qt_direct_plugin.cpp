/*
 * piplugin example - the PLUGIN half of the Qt-host direct integration
 *
 * This is a Qt plugin for a host that is itself a Qt program. Read
 * docs/tutorial/qt-host-direct.md first; the short version:
 *
 *   - it does NOT link the Qt adapter kit (piplugin_qt). The kit would try to
 *     create the process's QApplication, and the host already has one;
 *   - it implements the app's IQtDirectWidget protocol instead: the host asks
 *     for a QWidget* and adopts it with its own layout;
 *   - it declares PI_PLUGIN_CAP_PROVIDES for that protocol, so a host that requires it
 *     (see the host in this directory) rejects plugins that do not implement it
 *     BEFORE instantiating them - that is what turns "silently no UI" into a
 *     named load error.
 *
 * Nothing here is Windows-specific: no native window, no embedding, no
 * SetParent. The host's own QApplication and event loop do all the work.
 *
 * Run it with examples/qt_host_direct (three steps in its README).
 */
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <cstdio>

#include "pi_qt_direct_protocol.h"
#include "piplugin/pi_plugin.h"

static PiGuid const EXAMPLE_CLASS_GUID =
    PI_GUID(0x2F80C6A1, 0x73D5, 0x4E19, 0xB4, 0x8C, 0x51, 0x2E, 0xA7, 0x69, 0x0F, 0x38);

class QtDirectPlugin;

/* --------------------------------------------------------------------------
 * The app protocol lives on its own object: two vtables cannot both sit at
 * offset 0 of one object (same idiom as examples/specialized_app).
 * ------------------------------------------------------------------------ */
class QtDirectWidgetIfc
{
public:
    explicit QtDirectWidgetIfc(QtDirectPlugin* owner);

    PiRefCountedBase m_base; /* MUST be first: this is IQtDirectWidget */

private:
    static PiResult PI_CALL Qi(void* self_ptr, PiGuid const* iid, void** out);
    static QWidget* PI_CALL CreateWidgetThunk(void* self_ptr);
    static void PI_CALL     DestroyWidgetThunk(void* self_ptr, QWidget* widget);
    static void             Destroy(void* self);

    static IQtDirectWidgetVtbl const s_vtbl;
    QtDirectPlugin*                  m_owner; /* add-ref'd */
};

/* --------------------------------------------------------------------------
 * The plugin itself (IPiPluginBase)
 * ------------------------------------------------------------------------ */
class QtDirectPlugin
{
public:
    QtDirectPlugin()
        : m_base()
        , m_host(nullptr)
        , m_clicks(0)
        , m_live_widgets(0)
    {
        pi_refcounted_init_with_destroy(&m_base, (IPiUnknownVtbl const*)&s_vtbl, &Destroy);
    }

    ~QtDirectPlugin()
    {
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
        return PI_OK;
    }

    /* NO IPiPluginView, deliberately: in the direct integration the UI does not
     * travel over PI_PLUGIN_IID_PLUGIN_VIEW (that is the adapter kit's channel, and the
     * kit needs to own the event loop). It travels over the app's protocol. */
    PiResult GetView(IPiPluginView** out)
    {
        if (!out)
        {
            return PI_E_INVALIDARG;
        }
        *out = nullptr;
        return PI_E_NOINTERFACE;
    }

    PiResult Terminate()
    {
        if (m_live_widgets != 0)
        {
            /* Not fatal - the host's teardown may still be ahead of us - but it
             * means the module is about to be unmapped while one of our widgets
             * is alive. Say so instead of crashing later. */
            printf("[qt-direct plugin] WARNING: terminating with %d widget(s) still alive; "
                   "the host must call destroy_widget() before unloading\n",
                   m_live_widgets);
        }
        return PI_OK;
    }

    /* ---- UI: a fresh, parentless widget the host will adopt ---- */
    QWidget* CreateWidget()
    {
        QWidget*     root   = new QWidget();
        QVBoxLayout* layout = new QVBoxLayout(root);

        QLabel* title = new QLabel(QString::fromUtf8("I am a Qt plugin widget, "
                                                     "adopted by the host's QLayout"));
        title->setWordWrap(true);
        layout->addWidget(title);

        QLabel* count = new QLabel(QString::fromUtf8("button pressed 0 time(s)"));
        layout->addWidget(count);

        QPushButton* button = new QPushButton(QString::fromUtf8("Post a message to the host"));
        QObject::connect(button, &QPushButton::clicked, [this, count]()
                         {
            ++m_clicks;
            count->setText(QString::fromUtf8("button pressed %1 time(s)").arg(m_clicks));
            if (m_host){
                pi_plugin_host_post_message(m_host, 0x8000u, (uintptr_t)m_clicks, 0);
} });
        layout->addWidget(button);
        layout->addStretch();

        ++m_live_widgets;
        printf("[qt-direct plugin] created widget %p for the host\n", (void*)root);
        return root;
    }

    void DestroyWidget(QWidget* widget)
    {
        if (!widget)
        {
            return;
        }
        printf("[qt-direct plugin] destroying widget %p (host is done with it)\n",
               (void*)widget);
        if (m_live_widgets > 0)
        {
            --m_live_widgets;
        }
        delete widget; /* lives in this module: it must die here */
    }

public:
    PiRefCountedBase       m_base; /* MUST be first data member */
    IPiPluginHostServices* m_host; /* add-ref'd */

private:
    static void Destroy(void* self) { delete static_cast<QtDirectPlugin*>(self); }

    static PiResult PI_CALL Qi(void* self_ptr, PiGuid const* iid, void** out)
    {
        QtDirectPlugin* me = (QtDirectPlugin*)self_ptr;
        if (!out)
        {
            return PI_E_INVALIDARG;
        }

        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE))
        {
            *out = me;
            pi_refcounted_add_ref(me);
            return PI_OK;
        }
        if (pi_guid_equal(iid, &PI_PLUGIN_QT_DIRECT_WIDGET_IID))
        {
            QtDirectWidgetIfc* ifc = new QtDirectWidgetIfc(me);
            if (!ifc)
            {
                return PI_E_OUTOFMEMORY;
            }
            *out = &ifc->m_base;
            return PI_OK;
        }
        *out = nullptr;
        return PI_E_NOINTERFACE;
    }

    static PiResult PI_CALL InitThunk(void* self_ptr, IPiPluginHostServices* host)
    {
        return ((QtDirectPlugin*)self_ptr)->Initialize(host);
    }
    static PiResult PI_CALL TermThunk(void* self_ptr)
    {
        return ((QtDirectPlugin*)self_ptr)->Terminate();
    }
    static PiResult PI_CALL GetViewThunk(void* self_ptr, IPiPluginView** out)
    {
        return ((QtDirectPlugin*)self_ptr)->GetView(out);
    }

    static IPiPluginBaseVtbl const s_vtbl;

    int m_clicks;
    int m_live_widgets;
};

IPiPluginBaseVtbl const QtDirectPlugin::s_vtbl = {
    {&QtDirectPlugin::Qi, &pi_refcounted_add_ref, &pi_refcounted_release},
    &QtDirectPlugin::InitThunk,
    &QtDirectPlugin::TermThunk,
    &QtDirectPlugin::GetViewThunk
};

/* ---- the protocol wrapper ---- */

QtDirectWidgetIfc::QtDirectWidgetIfc(QtDirectPlugin* owner)
    : m_base()
    , m_owner(owner)
{
    pi_refcounted_init_with_destroy(&m_base, (IPiUnknownVtbl const*)&s_vtbl, &Destroy);
    if (m_owner)
    {
        pi_refcounted_add_ref(&m_owner->m_base);
    }
}

PiResult PI_CALL QtDirectWidgetIfc::Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_QT_DIRECT_WIDGET_IID))
    {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = nullptr;
    return PI_E_NOINTERFACE;
}

QWidget* PI_CALL QtDirectWidgetIfc::CreateWidgetThunk(void* self_ptr)
{
    QtDirectWidgetIfc* ifc = (QtDirectWidgetIfc*)self_ptr;
    return ifc->m_owner ? ifc->m_owner->CreateWidget() : nullptr;
}

void PI_CALL QtDirectWidgetIfc::DestroyWidgetThunk(void* self_ptr, QWidget* widget)
{
    QtDirectWidgetIfc* ifc = (QtDirectWidgetIfc*)self_ptr;
    if (ifc->m_owner)
    {
        ifc->m_owner->DestroyWidget(widget);
    }
}

void QtDirectWidgetIfc::Destroy(void* self)
{
    QtDirectWidgetIfc* ifc = (QtDirectWidgetIfc*)self;
    if (ifc->m_owner)
    {
        pi_refcounted_release(&ifc->m_owner->m_base);
        ifc->m_owner = nullptr;
    }
    delete ifc;
}

IQtDirectWidgetVtbl const QtDirectWidgetIfc::s_vtbl = {
    {&QtDirectWidgetIfc::Qi, &pi_refcounted_add_ref, &pi_refcounted_release},
    &QtDirectWidgetIfc::CreateWidgetThunk,
    &QtDirectWidgetIfc::DestroyWidgetThunk
};

/* --------------------------------------------------------------------------
 * Factory
 * ------------------------------------------------------------------------ */
class QtDirectFactory
{
public:
    QtDirectFactory()
    {
        pi_refcounted_init_with_destroy(&m_base, (IPiUnknownVtbl const*)&s_vtbl, &Destroy);
        pi_plugin_descriptor_init(&m_desc);

        /* The one declaration the host's gate reads: "I implement the app's
         * widget protocol". Note what is NOT here: PI_PLUGIN_IID_PLUGIN_VIEW (there is
         * no IPiPluginView in this design) and PI_PLUGIN_IID_HOST_UI (the host's layout
         * is the container, not a native window). */
        m_caps[0].iid   = PI_PLUGIN_QT_DIRECT_WIDGET_IID;
        m_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;

        m_desc.name             = "Example Qt Direct Plugin";
        m_desc.vendor           = "piplugin examples";
        m_desc.version          = "1.0.0";
        m_desc.category         = "Example/UI";
        m_desc.api_version      = PI_PLUGIN_API_VERSION;
        m_desc.capabilities     = m_caps;
        m_desc.capability_count = 1;
    }

    static PiResult PI_CALL Qi(void* self_ptr, PiGuid const* iid, void** out)
    {
        QtDirectFactory* me = (QtDirectFactory*)self_ptr;
        if (!out)
        {
            return PI_E_INVALIDARG;
        }
        if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY))
        {
            *out = me;
            pi_refcounted_add_ref(me);
            return PI_OK;
        }
        *out = nullptr;
        return PI_E_NOINTERFACE;
    }
    static PiPluginDescriptor const* PI_CALL GetDescriptor(void* self)
    {
        return &((QtDirectFactory*)self)->m_desc;
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

        QtDirectPlugin* plugin = new QtDirectPlugin();
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
    static void Destroy(void* self) { delete static_cast<QtDirectFactory*>(self); }

    static IPiPluginFactoryVtbl const s_vtbl;
    PiPluginDescriptor                m_desc;
    PiPluginCapability                m_caps[1];
};

IPiPluginFactoryVtbl const QtDirectFactory::s_vtbl = {
    {&QtDirectFactory::Qi, &pi_refcounted_add_ref, &pi_refcounted_release},
    &QtDirectFactory::GetDescriptor,
    &QtDirectFactory::GetClassCount,
    &QtDirectFactory::GetClassGuid,
    &QtDirectFactory::CreateInstance
};

PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory)
    {
        return PI_E_INVALIDARG;
    }
    *out_factory = (IPiPluginFactory*)&(new QtDirectFactory())->m_base;
    return PI_OK;
}
