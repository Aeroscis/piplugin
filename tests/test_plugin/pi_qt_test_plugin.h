/*
 * piplugin - Test Plugin (Qt-based)
 *
 * With the Qt adapter kit (piplugin_qt) the plugin contains NO
 * Qt integration code at all: no QApplication management, no embedding,
 * no event-loop plumbing. It only provides a widget factory and the
 * plugin lifecycle.
 */
#ifndef PI_QT_TEST_PLUGIN_H
#define PI_QT_TEST_PLUGIN_H

#include "piplugin/pi_plugin.h"

class QWidget;

/* Generic destroy thunk: lets PiRefCountedBase::release() run the C++
 * destructor instead of free()ing the object. */
template <typename T>
void pi_cpp_destroy(void* self_ptr) { delete static_cast<T*>(self_ptr); }

class QtPlugin {
public:
    friend class QtPluginFactory;

    QtPlugin();
    ~QtPlugin();

    PiResult Initialize(IPiHostServices* host);
    PiResult Terminate();

    static PiResult PI_CALL Qi_PluginBase(void* self_ptr, const PiGuid* iid, void** out);
    static PiResult PI_CALL Init(void* self_ptr, IPiHostServices* host);
    static PiResult PI_CALL Term(void* self_ptr);
    static PiResult PI_CALL GetView(void* self_ptr, IPiPluginView** out);

    /* Widget factory handed to the Qt adapter kit. Called on the Qt
     * runtime thread; builds the whole demo UI. */
    static QWidget* CreateUi(void* user_data);

    /* Refcount thunks so the adapter keeps this object alive while the
     * widget (whose signal lambdas capture `this`) still exists. */
    static void Retain(void* user_data);
    static void Release(void* user_data);

private:
    static void Destroy(void* self_ptr) { delete static_cast<QtPlugin*>(self_ptr); }

    PiRefCountedBase m_base;          /* MUST be first data member */
    static const IPiPluginBaseVtbl s_base_vtbl;

    IPiHostServices*  m_host;         /* add-ref'd */
    IPiHostUI*        m_hostUI;       /* add-ref'd, NULL on headless host */
    IPiPluginView*    m_view;         /* weak: owned by the host, see GetView */
};

class QtPluginFactory {
public:
    QtPluginFactory();

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

#endif /* PI_QT_TEST_PLUGIN_H */
