/*
 * piplugin - Test Plugin (Dear ImGui-based)
 *
 * The mirror twin of the Qt test plugin: same COM structure, same
 * capability declaration, but the UI is drawn with Dear ImGui through
 * the piplugin_imgui adapter kit - designed to run inside a
 * Qt host.
 */
#ifndef PI_PLUGIN_IMGUI_TEST_PLUGIN_H
#define PI_PLUGIN_IMGUI_TEST_PLUGIN_H

#include "piplugin/pi_plugin.h"
/* C++ RAII 层（可选头）：PiPluginPtr 管住宿主与服务接口，pi_plugin_cpp_destroy 提供
 * PiRefCountedBase 需要的析构 thunk。本文件此前自带一份 pi_plugin_cpp_destroy，
 * 现在框架头里有了，就不再各写一份。 */
#include "piplugin/pi_cpp.h"

class ImGuiPluginViewProvider;

class ImGuiPlugin
{
public:
    friend class ImGuiPluginFactory;
    friend class ImGuiPluginViewProvider;

    ImGuiPlugin();
    ~ImGuiPlugin();

    PiResult Initialize(IPiPluginHostServices* host);
    PiResult Terminate();

    static PiResult PI_CALL Qi_PluginBase(void* self_ptr, PiGuid const* iid, void** out);
    static PiResult PI_CALL Init(void* self_ptr, IPiPluginHostServices* host);
    static PiResult PI_CALL Term(void* self_ptr);

    /* ImGui adapter kit callbacks (host GUI thread). */
    static void SetupUi(void* user_data);
    static void DrawUi(void* user_data);
    static void Retain(void* user_data);
    static void Release(void* user_data);

private:
    static void Destroy(void* self_ptr) { delete static_cast<ImGuiPlugin*>(self_ptr); }

    /* 造一个 ImGui 套件的视图（provider 的 pi_plugin_get_view 落到这里）。 */
    PiResult CreateView(IPiPluginView** out);

    PiRefCountedBase               m_base; /* MUST be first data member */
    static IPiPluginBaseVtbl const s_base_vtbl;

    PiPluginPtr<IPiPluginHostServices> m_host;   /* 借用入参 -> 自己 add-ref，析构自动 release */
    PiPluginPtr<IPiPluginHostUI>       m_hostUI; /* headless 宿主上为空句柄 */
};

/* IPiPluginViewProvider 的"第二个 vtbl"对象：与 Qt 测试插件里那个逐个字段同构
 * （一个对象不能有两个 vtbl，所以 provider 是独立薄对象，各持一份插件引用）。 */
class ImGuiPluginViewProvider
{
public:
    static IPiPluginViewProviderVtbl const s_vtbl;

    explicit ImGuiPluginViewProvider(ImGuiPlugin* plugin);
    static void Destroy(void* self_ptr);

    static uint32_t PI_CALL AddRef(void* self_ptr);
    static uint32_t PI_CALL Release(void* self_ptr);
    static PiResult PI_CALL Qi(void* self_ptr, PiGuid const* iid, void** out);
    static PiResult PI_CALL Views(void* self_ptr, PiPluginViewInfo const** out_views,
                                  uint32_t* out_count);
    static PiResult PI_CALL GetView(void* self_ptr, char const* view_id, IPiPluginView** out);

    PiRefCountedBase m_base;   /* MUST be first data member */
    ImGuiPlugin*     m_plugin; /* add-ref'd */
};

class ImGuiPluginFactory
{
public:
    ImGuiPluginFactory();

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
    PiPluginProperty   m_properties[3]; /* APP-04：自由元数据（含变体标记） */
};

#endif /* PI_PLUGIN_IMGUI_TEST_PLUGIN_H */
