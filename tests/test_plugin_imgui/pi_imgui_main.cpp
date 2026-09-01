/*
 * pipluginframework - ImGui Test Plugin DLL Entry Point
 */
#include "pi_imgui_test_plugin.h"

extern "C" __declspec(dllexport) PiResult pi_plugin_entry(IPiPluginFactory** out_factory)
{
    if (!out_factory) return PI_E_INVALIDARG;
    ImGuiPluginFactory* factory = new ImGuiPluginFactory();
    if (!factory) return PI_E_OUTOFMEMORY;
    *out_factory = (IPiPluginFactory*)&factory->m_base;
    return PI_OK;
}
