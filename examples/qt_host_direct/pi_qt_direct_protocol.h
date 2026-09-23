/*
 * piplugin example - the app-defined protocol that carries a QWidget*
 * (channel A; see docs/design/interfaces.md §5 and
 * docs/tutorial/qt-host-direct.md)
 *
 * WHY THIS EXISTS
 *   The Qt adapter kit (piplugin_qt) is built for hosts that do NOT run Qt: it
 *   creates the process's one QApplication and pumps it from the host's loop.
 *   A host that already owns a QApplication cannot use it - the kit's attach
 *   fails (and a debug Qt build asserts "there should be only one application
 *   object"). The integration a Qt host wants is the other way round: the
 *   plugin hands it a QWidget* and the host adds it to its own layout, so the
 *   host's event loop drives it. There is no framework interface for "give me a
 *   QWidget*" - the framework's vocabulary is toolkit-neutral - so the app
 *   defines one. That is exactly what channel A is for.
 *
 * WHY IT IS C++-ONLY
 *   The vtable carries QWidget*, which is a C++ type. That does not bend any
 *   framework rule: this protocol is the app's, not the framework's, and both
 *   of its participants are Qt C++ by definition. The plugin's IPiPluginBase /
 *   IPiPluginFactory halves stay plain C ABI.
 *
 * GUID rule (interfaces.md 5.1): below 0x80000000 is the framework's reserved
 * range - never allocate there yourself. Apps use a random 128-bit UUID:
 *   python -c "import uuid; print(uuid.uuid4())"
 */
#ifndef PI_QT_DIRECT_PROTOCOL_H
#define PI_QT_DIRECT_PROTOCOL_H

#include "piplugin/pi_plugin.h"

#ifndef __cplusplus
#  error "pi_qt_direct_protocol.h is C++ only: its vtable carries QWidget*."
#endif

class QWidget;

#define PI_QT_DIRECT_WIDGET_IID_INIT \
    PI_GUID(0x9C3E71B5, 0x6D48, 0x4A21, 0xA5, 0x0E, 0x37, 0xD9, 0x82, 0x54, 0x1F, 0x6C)

static const PiGuid PI_QT_DIRECT_WIDGET_IID = PI_QT_DIRECT_WIDGET_IID_INIT;

/* The contract, and all of it matters (the host cannot check it for you):
 *
 *   create_widget()
 *     - called on the host's GUI thread, which IS the QApplication thread;
 *     - returns a fresh, PARENTLESS widget that has never been shown: the host
 *       owns where it goes and shows it by putting it in a layout;
 *     - does NOT transfer the plugin's lifetime: the plugin object stays owned
 *       by the session (the widget's signal/slot bodies, however, live in the
 *       plugin's module - see destroy_widget()).
 *
 *   destroy_widget()
 *     - destroys a widget this plugin created, on the host's GUI thread;
 *     - MUST be called before pi_host_session_unload() / FreeLibrary: after the
 *       module is gone, a widget that is still alive holds function pointers
 *       into unmapped memory, and the next click or repaint calls them.
 *     - a plugin that never links the Qt kit has no other teardown hook, which
 *       is why this slot exists instead of "the host just deletes it".
 */
typedef struct IQtDirectWidgetVtbl {
    IPiUnknownVtbl base;

    QWidget* (PI_CALL *create_widget)(void* this_ptr);
    void     (PI_CALL *destroy_widget)(void* this_ptr, QWidget* widget);
} IQtDirectWidgetVtbl;

typedef struct IQtDirectWidget {
    const IQtDirectWidgetVtbl* lpVtbl;
} IQtDirectWidget;

static inline QWidget* pi_qt_direct_create_widget(IQtDirectWidget* self)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->create_widget) return 0;
    return self->lpVtbl->create_widget((void*)self);
}

static inline void pi_qt_direct_destroy_widget(IQtDirectWidget* self, QWidget* widget)
{
    if (!self || !self->lpVtbl || !self->lpVtbl->destroy_widget) return;
    self->lpVtbl->destroy_widget((void*)self, widget);
}

#endif /* PI_QT_DIRECT_PROTOCOL_H */
