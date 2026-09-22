/*
 * piplugin - Test Plugin DLL Entry Point
 *
 * The factory is a refcounted object whose release destroys it, so no
 * manual cleanup is done here (DllMain cleanup would be a double free).
 */
#include "pi_qt_test_plugin.h"

extern "C" __declspec(dllexport) PiResult pi_plugin_entry(IPiPluginFactory** out_factory)
{
    if (!out_factory) return PI_E_INVALIDARG;
    QtPluginFactory* factory = new QtPluginFactory();
    if (!factory) return PI_E_OUTOFMEMORY;
    *out_factory = (IPiPluginFactory*)&factory->m_base;
    return PI_OK;
}
