/*
 * piplugin example - the HOST half of the Qt-host direct integration
 *
 * A host that is ITSELF a Qt program, loading a Qt plugin. Read
 * docs/tutorial/qt-host-direct.md; the short version:
 *
 *   - the host owns the process's QApplication (created here in main());
 *   - it never touches the Qt adapter kit (piplugin_qt): that kit wants to
 *     create the QApplication itself and fails when one already exists;
 *   - the integration is ONE line - the plugin hands over a QWidget* and the
 *     host puts it in its own QLayout. The host's event loop does the rest:
 *     no native container, no pi_on_idle(), no thread marshalling, and nothing
 *     Windows-specific;
 *   - the widget must be destroyed BEFORE the plugin module is unloaded (its
 *     signal/slot bodies live there), which is why the protocol has a
 *     destroy_widget() slot at all. See TearDown() below for the order.
 *
 * Three steps: see README.md. Two ways to run:
 *   pi_example_qt_direct_host.exe [plugin.dll]              interactive
 *   pi_example_qt_direct_host.exe --self-test [plugin.dll]  scriptable, exit code
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"          /* host kit L0: load / gate / unload order */
#include "pi_qt_direct_protocol.h"    /* the APP's protocol (channel A)          */

#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtCore/QTimer>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

/* CMake defines this for the platform's plugin file name. */
#ifndef PI_QT_DIRECT_DEFAULT_PLUGIN
#  define PI_QT_DIRECT_DEFAULT_PLUGIN "pi_example_plugin_qt_direct.dll"
#endif

/* --------------------------------------------------------------------------
 * Host state: everything the callbacks and the teardown order need
 * ------------------------------------------------------------------------ */
struct Host {
    QMainWindow*         window;
    QVBoxLayout*         layout;
    QLabel*              status;

    IPiHostServices*     services;      /* owned (refcount 1) */
    PiPluginHostSession* session;       /* owned by us        */
    uint32_t             slot;

    IQtDirectWidget*     widget_ifc;    /* add-ref'd protocol pointer */
    QWidget*             plugin_widget; /* the widget the plugin handed over */
    int                  messages;
    int                  failures;
    bool                 torn_down;

    Host() : window(nullptr), layout(nullptr), status(nullptr),
             services(nullptr), session(nullptr),
             slot(PI_HOST_SESSION_INVALID_SLOT),
             widget_ifc(nullptr), plugin_widget(nullptr),
             messages(0), failures(0), torn_down(false) {}

    void Check(bool ok, const char* what)
    {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    }

    /* The teardown order this example exists to teach (idempotent):
     *   widget -> protocol pointer -> unload (seven steps) -> session -> services */
    void TearDown()
    {
        if (torn_down) return;
        torn_down = true;

        /* (1) the plugin's widget FIRST. Its signal/slot bodies are code in the
         *     plugin's module; step (3) unmaps that module. */
        if (plugin_widget) {
            if (widget_ifc)
                pi_qt_direct_destroy_widget(widget_ifc, plugin_widget);
            plugin_widget = nullptr;
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        /* (2) ours to release: we add-ref'd it in QueryInterface */
        if (widget_ifc) {
            pi_iunknown_release((IPiUnknown*)widget_ifc);
            widget_ifc = nullptr;
        }

        /* (3) the seven-step unload sequence, module unmap included */
        if (session && slot != PI_HOST_SESSION_INVALID_SLOT) {
            pi_host_session_unload(session, slot);
            Check(pi_host_session_is_loaded(session, slot) == 0, "slot is empty after unload");
            slot = PI_HOST_SESSION_INVALID_SLOT;
        }

        if (session) { pi_host_session_destroy(session); session = nullptr; }
        if (services) { pi_iunknown_release((IPiUnknown*)services); services = nullptr; }
    }
};

/* The plugin posts messages here (pi_host_post_message). Same thread as the GUI
 * in this example, so touching widgets is legal; a plugin posting from its own
 * worker thread would need to marshal (see docs/tutorial/adapters.md). */
static void OnHostMessage(void* user_data, uint32_t msg, uintptr_t wparam, intptr_t lparam)
{
    Host* host = (Host*)user_data;
    (void)lparam;
    if (!host) return;
    ++host->messages;
    printf("  [host] message from the plugin: msg=0x%04X wparam=%llu\n",
           msg, (unsigned long long)wparam);
    if (host->status)
        host->status->setText(QString::fromUtf8("messages from the plugin: %1").arg(host->messages));
}

static void OnSessionLog(void* user_data, const char* message)
{
    (void)user_data;
    printf("  [kit] %s\n", message);
}

int main(int argc, char** argv)
{
    /* Copy the arguments BEFORE QApplication runs: Qt consumes its own options
     * and does not promise to leave the rest where we put them. */
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(argv[i] ? argv[i] : "");

    bool        self_test    = false;
    int         self_test_ms = 1200;
    std::string plugin;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--self-test")               self_test = true;
        else if (args[i] == "--ms" && i + 1 < args.size()) self_test_ms = atoi(args[++i].c_str());
        else if (!args[i].empty() && args[i][0] != '-')    plugin = args[i];
    }
    if (plugin.empty()) plugin = PI_QT_DIRECT_DEFAULT_PLUGIN;

    /* THE line that makes this a "Qt host": the host owns the QApplication. */
    QApplication app(argc, argv);

    printf("== piplugin Qt host, direct integration ==\n");
    printf("plugin: %s\n", plugin.c_str());
    printf("mode:   %s\n", self_test ? "self-test" : "interactive (close the window to unload)");

    Host host;
    PiResult hr;

    /* --- 1) the host's own UI: a window and a layout IT owns --------------- */
    host.window = new QMainWindow();
    host.window->setWindowTitle(QString::fromUtf8("piplugin - Qt host + Qt plugin (direct)"));
    QWidget* central = new QWidget(host.window);
    host.layout = new QVBoxLayout(central);
    host.layout->addWidget(new QLabel(QString::fromUtf8(
        "This window and its layout belong to the HOST.\n"
        "The widget below was created by the PLUGIN and adopted by that layout.")));
    host.status = new QLabel(QString::fromUtf8("messages from the plugin: 0"));
    host.layout->addWidget(host.status);

    QPushButton* unload_button = new QPushButton(QString::fromUtf8(
        "Unload the plugin (destroy widget -> unload module)"));
    QObject::connect(unload_button, &QPushButton::clicked, [&host]() {
        host.TearDown();
        if (host.status)
            host.status->setText(QString::fromUtf8("plugin unloaded - its widget is gone"));
    });
    host.layout->addWidget(unload_button);

    host.window->setCentralWidget(central);
    host.window->resize(560, 380);
    host.window->show();

    /* --- 2) host services + session ---------------------------------------- */
    /* Headless on purpose: the plugin gets NO native container. Its UI travels
     * as a QWidget over the app protocol, so PI_IID_HOST_UI is not in play. */
    hr = pi_host_services_create_default(&OnHostMessage, &host, PI_INVALID_WINDOW,
                                        &host.services);
    if (PI_FAILED(hr)) { printf("FATAL: host services (hr=%d)\n", (int)hr); return 1; }

    hr = pi_host_session_create(host.services, &host.session);
    if (PI_FAILED(hr)) { printf("FATAL: session (hr=%d)\n", (int)hr); return 1; }
    pi_host_session_set_logger(host.session, &OnSessionLog, nullptr);

    /* The app's requirement, declared BEFORE any load: every plugin in this
     * host's ecosystem must implement the app's widget protocol. A Qt plugin
     * written for the adapter kit is rejected HERE, by name - instead of loading
     * cleanly and then silently never showing UI (the failure mode this example
     * and docs/tutorial/qt-host-direct.md exist to remove). */
    pi_host_session_require(host.session, &PI_QT_DIRECT_WIDGET_IID);

    /* --- 3) load: module + version gate + capability gate + instantiate ----- */
    hr = pi_host_session_load(host.session, plugin.c_str(), &host.slot);
    if (PI_FAILED(hr)) {
        printf("load failed (hr=%d): %s\n", (int)hr, pi_host_session_last_error(host.session));
        printf("RESULT: FAIL\n");
        host.TearDown();
        delete host.window;
        return 1;
    }
    host.Check(pi_host_session_is_loaded(host.session, host.slot) != 0, "plugin loaded");

    {
        const PiPluginDescriptor* desc = pi_host_session_get_descriptor(host.session, host.slot);
        printf("loaded: %s %s (%s)\n",
               (desc && desc->name) ? desc->name : "?",
               (desc && desc->version) ? desc->version : "",
               (desc && desc->category) ? desc->category : "-");
    }

    /* --- 4) the integration: ask for a QWidget*, hand it to our layout ------ */
    {
        IPiPluginBase* plugin_base = pi_host_session_get_plugin(host.session, host.slot); /* borrowed */
        if (plugin_base) {
            pi_iunknown_query_interface((IPiUnknown*)plugin_base, &PI_QT_DIRECT_WIDGET_IID,
                                        (void**)&host.widget_ifc);
        }
    }
    host.Check(host.widget_ifc != nullptr, "plugin implements the app's widget protocol");

    if (host.widget_ifc)
        host.plugin_widget = pi_qt_direct_create_widget(host.widget_ifc);
    host.Check(host.plugin_widget != nullptr, "plugin created a widget for us");

    if (host.plugin_widget) {
        host.layout->addWidget(host.plugin_widget);   /* <- the whole integration */
        QCoreApplication::processEvents();
        host.Check(host.plugin_widget->isVisible(),
                   "the adopted widget is visible inside the host's layout");
    }

    /* --- 5) the event loop is the host's own; no pumping API is involved ---- */
    if (self_test) {
        QTimer::singleShot(self_test_ms / 3, [&host]() {
            QPushButton* button = host.plugin_widget
                                ? host.plugin_widget->findChild<QPushButton*>() : nullptr;
            if (!button) { host.Check(false, "found the plugin's button to click"); return; }
            printf("  [host] clicking the plugin's button...\n");
            button->click();          /* plugin code -> pi_host_post_message -> OnHostMessage */
        });
        QTimer::singleShot(self_test_ms, [&host]() {
            host.Check(host.messages == 1, "plugin -> host message arrived end to end");
            host.TearDown();          /* incl. "slot is empty after unload" */
            QCoreApplication::quit();
        });
    }

    app.exec();

    host.TearDown();                  /* interactive path lands here; idempotent */
    if (host.window) { delete host.window; host.window = nullptr; }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    printf("RESULT: %s\n", host.failures ? "FAIL" : "PASS");
    return host.failures ? 1 : 0;
}
