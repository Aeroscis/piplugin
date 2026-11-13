/*
 * piplugin - Test Plugin (Qt-based)
 *
 * With the Qt adapter kit (piplugin_qt) the plugin contains NO
 * Qt integration code at all: no QApplication management, no embedding,
 * no event-loop plumbing. It only provides a widget factory and the
 * plugin lifecycle.
 *
 * Since 0.6 the GUI is a separate capability interface: IPiPluginBase carries
 * lifecycle only, and QueryInterface(PI_PLUGIN_IID_VIEW_PROVIDER) hands out a
 * small IPiPluginViewProvider tear-off (the same containment pattern the pure-C
 * test plugins use for their service / sink objects). The provider enumerates
 * the views this plugin has - here exactly one, id "main" - and builds them on
 * demand.
 */
#ifndef PI_PLUGIN_QT_TEST_PLUGIN_H
#define PI_PLUGIN_QT_TEST_PLUGIN_H

#include "piplugin/pi_plugin.h"
/* C++ RAII 层（可选头，见 docs/design/interfaces.md）：PiPluginPtr / PiPluginUniqueModule /
 * pi_plugin_cpp_destroy。本插件只用到 PiPluginPtr —— 宿主指针与两个可选接口的生命周期
 * 全部交给句柄，构造函数与析构函数里一行 release 都不用写。 */
#include <atomic>
#include <thread>

#include "piplugin/pi_cpp.h"

class QWidget;
class QThread;

class QtPluginViewProvider;

class QtPlugin
{
public:
    friend class QtPluginFactory;
    friend class QtPluginViewProvider;

    QtPlugin();
    ~QtPlugin();

    PiResult Initialize(IPiPluginHostServices* host);
    PiResult Terminate();

    static PiResult PI_CALL Qi_PluginBase(void* self_ptr, PiGuid const* iid, void** out);
    static PiResult PI_CALL Init(void* self_ptr, IPiPluginHostServices* host);
    static PiResult PI_CALL Term(void* self_ptr);

    /* Widget factory handed to the Qt adapter kit. Called on the Qt
     * runtime thread; builds the whole demo UI. */
    static QWidget* CreateUi(void* user_data);

    /* Refcount thunks so the adapter keeps this object alive while the
     * widget (whose signal lambdas capture `this`) still exists. */
    static void Retain(void* user_data);
    static void Release(void* user_data);

private:
    static void Destroy(void* self_ptr) { delete static_cast<QtPlugin*>(self_ptr); }

    /* 造一个 Qt 套件的视图（provider 的 pi_plugin_get_view 落到这里）。
     * On a headless host (no IPiPluginHostUI) there is nothing to build;
     * the provider is not even exposed in that case, see Qi_PluginBase. */
    PiResult CreateView(IPiPluginView** out);

    /* W-04：跨线程 pi_plugin_qt_view_post 的验收辅助（默认关闭，见 .cpp 里的开关）。
     * 起一条后台线程，从那条线程调用套件的 pi_plugin_qt_view_post()，并把"回调最终跑在
     * 哪条线程"报给宿主。做这件事的地方是控件的 create_widget 回调 —— 那是宿主
     * GUI 线程上唯一确定会被调到的插件代码。 */
    void StartPostWorkerIfEnabled();
    void StopPostWorker();

    static void PostProbe(void* user_data);
    static void PostWorkerMain(QtPlugin* me);

    PiRefCountedBase               m_base; /* MUST be first data member */
    static IPiPluginBaseVtbl const s_base_vtbl;

    PiPluginPtr<IPiPluginHostServices> m_host;   /* 借用入参 -> 自己 add-ref，析构自动 release */
    PiPluginPtr<IPiPluginHostUI>       m_hostUI; /* headless 宿主上为空句柄 */
    /* weak: owned by the host, see CreateView。原子是因为 W-04 的探针线程会读它，
     * 而 CreateView / Terminate 在宿主 GUI 线程上写它。 */
    std::atomic<IPiPluginView*>        m_view;

    QThread*          m_uiThread; /* create_widget 时所在的线程（= 宿主 GUI 线程） */
    std::thread       m_postWorker;
    std::atomic<bool> m_postWorkerStop;
    std::atomic<bool> m_postProbeDone;
    std::atomic<int>  m_postProbeRuns; /* 回调被执行了几次 */
};

/* IPiPluginViewProvider 的"第二个 vtbl"对象。
 *
 * 一个对象不能有两个 vtbl（接口指针必须指向以 vtbl 为第一个成员的结构），
 * 所以 provider 是一个独立的、被插件持有的薄对象：它各自持有一份插件引用，
 * 释放时把那份引用还回去。宿主 QI 一次拿一个，用完 release 即可。 */
class QtPluginViewProvider
{
public:
    static IPiPluginViewProviderVtbl const s_vtbl;

    explicit QtPluginViewProvider(QtPlugin* plugin);
    static void Destroy(void* self_ptr);

    static uint32_t PI_CALL AddRef(void* self_ptr);
    static uint32_t PI_CALL Release(void* self_ptr);
    static PiResult PI_CALL Qi(void* self_ptr, PiGuid const* iid, void** out);
    static PiResult PI_CALL Views(void* self_ptr, PiPluginViewInfo const** out_views,
                                  uint32_t* out_count);
    static PiResult PI_CALL GetView(void* self_ptr, char const* view_id, IPiPluginView** out);

    PiRefCountedBase m_base;   /* MUST be first data member */
    QtPlugin*        m_plugin; /* add-ref'd */
};

class QtPluginFactory
{
public:
    QtPluginFactory();

    static uint32_t PI_CALL AddRef(void* self_ptr);
    static uint32_t PI_CALL Release(void* self_ptr);
    static PiResult PI_CALL Qi_Factory(void* self_ptr, PiGuid const* iid, void** out);

    static PiPluginDescriptor const* PI_CALL GetDescriptor(void* self_ptr);
    static uint32_t PI_CALL                  GetClassCount(void* self_ptr);
    static PiResult PI_CALL                  GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid);
    static PiResult PI_CALL                  CreateInstance(void*                  self_ptr,
                                                            PiGuid const*          guid,
                                                            IPiPluginHostServices* host,
                                                            IPiPluginBase**        out);

    static IPiPluginFactoryVtbl const s_factory_vtbl;
    PiRefCountedBase                  m_base; /* MUST be first data member */

private:
    PiPluginDescriptor m_descriptor;
    PiPluginCapability m_capabilities[2];
    PiPluginProperty   m_properties[3]; /* APP-04：自由元数据 */
};

#endif /* PI_PLUGIN_QT_TEST_PLUGIN_H */
