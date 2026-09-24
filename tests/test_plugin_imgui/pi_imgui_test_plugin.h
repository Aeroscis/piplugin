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

class ImGuiPlugin
{
public:
    friend class ImGuiPluginFactory;

    ImGuiPlugin();
    ~ImGuiPlugin();

    PiResult Initialize(IPiPluginHostServices* host);
    PiResult Terminate();

    static PiResult PI_CALL Qi_PluginBase(void* self_ptr, PiGuid const* iid, void** out);
    static PiResult PI_CALL Init(void* self_ptr, IPiPluginHostServices* host);
    static PiResult PI_CALL Term(void* self_ptr);
    static PiResult PI_CALL GetView(void* self_ptr, IPiPluginView** out);

    /* ImGui adapter kit callbacks (host GUI thread). */
    static void SetupUi(void* user_data);
    static void DrawUi(void* user_data);
    static void Retain(void* user_data);
    static void Release(void* user_data);

private:
    static void Destroy(void* self_ptr) { delete static_cast<ImGuiPlugin*>(self_ptr); }

    PiRefCountedBase               m_base; /* MUST be first data member */
    static IPiPluginBaseVtbl const s_base_vtbl;

    PiPluginPtr<IPiPluginHostServices> m_host;   /* 借用入参 -> 自己 add-ref，析构自动 release */
    PiPluginPtr<IPiPluginHostUI>       m_hostUI; /* headless 宿主上为空句柄 */
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
