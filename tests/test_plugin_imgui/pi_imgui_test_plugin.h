/*
 * piplugin - Test Plugin (Dear ImGui-based)
 *
 * The mirror twin of the Qt test plugin: same COM structure, same
 * capability declaration, but the UI is drawn with Dear ImGui through
 * the piplugin_imgui adapter kit - designed to run inside a
 * Qt host.
 */
#ifndef PI_IMGUI_TEST_PLUGIN_H
#define PI_IMGUI_TEST_PLUGIN_H

#include "piplugin/pi_plugin.h"

template <typename T>
void pi_cpp_destroy(void* self_ptr) { delete static_cast<T*>(self_ptr); }

class ImGuiPlugin {
public:
    friend class ImGuiPluginFactory;

    ImGuiPlugin();
    ~ImGuiPlugin();

    PiResult Initialize(IPiHostServices* host);
    PiResult Terminate();

    static PiResult PI_CALL Qi_PluginBase(void* self_ptr, const PiGuid* iid, void** out);
    static PiResult PI_CALL Init(void* self_ptr, IPiHostServices* host);
    static PiResult PI_CALL Term(void* self_ptr);
    static PiResult PI_CALL GetView(void* self_ptr, IPiPluginView** out);

    /* ImGui adapter kit callbacks (host GUI thread). */
    static void SetupUi(void* user_data);
    static void DrawUi(void* user_data);
    static void Retain(void* user_data);
    static void Release(void* user_data);

private:
    static void Destroy(void* self_ptr) { delete static_cast<ImGuiPlugin*>(self_ptr); }

    PiRefCountedBase m_base;          /* MUST be first data member */
    static const IPiPluginBaseVtbl s_base_vtbl;

    IPiHostServices*  m_host;         /* add-ref'd */
    IPiHostUI*        m_hostUI;       /* add-ref'd, NULL on headless host */
};

class ImGuiPluginFactory {
public:
    ImGuiPluginFactory();

    static uint32_t PI_CALL AddRef(void* self_ptr);
    static uint32_t PI_CALL Release(void* self_ptr);
    static PiResult PI_CALL Qi_Factory(void* self_ptr, const PiGuid* iid, void** out);

    static const PiPluginDescriptor* PI_CALL GetDescriptor(void* self_ptr);
    static uint32_t PI_CALL GetClassCount(void* self_ptr);
    static PiResult PI_CALL GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid);
    static PiResult PI_CALL CreateInstance(void* self_ptr,
                                            const PiGuid* guid,
                                            IPiHostServices* host,
                                            IPiPluginBase** out);

    static const IPiPluginFactoryVtbl s_factory_vtbl;
    PiRefCountedBase m_base;          /* MUST be first data member */

private:
    PiPluginDescriptor m_descriptor;
    PiPluginCapability m_capabilities[2];
};

#endif /* PI_IMGUI_TEST_PLUGIN_H */
