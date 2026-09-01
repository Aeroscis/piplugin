#include "pi_qt_test_plugin.h"
#include "pi_qt_view.h"

#include <QWidget>
#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QTimer>
#include <stdio.h>

static const PiGuid QT_PLUGIN_CLASS_GUID =
    PI_GUID(0x7F83A100, 0x5C4D, 0x4E2A,
            0x91, 0xD3, 0x8A, 0xFC, 0x2E, 0xB1, 0x44, 0x00);

/* ==========================================================================
 * Factory
 * ======================================================================== */
const IPiPluginFactoryVtbl QtPluginFactory::s_factory_vtbl = {
    { &QtPluginFactory::Qi_Factory, &pi_refcounted_add_ref, &pi_refcounted_release },
    &QtPluginFactory::GetDescriptor,
    &QtPluginFactory::GetClassCount,
    &QtPluginFactory::GetClassGuid,
    &QtPluginFactory::CreateInstance
};

QtPluginFactory::QtPluginFactory()
{
    pi_refcounted_init_with_destroy(&m_base,
                                    (const IPiUnknownVtbl*)&s_factory_vtbl,
                                    &pi_cpp_destroy<QtPluginFactory>);

    m_descriptor.name = "Qt Test Plugin";
    m_descriptor.vendor = "pipluginframework";
    m_descriptor.version = "1.2.0";
    m_descriptor.category = "UI/Test";
    m_descriptor.api_version = PI_API_VERSION;

    /* LV2-style capability declaration:
     *  - this plugin provides a GUI view
     *  - it optionally uses the host's UI services (runs headless without) */
    m_capabilities[0].iid = PI_IID_PLUGIN_VIEW;
    m_capabilities[0].flags = PI_CAP_PROVIDES;
    m_capabilities[1].iid = PI_IID_HOST_UI;
    m_capabilities[1].flags = PI_CAP_OPTIONAL;

    m_descriptor.capabilities = m_capabilities;
    m_descriptor.capability_count = 2;
}

uint32_t PI_CALL QtPluginFactory::AddRef(void* self_ptr) { return pi_refcounted_add_ref(self_ptr); }
uint32_t PI_CALL QtPluginFactory::Release(void* self_ptr) { return pi_refcounted_release(self_ptr); }
PiResult PI_CALL QtPluginFactory::Qi_Factory(void* self_ptr, const PiGuid* iid, void** out) {
    if (!out) return PI_E_INVALIDARG;
    QtPluginFactory* me = (QtPluginFactory*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_FACTORY)) {
        *out = me; me->m_base.unk.lpVtbl->pi_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}
const PiPluginDescriptor* PI_CALL QtPluginFactory::GetDescriptor(void* self_ptr) {
    return &((QtPluginFactory*)self_ptr)->m_descriptor;
}
uint32_t PI_CALL QtPluginFactory::GetClassCount(void* self_ptr) { (void)self_ptr; return 1; }
PiResult PI_CALL QtPluginFactory::GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid) {
    (void)self_ptr; if (index != 0 || !guid) return PI_E_INVALIDARG; *guid = QT_PLUGIN_CLASS_GUID; return PI_OK;
}
PiResult PI_CALL QtPluginFactory::CreateInstance(void* self_ptr, const PiGuid* guid,
                                                 IPiHostServices* host, IPiPluginBase** out) {
    (void)self_ptr;
    if (!guid || !out) return PI_E_INVALIDARG;
    if (!pi_guid_equal(guid, &QT_PLUGIN_CLASS_GUID)) return PI_E_NOINTERFACE;
    QtPlugin* plugin = new QtPlugin();
    if (!plugin) return PI_E_OUTOFMEMORY;
    PiResult hr = plugin->Initialize(host);
    if (PI_FAILED(hr)) { delete plugin; return hr; }
    *out = (IPiPluginBase*)&plugin->m_base;
    return PI_OK;
}

/* ==========================================================================
 * Plugin
 * ======================================================================== */
const IPiPluginBaseVtbl QtPlugin::s_base_vtbl = {
    { &QtPlugin::Qi_PluginBase, &pi_refcounted_add_ref, &pi_refcounted_release },
    &QtPlugin::Init, &QtPlugin::Term, &QtPlugin::GetView
};

QtPlugin::QtPlugin() : m_host(NULL), m_hostUI(NULL)
{
    pi_refcounted_init_with_destroy(&m_base,
                                    (const IPiUnknownVtbl*)&s_base_vtbl,
                                    &QtPlugin::Destroy);
}

QtPlugin::~QtPlugin()
{
    if (m_hostUI)   { pi_iunknown_release((IPiUnknown*)m_hostUI);   m_hostUI = NULL; }
    if (m_host)     { pi_iunknown_release((IPiUnknown*)m_host);     m_host = NULL; }
}

PiResult QtPlugin::Initialize(IPiHostServices* host)
{
    if (host) {
        m_host = host;
        pi_iunknown_add_ref((IPiUnknown*)host);

        /* Discover whether this is a GUI host. A headless host (task
         * server) returns PI_E_NOINTERFACE and we simply skip UI. */
        IPiHostUI* ui = NULL;
        if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host,
                                                     &PI_IID_HOST_UI, (void**)&ui))) {
            m_hostUI = ui;
        }
    }
    return PI_OK;
}

PiResult QtPlugin::Terminate()
{
    /* Nothing to do: the adapter kit tears the UI down when the view is
     * detached / released. */
    return PI_OK;
}

/* --------------------------------------------------------------------------
 * Widget factory — everything Qt-specific lives here, and it is pure UI
 * construction: no threads, no embedding, no host knowledge beyond the
 * message post.
 * ------------------------------------------------------------------------ */
QWidget* QtPlugin::CreateUi(void* user_data)
{
    QtPlugin* me = (QtPlugin*)user_data;

    /* Root widget embedded into the host window */
    QWidget* w = new QWidget();
    w->setMinimumSize(200, 150);
    QVBoxLayout* layout = new QVBoxLayout(w);
    layout->setContentsMargins(10, 10, 10, 10);

    QLabel* label = new QLabel(QString::fromUtf8("Qt Plugin - Running!"), w);
    label->setAlignment(Qt::AlignCenter);
    QFont font = label->font();
    font.setPointSize(14);
    label->setFont(font);
    layout->addWidget(label);

    QSlider* slider = new QSlider(Qt::Horizontal, w);
    slider->setRange(0, 100);
    slider->setValue(50);
    QObject::connect(slider, &QSlider::valueChanged, [me, label](int v) {
        label->setText(QString::fromUtf8("Qt Plugin - Value: %1").arg(v));
        if (me->m_host)
            pi_host_post_message(me->m_host, 0x1000, (uintptr_t)v, 0);
    });
    layout->addWidget(slider);

    QPushButton* btn = new QPushButton(QString::fromUtf8("Reset"), w);
    QObject::connect(btn, &QPushButton::clicked, [slider]() { slider->setValue(50); });
    layout->addWidget(btn);
    layout->addStretch();

    /* Standalone animation popup (proof that the Qt loop runs). A child
     * with the Qt::Window flag: top-level, but owned by w so it is
     * destroyed together with the embedded widget. */
    QWidget* popup = new QWidget(w, Qt::Window | Qt::WindowStaysOnTopHint);
    popup->setWindowTitle(QString::fromUtf8("Qt Heartbeat"));
    popup->setFixedSize(340, 100);
    popup->move(200, 200);
    QVBoxLayout* popLay = new QVBoxLayout(popup);
    QLabel* popLabel = new QLabel(QString::fromUtf8("Qt Event Loop: tick 0"), popup);
    popLabel->setAlignment(Qt::AlignCenter);
    popLay->addWidget(popLabel);
    int* popTick = new int(0);
    QTimer* popTimer = new QTimer(popup);
    QObject::connect(popTimer, &QTimer::timeout, [popLabel, popTick]() {
        (*popTick)++;
        int p = (*popTick) % 100, r = 0, g = 0, b = 0;
        if (p < 33)      { r = 255 - p*7;  g = p*7;        b = 0; }
        else if (p < 66) { r = 0;          g = 255-(p-33)*7; b = (p-33)*7; }
        else             { r = (p-66)*7;   g = 0;          b = 255-(p-66)*7; }
        char s[256];
        snprintf(s, sizeof(s),
            "QLabel{background:rgb(%d,%d,%d);color:white;font-size:14px;"
            "font-weight:bold;padding:8px;border-radius:6px;}", r, g, b);
        popLabel->setStyleSheet(QString::fromUtf8(s));
        char t[64]; snprintf(t, sizeof(t), "Qt Event Loop - tick %d", *popTick);
        popLabel->setText(QString::fromUtf8(t));
    });
    popTimer->start(40);
    popup->show();

    return w;
}

void QtPlugin::Retain(void* user_data)
{
    QtPlugin* me = (QtPlugin*)user_data;
    if (me) me->m_base.unk.lpVtbl->pi_add_ref(me);
}

void QtPlugin::Release(void* user_data)
{
    QtPlugin* me = (QtPlugin*)user_data;
    if (me) me->m_base.unk.lpVtbl->pi_release(me);
}

PiResult PI_CALL QtPlugin::Qi_PluginBase(void* self_ptr, const PiGuid* iid, void** out) {
    if (!out) return PI_E_INVALIDARG;
    QtPlugin* me = (QtPlugin*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_BASE)) {
        *out = me; me->m_base.unk.lpVtbl->pi_add_ref(self_ptr); return PI_OK;
    }
    *out = NULL; return PI_E_NOINTERFACE;
}
PiResult PI_CALL QtPlugin::Init(void* self_ptr, IPiHostServices* host) {
    return ((QtPlugin*)self_ptr)->Initialize(host);
}
PiResult PI_CALL QtPlugin::Term(void* self_ptr) {
    return ((QtPlugin*)self_ptr)->Terminate();
}
PiResult PI_CALL QtPlugin::GetView(void* self_ptr, IPiPluginView** out) {
    if (!out) return PI_E_INVALIDARG;
    QtPlugin* me = (QtPlugin*)self_ptr;
    /* On a headless host we advertise no view at all. */
    if (!me->m_hostUI) { *out = NULL; return PI_E_NOINTERFACE; }

    /* All Qt integration is delegated to the adapter kit. */
    PiQtViewDesc desc = {};
    desc.create_widget = &QtPlugin::CreateUi;
    desc.retain        = &QtPlugin::Retain;
    desc.release       = &QtPlugin::Release;
    desc.user_data     = me;
    return pi_qt_view_create(&desc, out);
}
