/*
 * pipluginframework — Test Host (Qt / Widgets)
 *
 * The mirror twin of the imgui test host: a Qt application that embeds
 * an ImGui-based plugin. Demonstrates how a Qt host merges foreign
 * plugin UIs into ITS event loop:
 *
 *   1. A native QWidget acts as the embed container; its winId() is the
 *      PiNativeWindow passed to pi_view_attach().
 *   2. A QTimer (fires on every event-loop iteration) calls
 *      pi_view_on_idle() — Qt's idiomatic way to drive per-frame plugin
 *      work without stealing the loop.
 *   3. Container resizes are forwarded via pi_view_on_resize().
 *
 * The host knows nothing about Dear ImGui (or Qt-as-a-plugin): the whole
 * compatibility burden lives in the plugin-side adapter kits.
 */
#include "pipluginframework/pi_plugin.h"

#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QEvent>
#include <QCloseEvent>
#include <QString>
#include <cstdio>
#include <string>
#include <cstdarg>
#include <windows.h>

/* --------------------------------------------------------------------------
 * Debug log (mirrors the imgui host) — also used for automated
 * verification without touching the UI.
 * ------------------------------------------------------------------------ */
static void LogStatus(const char* fmt, ...)
{
    static FILE* f = NULL;
    if (!f) {
        char logPath[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, logPath, MAX_PATH);
        std::string s(logPath, len > 0 && len < MAX_PATH ? len : 0);
        size_t slash = s.find_last_of("\\/");
        if (slash != std::string::npos) s = s.substr(0, slash + 1);
        s += "pi_qt_host.log";
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
 * Host state
 * ------------------------------------------------------------------------ */
static IPiPluginBase*   g_plugin       = NULL;
static IPiPluginView*   g_pluginView   = NULL;
static PiPluginModule*  g_pluginModule = NULL;
static IPiHostServices* g_hostServices = NULL;

static QWidget*  g_embedContainer = NULL;
static QLabel*   g_statusLabel     = NULL;
static QTimer*   g_idleDriver      = NULL;

static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    LogStatus("[plugin message] msg=0x%04X wparam=%llu",
              msg, (unsigned long long)wparam);
}

static std::string ExeDirPath(const char* dllName)
{
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string s(exePath, len > 0 && len < MAX_PATH ? len : 0);
    size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s.resize(slash + 1);
    return s + dllName;
}

static void SetStatus(const QString& text)
{
    if (g_statusLabel) g_statusLabel->setText(text);
    LogStatus("%s", text.toUtf8().constData());
}

/* --------------------------------------------------------------------------
 * Plugin load / unload
 * ------------------------------------------------------------------------ */
static void UnloadPlugin()
{
    LogStatus("unload: begin");
    if (g_pluginView) {
        pi_view_detach(g_pluginView);
        pi_iunknown_release((IPiUnknown*)g_pluginView);
        g_pluginView = NULL;
    }
    if (g_plugin) {
        pi_plugin_terminate(g_plugin);
        pi_iunknown_release((IPiUnknown*)g_plugin);
        g_plugin = NULL;
    }
    if (g_pluginModule) {
        pi_module_unload(g_pluginModule);
        g_pluginModule = NULL;
    }
    SetStatus(QString::fromUtf8("No plugin loaded"));
    LogStatus("unload: done");
}

static void LoadPlugin(const char* dllPath)
{
    UnloadPlugin();

    if (!g_hostServices) {
        /* Create the host services object with the embed container's
         * native window, so IPiHostUI is exposed to plugins. */
        if (PI_FAILED(pi_host_services_create_default(&HostMessageProc, NULL,
                                                      (PiNativeWindow)g_embedContainer->winId(),
                                                      &g_hostServices))) {
            SetStatus(QString::fromUtf8("Host services unavailable"));
            return;
        }
    }

    PiPluginModule* module = pi_module_load(dllPath);
    if (!module) {
        SetStatus(QString::fromUtf8("Load failed: %1")
                  .arg(QString::fromLocal8Bit(pi_module_get_load_error())));
        return;
    }

    IPiPluginFactory* factory = NULL;
    if (PI_FAILED(pi_module_get_factory(module, &factory))) {
        SetStatus(QString::fromUtf8("No factory in plugin"));
        pi_module_unload(module);
        return;
    }

    const PiPluginDescriptor* desc = NULL;
    pi_factory_get_descriptor(factory, &desc);

    /* Capability gate (LV2-style): check requirements BEFORE instantiating. */
    if (desc && pi_descriptor_requires(desc, &PI_IID_HOST_UI)) {
        void* dummy = NULL;
        if (PI_FAILED(pi_iunknown_query_interface((IPiUnknown*)g_hostServices,
                                                  &PI_IID_HOST_UI, &dummy))) {
            SetStatus(QString::fromUtf8("Rejected: plugin requires GUI host"));
            pi_iunknown_release((IPiUnknown*)factory);
            pi_module_unload(module);
            return;
        }
        pi_iunknown_release((IPiUnknown*)dummy);
    }

    PiGuid classGuid;
    if (PI_FAILED(pi_factory_get_class_guid(factory, 0, &classGuid))) {
        SetStatus(QString::fromUtf8("Plugin has no classes"));
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        return;
    }

    if (PI_FAILED(pi_factory_create_instance(factory, &classGuid, g_hostServices, &g_plugin))) {
        SetStatus(QString::fromUtf8("Failed to create instance"));
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        return;
    }

    if (PI_FAILED(pi_plugin_initialize(g_plugin, g_hostServices))) {
        SetStatus(QString::fromUtf8("Plugin initialize failed"));
        pi_iunknown_release((IPiUnknown*)g_plugin); g_plugin = NULL;
        pi_iunknown_release((IPiUnknown*)factory);
        pi_module_unload(module);
        return;
    }

    g_pluginModule = module;

    IPiPluginView* view = NULL;
    if (PI_SUCCEEDED(pi_plugin_get_view(g_plugin, &view)) && view) {
        g_pluginView = view;
        if (PI_SUCCEEDED(pi_view_attach(g_pluginView,
                                        (PiNativeWindow)g_embedContainer->winId()))) {
            pi_view_set_visible(g_pluginView, 1);
        }
    }

    pi_iunknown_release((IPiUnknown*)factory);

    if (desc) {
        SetStatus(QString::fromUtf8("Loaded: %1 %2 (view:%3)")
                  .arg(QString::fromUtf8(desc->name))
                  .arg(QString::fromUtf8(desc->version))
                  .arg(g_pluginView ? QString('Y') : QString('N')));
    }
}

/* --------------------------------------------------------------------------
 * Main window — plain QWidget subclass (lambdas instead of slots, so no
 * moc step is needed).
 * ------------------------------------------------------------------------ */
class PiQtHostWindow : public QWidget {
public:
    explicit PiQtHostWindow(QWidget* parent = NULL) : QWidget(parent)
    {
        setWindowTitle(QString::fromUtf8("pipluginframework — Test Host (Qt)"));
        resize(1280, 720);

        QVBoxLayout* root = new QVBoxLayout(this);

        QHBoxLayout* bar = new QHBoxLayout();
        QLabel* title = new QLabel(QString::fromUtf8("pipluginframework Qt host — embedding an ImGui plugin"));
        g_statusLabel = new QLabel(QString::fromUtf8("No plugin loaded"));
        QPushButton* loadBtn = new QPushButton(QString::fromUtf8("Load ImGui Plugin"));
        QPushButton* unloadBtn = new QPushButton(QString::fromUtf8("Unload"));

        bar->addWidget(title, 1);
        bar->addWidget(g_statusLabel, 1);
        bar->addWidget(loadBtn);
        bar->addWidget(unloadBtn);
        root->addLayout(bar);

        /* Native child widget: the embed container for plugin views. */
        g_embedContainer = new QWidget(this);
        g_embedContainer->setAttribute(Qt::WA_NativeWindow);
        g_embedContainer->setStyleSheet("background-color: #26262a;");
        g_embedContainer->installEventFilter(this);
        root->addWidget(g_embedContainer, 1);

        connect(loadBtn, &QPushButton::clicked, this, [this]() {
            LoadPlugin(ExeDirPath("pi_test_plugin_imgui.dll").c_str());
        });
        connect(unloadBtn, &QPushButton::clicked, this, [this]() {
            UnloadPlugin();
        });

        /* THE Qt-side "compatibility glue": drive plugin frames from the
         * Qt event loop. A 0-interval timer fires once per loop pass. */
        g_idleDriver = new QTimer(this);
        connect(g_idleDriver, &QTimer::timeout, this, []() {
            if (g_pluginView)
                pi_view_on_idle(g_pluginView);
        });
        g_idleDriver->start(0);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == g_embedContainer && event->type() == QEvent::Resize) {
            if (g_pluginView) {
                const QSize& s = g_embedContainer->size();
                if (s.width() > 0 && s.height() > 0)
                    pi_view_on_resize(g_pluginView, s.width(), s.height());
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent* event) override
    {
        UnloadPlugin();
        if (g_hostServices) {
            pi_iunknown_release((IPiUnknown*)g_hostServices);
            g_hostServices = NULL;
        }
        QWidget::closeEvent(event);
    }
};

/* --------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */
int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    PiQtHostWindow window;
    window.show();

    /* Auto-load for automated testing (command line = plugin DLL path). */
    if (argc > 1 && argv[1]) {
        LogStatus("auto-load: %s", argv[1]);
        LoadPlugin(argv[1]);
    }

    return app.exec();
}
