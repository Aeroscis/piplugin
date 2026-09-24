#include "pi_qt_test_plugin.h"

#include <stdio.h>

#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <chrono>

#include "pi_qt_view.h"
#include "pi_test_host_service.h"

#if PI_PLATFORM_WINDOWS
    #include <windows.h> /* GetEnvironmentVariableA：只给下面的 W-04 开关用 */
#endif

/* --------------------------------------------------------------------------
 * W-04：跨线程 pi_plugin_qt_view_post 的验收辅助开关
 *
 * 默认**关闭** —— 这个插件被一致性验收（run_selftest.ps1）、多插件宿主
 * （APP-08）、headless 冒烟等一堆用例共用，不能因为一个专项用例就改变它的
 * 正常行为。ctest `qt_view_post_from_worker_thread` 通过 ENVIRONMENT 打开它。
 * ------------------------------------------------------------------------ */
#define PI_PLUGIN_QT_POST_REPORT_MSG  ((uint32_t)0x2010u) /* wparam: 1 = 回调跑在宿主 GUI 线程 */
#define PI_PLUGIN_QT_WORKER_ALIVE_MSG ((uint32_t)0x2011u) /* wparam: 子线程发出的第几条 */

static bool PostThreadProbeEnabled()
{
    static bool const enabled = []()
    {
        char buf[8] = {0};
#if PI_PLATFORM_WINDOWS
        size_t n = GetEnvironmentVariableA("PI_PLUGIN_QT_TEST_POST_THREAD", buf, sizeof(buf));
        return n > 0 && buf[0] != '0';
#else
        const char* env = getenv("PI_PLUGIN_QT_TEST_POST_THREAD");
        if (!env)
            return false;
        size_t n = strlen(env);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        memcpy(buf, env, n);
        return n > 0 && buf[0] != '0';
#endif
    }();
    return enabled;
}

/* 同一份源码编出两个**不同的** Qt 插件 DLL（roadmap APP-08 的多插件同进程验收）。
 *
 * 必须是两个不同的模块：同一个 DLL 加载两次共享同一份静态数据，套件"每个模块
 * 一份进程级状态"的问题根本测不出来。变体之间只差：显示名、class GUID，以及
 * 心跳消息码（宿主据此分别确认两个插件都真的在跑）。
 *
 * 变体 A = pi_plugin_test_plugin_qt.dll（默认），变体 B = 由 CMake 传
 * PI_PLUGIN_TEST_QT_VARIANT_B 编出的 pi_plugin_test_plugin_qt2.dll。 */
#if defined(PI_PLUGIN_TEST_QT_VARIANT_B)
static const PiGuid QT_PLUGIN_CLASS_GUID =
    PI_GUID(0x7F83A101, 0x5C4D, 0x4E2A,
            0x91, 0xD3, 0x8A, 0xFC, 0x2E, 0xB1, 0x44, 0x00);
    #define PI_PLUGIN_QT_PLUGIN_NAME      "Qt Test Plugin B"
    #define PI_PLUGIN_QT_PLUGIN_HEARTBEAT ((uint32_t)0x2001u)
    #define PI_PLUGIN_QT_PLUGIN_VARIANT   "B"
#else
static const PiGuid QT_PLUGIN_CLASS_GUID =
    PI_GUID(0x7F83A100, 0x5C4D, 0x4E2A,
            0x91, 0xD3, 0x8A, 0xFC, 0x2E, 0xB1, 0x44, 0x00);
    #define PI_PLUGIN_QT_PLUGIN_NAME      "Qt Test Plugin"
    #define PI_PLUGIN_QT_PLUGIN_HEARTBEAT ((uint32_t)0x2000u)
    #define PI_PLUGIN_QT_PLUGIN_VARIANT   "A"
#endif

/* Animated colour block with its tick counter painted inside - the Qt
 * counterpart of the imgui plugin's "ImGui Heartbeat" window.
 *
 * It is deliberately a plain child widget rather than a Qt::Window: when the
 * plugin runs inside a non-Qt host, the adapter embeds the root widget as a
 * child of the host's container window, and Windows clips child windows to
 * their parent. A Qt::Window-flagged popup would be clipped to (or covered
 * by) the embedded widget and never show up. A child widget is composited by
 * Qt itself, so it is always visible inside the plugin's panel. */
class PiPluginHeartbeatBlock : public QWidget
{
public:
    explicit PiPluginHeartbeatBlock(QWidget* parent)
        : QWidget(parent)
        , m_tick(0)
        , m_r(0)
        , m_g(0)
        , m_b(0)
    {
        setMinimumSize(220, 120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setTick(int tick, int r, int g, int b)
    {
        m_tick = tick;
        m_r    = r;
        m_g    = g;
        m_b    = b;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(m_r, m_g, m_b));
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);

        QFont f = font();
        f.setPointSize(f.pointSize() + 2);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter,
                   QString::fromUtf8("Qt Event Loop - tick %1").arg(m_tick));
    }

private:
    int m_tick, m_r, m_g, m_b;
};

/* ==========================================================================
 * Factory
 * ======================================================================== */
IPiPluginFactoryVtbl const QtPluginFactory::s_factory_vtbl = {
    {&QtPluginFactory::Qi_Factory, &pi_refcounted_add_ref, &pi_refcounted_release},
    &QtPluginFactory::GetDescriptor,
    &QtPluginFactory::GetClassCount,
    &QtPluginFactory::GetClassGuid,
    &QtPluginFactory::CreateInstance
};

QtPluginFactory::QtPluginFactory()
{
    pi_refcounted_init_with_destroy(&m_base,
                                    (IPiUnknownVtbl const*)&s_factory_vtbl,
                                    &pi_plugin_cpp_destroy<QtPluginFactory>);

    m_descriptor.name        = PI_PLUGIN_QT_PLUGIN_NAME;
    m_descriptor.vendor      = "piplugin";
    m_descriptor.version     = "1.2.0";
    m_descriptor.category    = "UI/Test";
    m_descriptor.api_version = PI_PLUGIN_API_VERSION;

    /* LV2-style capability declaration:
     *  - this plugin provides a GUI view
     *  - it optionally uses the host's UI services (runs headless without) */
    m_capabilities[0].iid   = PI_PLUGIN_IID_PLUGIN_VIEW;
    m_capabilities[0].flags = PI_PLUGIN_CAP_PROVIDES;
    m_capabilities[1].iid   = PI_PLUGIN_IID_HOST_UI;
    m_capabilities[1].flags = PI_PLUGIN_CAP_OPTIONAL;

    m_descriptor.capabilities     = m_capabilities;
    m_descriptor.capability_count = 2;

    /* 自由元数据（roadmap APP-04）：描述性事实不该硬塞进 capabilities。
     * `pi.` 前缀是框架保留区，所以这里用 com.example.* 这种自有前缀。
     * 宿主侧用 pi_plugin_descriptor_find_property() 读它们（见 headless 测试宿主）。 */
    m_properties[0].key   = "com.example.kind";
    m_properties[0].value = "qt-plugin";
    m_properties[1].key   = "com.example.ui.toolkit";
    m_properties[1].value = "qt5";
    m_properties[2].key   = "com.example.variant";
    m_properties[2].value = PI_PLUGIN_QT_PLUGIN_VARIANT;

    m_descriptor.properties     = m_properties;
    m_descriptor.property_count = 3;
}

uint32_t PI_CALL QtPluginFactory::AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}
uint32_t PI_CALL QtPluginFactory::Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}
PiResult PI_CALL QtPluginFactory::Qi_Factory(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    QtPluginFactory* me = (QtPluginFactory*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY))
    {
        *out = me;
        me->m_base.unk.lpVtbl->pi_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}
PiPluginDescriptor const* PI_CALL QtPluginFactory::GetDescriptor(void* self_ptr)
{
    return &((QtPluginFactory*)self_ptr)->m_descriptor;
}
uint32_t PI_CALL QtPluginFactory::GetClassCount(void* self_ptr)
{
    (void)self_ptr;
    return 1;
}
PiResult PI_CALL QtPluginFactory::GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid)
{
    (void)self_ptr;
    if (index != 0 || !guid)
    {
        return PI_E_INVALIDARG;
    }
    *guid = QT_PLUGIN_CLASS_GUID;
    return PI_OK;
}
PiResult PI_CALL QtPluginFactory::CreateInstance(void* self_ptr, PiGuid const* guid,
                                                 IPiPluginHostServices* host, IPiPluginBase** out)
{
    (void)self_ptr;
    if (!guid || !out)
    {
        return PI_E_INVALIDARG;
    }
    /* 终审约定 2.4：失败时一律把 *out 置 NULL。这里原本漏了，
     * 被 tests/unit 的 ECO-08 负向用例抓出来。 */
    *out = nullptr;
    if (!pi_guid_equal(guid, &QT_PLUGIN_CLASS_GUID))
    {
        return PI_E_NOINTERFACE;
    }
    QtPlugin* plugin = new QtPlugin();
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

/* ==========================================================================
 * Plugin
 * ======================================================================== */
IPiPluginBaseVtbl const QtPlugin::s_base_vtbl = {
    {&QtPlugin::Qi_PluginBase, &pi_refcounted_add_ref, &pi_refcounted_release},
    &QtPlugin::Init,
    &QtPlugin::Term,
    &QtPlugin::GetView
};

QtPlugin::QtPlugin()
    : m_view(NULL)
    , m_uiThread(nullptr)
    , m_postWorkerStop(false)
    , m_postProbeDone(false)
    , m_postProbeRuns(0)
{
    pi_refcounted_init_with_destroy(&m_base,
                                    (IPiUnknownVtbl const*)&s_base_vtbl,
                                    &QtPlugin::Destroy);
}

QtPlugin::~QtPlugin()
{
    /* 没有一行 release：m_host / m_hostUI 是 PiPluginPtr（C++ RAII 层，pi_cpp.h），
     * 析构顺序自动把这两个接口引用放掉。
     * 但后台线程必须显式收掉：它还会调用套件与宿主服务，跑在模块卸载之后就是
     * 调到已卸载的内存。 */
    StopPostWorker();
}

PiResult QtPlugin::Initialize(IPiPluginHostServices* host)
{
    /* 幂等：宿主会在 create_instance 之后再调一次 pi_plugin_initialize（框架的便捷
     * 加载 pi_plugin_host_create_plugin 与宿主 kit L0 都是这个顺序），而本工厂的
     * CreateInstance 里也已经初始化过。没有这道闸，同一个宿主指针会被 add-ref
     * 两次，而 m_hostUI 会被第二次 QI 的新包装覆盖 —— 旧包装就泄漏了。 */
    if (m_host)
    {
        return PI_OK;
    }

    if (host)
    {
        /* 入参 host 是**借用**（冻结约定 2.3：create_instance 不为它 add-ref），
         * 插件要留住就必须自己加一次引用 —— PiPluginPtr::add_ref() 表达的正是这件事。 */
        m_host = PiPluginPtr<IPiPluginHostServices>::add_ref(host);

        /* Discover whether this is a GUI host. A headless host (task
         * server) returns PI_E_NOINTERFACE and we simply skip UI.
         * qi_to<T>() 用 PiPluginIidOf<T> 里的框架 IID；查询失败时返回空句柄，
         * 所以"宿主是不是 GUI 宿主"这一个判断就是句柄的真假。 */
        m_hostUI = m_host.qi_to<IPiPluginHostUI>();

        /* 通道 B（roadmap APP-01）：宿主可以挂 app 自定义服务，插件侧只是对
         * 同一个宿主对象 QI 一次 —— 没有任何新 API，也没有新 vtbl。
         * 宿主没提供时 QI 失败，我们照常运行（对应 descriptor 里声明 OPTIONAL），
         * 这正是"插件对宿主能力做运行时协商"的另一半。
         * 自定义接口没有 PiPluginIidOf 特化，所以这里显式给 IID。 */
        PiPluginPtr<IPiPluginTestHostService> svc =
            m_host.qi_to<IPiPluginTestHostService>(PI_PLUGIN_TEST_IID_HOST_SERVICE);
        if (svc)
        {
            /* 先告诉宿主"我找到你的服务了"，再问它一共收到过几条插件消息：
             * 这个值只有宿主知道，所以打印出来就等于证明 QI + 调用都通了。 */
            pi_plugin_host_post_message(m_host.get(), PI_PLUGIN_TEST_MSG_HOST_SERVICE, 1u, 0);
            printf("[qt plugin] host-provided service: host=%s, "
                   "the host has seen %u plugin message(s)\n",
                   pi_plugin_test_host_service_name(svc.get()),
                   (unsigned)pi_plugin_test_host_service_messages_seen(svc.get()));
        }
        else
        {
            printf("[qt plugin] host provides no app-defined service "
                   "(framework services only)\n");
        }
    }
    return PI_OK;
}

PiResult QtPlugin::Terminate()
{
    /* 先把后台线程收掉（W-04 的探针）：它还会调用套件与宿主服务，
     * 让它活过模块卸载就是调到已卸载的内存。 */
    StopPostWorker();

    /* The adapter kit owns the Qt side, and it needs an explicit "we are
     * about to go away" signal: destroying the widget and the QApplication
     * runs code inside THIS module, so it must all be finished before the
     * host can call FreeLibrary() on us. A host that follows the documented
     * order (detach + release the view first) is already safe; calling this
     * makes the teardown complete regardless of how the host unwinds - which
     * is what stops hosts that just drop the module from crashing inside Qt
     * during unload.
     *
     * _owner(this)：套件是 SHARED 的，不带 owner 的 pi_plugin_qt_view_shutdown() 是
     * "拆掉进程里所有 Qt 视图"的大锤 —— 在多 Qt 插件进程里会把别的插件的界面
     * 一起拆掉。只拆自己的（tests/test_host_multi 正是断言这一点）。 */
    pi_plugin_qt_view_shutdown_owner(this);
    m_view = NULL;
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
    QObject::connect(slider, &QSlider::valueChanged, [me, label](int v)
                     {
        label->setText(QString::fromUtf8("Qt Plugin - Value: %1").arg(v));
        if (me->m_host){
            pi_plugin_host_post_message(me->m_host.get(), 0x1000, (uintptr_t)v, 0);
} });
    layout->addWidget(slider);

    QPushButton* btn = new QPushButton(QString::fromUtf8("Reset"), w);
    QObject::connect(btn, &QPushButton::clicked, [slider]()
                     { slider->setValue(50); });
    layout->addWidget(btn);
    layout->addStretch();

    /* Animated heartbeat block (proof that the Qt loop keeps running): a
     * colour block whose fill cycles and whose tick counter is drawn inside
     * it - same look as the imgui plugin's heartbeat window. */
    PiPluginHeartbeatBlock* heartbeat = new PiPluginHeartbeatBlock(w);
    layout->addWidget(heartbeat);

    int*    heartbeatTick  = new int(0);
    QTimer* heartbeatTimer = new QTimer(heartbeat);
    QObject::connect(heartbeatTimer, &QTimer::timeout, [heartbeat, heartbeatTick]()
                     {
        (*heartbeatTick)++;
        int p = (*heartbeatTick) % 100, r = 0, g = 0, b = 0;
        if (p < 33)      { r = 255 - p*7;  g = p*7;          b = 0; }
        else if (p < 66) { r = 0;          g = 255-(p-33)*7; b = (p-33)*7; }
        else             { r = (p-66)*7;   g = 0;            b = 255-(p-66)*7; }
        heartbeat->setTick(*heartbeatTick, r, g, b); });
    heartbeatTimer->start(40);

    /* Reports a tick to the host periodically (变体 A 用 0x2000、变体 B 用 0x2001）
     * so an automated run can tell "the host really drives the Qt event loop" from
     * "the UI was painted once and then froze" —— 多插件同进程的验收据此分别确认
     * 两个插件都在跑（见 tests/test_host_multi）。
     * 第一帧就报一次（tick 1），否则 40ms 的定时器要等到 25 tick（约 1 秒）才有
     * 第一条消息，短跑的自检会看不到任何证据。 */
    QObject::connect(heartbeatTimer, &QTimer::timeout, [me, heartbeatTick]()
                     {
        if (me->m_host && ((*heartbeatTick % 25) == 1)){
            pi_plugin_host_post_message(me->m_host.get(), PI_PLUGIN_QT_PLUGIN_HEARTBEAT,
                                 (uintptr_t)*heartbeatTick, 0);
} });

    /* W-04：create_widget 是宿主 GUI 线程上唯一确定会被调到的插件代码，所以
     * 在这里记下"UI 线程是哪条"，并按开关决定要不要起那条后台线程。 */
    me->m_uiThread = QThread::currentThread();
    me->StartPostWorkerIfEnabled();

    return w;
}

/* --------------------------------------------------------------------------
 * W-04：从**子线程**调用 pi_plugin_qt_view_post 与 pi_plugin_host_post_message
 * ------------------------------------------------------------------------ */
void QtPlugin::PostProbe(void* user_data)
{
    QtPlugin*  me           = (QtPlugin*)user_data;
    /* 套件契约：pi_plugin_qt_view_post 的回调在宿主 GUI 线程上执行，所以这里可以
     * 安全地碰 Qt —— 下面这一句就是"回调跑在正确的线程上"的直接证据。 */
    bool const on_ui_thread = (me->m_uiThread != nullptr) &&
                              (QThread::currentThread() == me->m_uiThread);
    me->m_postProbeRuns.fetch_add(1);
    if (me->m_host)
    {
        pi_plugin_host_post_message(me->m_host.get(), PI_PLUGIN_QT_POST_REPORT_MSG,
                                    on_ui_thread ? 1u : 0u, 0);
    }
    me->m_postProbeDone = true;
}

void QtPlugin::PostWorkerMain(QtPlugin* me)
{
    uintptr_t seq = 0;
    /* attach 之前的投递会被套件丢弃（那是设计），所以重试到回调真的跑过为止，
     * 最多约 5 秒；宿主每帧 pump，正常情况下第一两轮就成。 */
    for (int i = 0; i < 250; ++i)
    {
        if (me->m_postWorkerStop || me->m_postProbeDone)
        {
            break;
        }

        /* 1) 普通跨线程 post_message：消息**就在子线程上**到达宿主回调，
         *    宿主按契约自己负责 marshal 到它的事件循环。 */
        if (me->m_host)
        {
            pi_plugin_host_post_message(me->m_host.get(), PI_PLUGIN_QT_WORKER_ALIVE_MSG,
                                        ++seq, 0);
        }

        /* 2) 套件的跨线程 pi_plugin_qt_view_post：套件负责 marshal 到宿主 GUI 线程 */
        IPiPluginView* view = me->m_view.load();
        if (view)
        {
            pi_plugin_qt_view_post(view, &QtPlugin::PostProbe, me);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void QtPlugin::StartPostWorkerIfEnabled()
{
    if (!PostThreadProbeEnabled())
    {
        return;
    }
    if (m_postWorker.joinable())
    {
        return;
    }
    m_postWorkerStop = false;
    m_postProbeDone  = false;
    m_postWorker     = std::thread(&QtPlugin::PostWorkerMain, this);
}

void QtPlugin::StopPostWorker()
{
    m_postWorkerStop = true;
    if (m_postWorker.joinable())
    {
        m_postWorker.join();
    }
}

void QtPlugin::Retain(void* user_data)
{
    QtPlugin* me = (QtPlugin*)user_data;
    if (me)
    {
        me->m_base.unk.lpVtbl->pi_add_ref(me);
    }
}

void QtPlugin::Release(void* user_data)
{
    QtPlugin* me = (QtPlugin*)user_data;
    if (me)
    {
        me->m_base.unk.lpVtbl->pi_release(me);
    }
}

PiResult PI_CALL QtPlugin::Qi_PluginBase(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    QtPlugin* me = (QtPlugin*)self_ptr;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE))
    {
        *out = me;
        me->m_base.unk.lpVtbl->pi_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}
PiResult PI_CALL QtPlugin::Init(void* self_ptr, IPiPluginHostServices* host)
{
    return ((QtPlugin*)self_ptr)->Initialize(host);
}
PiResult PI_CALL QtPlugin::Term(void* self_ptr)
{
    return ((QtPlugin*)self_ptr)->Terminate();
}
PiResult PI_CALL QtPlugin::GetView(void* self_ptr, IPiPluginView** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    QtPlugin* me = (QtPlugin*)self_ptr;
    /* On a headless host we advertise no view at all. */
    if (!me->m_hostUI)
    {
        *out = NULL;
        return PI_E_NOINTERFACE;
    }

    /* All Qt integration is delegated to the adapter kit. */
    PiPluginQtViewDesc desc = {};
    desc.create_widget      = &QtPlugin::CreateUi;
    desc.retain             = &QtPlugin::Retain;
    desc.release            = &QtPlugin::Release;
    desc.user_data          = me;
    PiResult hr             = pi_plugin_qt_view_create(&desc, out);
    if (PI_SUCCEEDED(hr))
    {
        me->m_view = *out; /* weak: host owns the ref */
    }
    return hr;
}
