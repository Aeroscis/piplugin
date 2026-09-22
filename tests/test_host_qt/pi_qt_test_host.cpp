/*
 * piplugin — Test Host (Qt / Widgets)
 *
 * The mirror twin of the imgui test host: a Qt application that embeds
 * an ImGui-based plugin. Demonstrates how a Qt host merges foreign
 * plugin UIs into ITS event loop:
 *
 *   1. A PiPluginEmbedArea (host kit L1) is the embed container; the host
 *      creates it and puts it in its own layout, the kit makes it an
 *      embed host (native window + attach + resize forwarding).
 *   2. The host's own QTimer (fires on every event-loop iteration) drives
 *      plugin frames — the cadence is the HOST's decision.
 *
 * The host knows nothing about Dear ImGui (or Qt-as-a-plugin): the whole
 * compatibility burden lives in the plugin-side adapter kits.
 *
 * 分层：宿主侧机制来自宿主 kit（L0 会话 = 加载/门禁/实例化/卸载序列；
 * L1 嵌入区域 = 容器/attach/resize 转发）。本文件只剩"这台宿主的窗口、
 * 布局、样式与帧时钟长什么样"——容器是谁、在哪、多大、怎么美化，全归宿主。
 */
#include "piplugin/pi_plugin.h"
/* C++ RAII 层（可选头）：宿主服务对象用 PiPtr 持有，退出路径上不用手写 release。 */
#include "piplugin/pi_cpp.h"
#include "pi_host_session.h"
#include "pi_host_embed_area.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
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
static PiPtr<IPiHostServices> g_hostServices;   /* RAII：析构即 release */
static PiPluginHostSession*   g_session      = NULL;
static uint32_t               g_slot         = PI_HOST_SESSION_INVALID_SLOT;

static PiPluginEmbedArea* g_embedArea  = NULL;   /* 宿主创建、宿主摆位、宿主美化 */
static QLabel*            g_statusLabel = NULL;
static QTimer*            g_idleDriver  = NULL;

static void HostMessageProc(void* user_data, uint32_t msg,
                            uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)lparam;
    LogStatus("[plugin message] msg=0x%04X wparam=%llu",
              msg, (unsigned long long)wparam);
}

/* kit 的步骤日志 -> 本宿主的日志文件（去向由宿主决定） */
static void SessionLogProc(void* user_data, const char* message)
{
    (void)user_data;
    LogStatus("%s", message);
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
 * Plugin load / unload（机制全部在 kit 里；这里只做本宿主的 UI 反应）
 * ------------------------------------------------------------------------ */
static void UnloadPlugin()
{
    LogStatus("unload: begin");
    if (g_session && g_slot != PI_HOST_SESSION_INVALID_SLOT) {
        /* 先解除嵌入区域的绑定，再让 session 走七步卸载序列：
         * 解绑后本控件不再持有任何指向该插件的视图（也就不会在卸载后再转发 resize） */
        if (g_embedArea) g_embedArea->detachBinding();
        pi_host_session_unload(g_session, g_slot);
        g_slot = PI_HOST_SESSION_INVALID_SLOT;
    }
    SetStatus(QString::fromUtf8("No plugin loaded"));
    LogStatus("unload: done");
}

static void LoadPlugin(const char* dllPath)
{
    UnloadPlugin();

    if (!g_hostServices) {
        /* Create the host services object with the embed container's
         * native window, so IPiHostUI is exposed to plugins.
         * put() 给出参地址：句柄接管 create_default 返回的引用（已 add-ref），
         * 失败时框架会把 *out 置 NULL，句柄保持为空。 */
        if (PI_FAILED(pi_host_services_create_default(&HostMessageProc, NULL,
                                                      (PiNativeWindow)g_embedArea->winId(),
                                                      g_hostServices.put()))) {
            SetStatus(QString::fromUtf8("Host services unavailable"));
            return;
        }
    }
    if (!g_session) {
        if (PI_FAILED(pi_host_session_create(g_hostServices.get(), &g_session))) {
            SetStatus(QString::fromUtf8("Host session unavailable"));
            return;
        }
        pi_host_session_set_logger(g_session, &SessionLogProc, NULL);
    }

    uint32_t slot = PI_HOST_SESSION_INVALID_SLOT;
    PiResult hr = pi_host_session_load(g_session, dllPath, &slot);
    if (PI_FAILED(hr)) {
        /* 失败原因（含 pi_module_load 的错误描述与双向门禁的拒绝理由）由 kit 给出 */
        const QString why = QString::fromLocal8Bit(pi_host_session_last_error(g_session));
        if (hr == PI_E_MISSINGCAPABILITY)
            SetStatus(QString::fromUtf8("Rejected: %1").arg(why));
        else
            SetStatus(QString::fromUtf8("Load failed: %1").arg(why));
        return;
    }
    g_slot = slot;

    const PiPluginDescriptor* desc = pi_host_session_get_descriptor(g_session, slot);

    /* 嵌入：区域是本宿主创建并摆位的，L1 kit 负责让它成为 embed host */
    const bool view_attached = PI_SUCCEEDED(g_embedArea->attach(g_session, slot, true));

    if (desc) {
        SetStatus(QString::fromUtf8("Loaded: %1 %2 (view:%3)")
                  .arg(QString::fromUtf8(desc->name))
                  .arg(QString::fromUtf8(desc->version))
                  .arg(view_attached ? QString('Y') : QString('N')));

        /* 自由元数据（roadmap APP-04）：按键取值，写进日志便于自动化断言。 */
        for (uint32_t i = 0; i < desc->property_count; ++i) {
            const PiPluginProperty* prop = &desc->properties[i];
            LogStatus("property: %s = %s",
                      prop->key ? prop->key : "(null)",
                      prop->value ? prop->value : "(null)");
        }
        {
            const char* kind = pi_descriptor_find_property(desc, "com.example.kind");
            LogStatus("property com.example.kind = %s", kind ? kind : "(absent)");
        }
    } else {
        SetStatus(QString::fromUtf8("Loaded (no descriptor)"));
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
        setWindowTitle(QString::fromUtf8("piplugin — Test Host (Qt)"));
        resize(1280, 720);

        QVBoxLayout* root = new QVBoxLayout(this);

        QHBoxLayout* bar = new QHBoxLayout();
        QLabel* title = new QLabel(QString::fromUtf8("piplugin Qt host — embedding an ImGui plugin"));
        g_statusLabel = new QLabel(QString::fromUtf8("No plugin loaded"));
        QPushButton* loadBtn = new QPushButton(QString::fromUtf8("Load ImGui Plugin"));
        QPushButton* unloadBtn = new QPushButton(QString::fromUtf8("Unload"));

        bar->addWidget(title, 1);
        bar->addWidget(g_statusLabel, 1);
        bar->addWidget(loadBtn);
        bar->addWidget(unloadBtn);
        root->addLayout(bar);

        /* 嵌入区域：本宿主创建、放进自己的布局、并自己决定外观（样式表归宿主；
         * L1 kit 不做任何视觉决策，所以 QSS 完全由这里说了算）。 */
        g_embedArea = new PiPluginEmbedArea(this);
        g_embedArea->setStyleSheet("background-color: #26262a;");
        root->addWidget(g_embedArea, 1);

        connect(loadBtn, &QPushButton::clicked, this, [this]() {
            LoadPlugin(ExeDirPath("pi_test_plugin_imgui.dll").c_str());
        });
        connect(unloadBtn, &QPushButton::clicked, this, [this]() {
            UnloadPlugin();
        });

        /* THE Qt-side cadence: drive plugin frames from the Qt event loop.
         * A 0-interval timer fires once per loop pass. 何时 pump 归宿主决定
         * （L1 也提供内部定时器，但默认关闭，避免和宿主自己的时钟打架）。 */
        g_idleDriver = new QTimer(this);
        connect(g_idleDriver, &QTimer::timeout, this, []() {
            if (g_embedArea) g_embedArea->driveIdle();
        });
        g_idleDriver->start(0);
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        UnloadPlugin();
        if (g_session) { pi_host_session_destroy(g_session); g_session = NULL; }
        g_hostServices.reset();   /* 引用归零即销毁，不再手写 release */
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
