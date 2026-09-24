/*
 * piplugin - Qt UI adapter kit (piplugin_qt)
 *
 * Lets a plugin expose its UI as Qt widgets inside ANY piplugin
 * host (imgui, wxWidgets, raw Win32, ...), without the host knowing Qt.
 *
 * The kit owns the whole "Qt compatibility layer":
 *   - the module's single QApplication, created on the HOST'S GUI THREAD
 *     (Qt requires the GUI thread to be the QApplication thread, and Win32
 *     requires parent and child windows to be serviced by one thread)
 *   - IPiPluginView implementation with foreign-window embedding (Win32: the
 *     host container is announced to Qt before the widget's native window is
 *     created, so Qt itself creates it as a WS_CHILD of the host - no
 *     SetParent() of a finished top-level window, no frame margins, no screen
 *     coordinate arithmetic; see the embedding note in pi_qt_view.cpp),
 *     host-driven event pumping via IPiPluginView::pi_plugin_on_idle() (which calls
 *     processEvents() on a bounded slice), and fully synchronous
 *     attach / detach / resize / visibility
 *
 * Every call runs on the host GUI thread, so there is no marshaling, no
 * second thread and nothing that can outlive the module: when
 * pi_plugin_view_release() returns, the widget AND the QApplication are gone, and
 * the host may FreeLibrary() the plugin.
 *
 * Plugin authors only write a widget factory:
 *
 *   static QWidget* MakeUi(void* user) {
 *       QWidget* w = new QWidget();
 *       ... build your UI ...
 *       return w;
 *   }
 *
 *   // inside IPiPluginBase::pi_plugin_get_view:
 *   PiPluginQtViewDesc desc = {};
 *   desc.create_widget = &MakeUi;
 *   desc.user_data     = this;
 *   desc.retain        = &MyPlugin::AddRefThunk;   // optional
 *   desc.release       = &MyPlugin::ReleaseThunk;  // optional
 *   pi_plugin_qt_view_create(&desc, out);
 *
 *   // inside IPiPluginBase::pi_plugin_terminate (recommended):
 *   pi_plugin_qt_view_shutdown_owner(this);
 *
 * Host requirements: call IPiPluginView::pi_plugin_on_idle() once per frame, and
 * detach + release the view before unloading the plugin module.
 *
 * Limitation: the kit is meant for hosts that do NOT themselves run Qt
 * (they use another toolkit). A Qt-based host should instead put the
 * plugin widgets into its own Qt event loop directly.
 */
#ifndef PI_PLUGIN_QT_VIEW_H
#define PI_PLUGIN_QT_VIEW_H

#include "piplugin/pi_plugin.h"

#include <QWidget>

/* --------------------------------------------------------------------------
 * Symbol visibility
 *
 * The kit is a SHARED library (roadmap APP-08): the process-level state it owns
 * (the single QApplication, the live-view registry) must exist ONCE for the
 * whole process, so every Qt plugin links the same DLL instead of embedding its
 * own copy. On Windows that means the four functions below have to be exported
 * explicitly - this repository deliberately does not use
 * CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS (see interface-freeze-review.md F1, where
 * that switch leaked CRT internals out of the core DLL).
 *
 * PI_PLUGIN_QT_BUILDING is defined for the kit's own translation units only;
 * consumers see dllimport. Taking the address of a dllimport function is why
 * plugin code must not put framework functions straight into a vtable (the
 * C4232 note in docs/design/interfaces.md 5.3) - this header is not a vtable,
 * so the usual call sites are unaffected.
 * -------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
#  ifdef PI_PLUGIN_QT_BUILDING
#    define PI_PLUGIN_QT_API __declspec(dllexport)
#  else
#    define PI_PLUGIN_QT_API __declspec(dllimport)
#  endif
#else
#  define PI_PLUGIN_QT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * View descriptor
 * -------------------------------------------------------------------------- */

/* Create the plugin's root widget. Called on the HOST'S GUI THREAD while
 * QApplication exists and before embedding. Return a parentless QWidget
 * (the kit embeds and sizes it) that has NOT been shown and does not own a
 * native window yet: the kit has to tell Qt which foreign window is the
 * parent *before* the native window is created (that is what makes Qt create
 * it as a real child window instead of a top-level one). Return NULL to fail
 * the attach. */
typedef QWidget* (*PiPluginQtCreateWidgetProc)(void* user_data);

/* Optional: called on the host GUI thread to destroy the widget
 * (e.g. to disconnect signals first). If NULL the kit deletes it. */
typedef void (*PiPluginQtDestroyWidgetProc)(void* user_data, QWidget* widget);

/* Optional refcount hooks: retain is called when the kit starts holding
 * user_data (attach), release after the widget has been destroyed. Use
 * them to keep your plugin object alive while Qt still references it. */
typedef void (*PiPluginQtRetainProc)(void* user_data);
typedef void (*PiPluginQtReleaseProc)(void* user_data);

typedef struct PiPluginQtViewDesc {
    PiPluginQtCreateWidgetProc  create_widget;   /* required */
    PiPluginQtDestroyWidgetProc destroy_widget;  /* optional, NULL = delete */
    PiPluginQtRetainProc        retain;          /* optional, NULL = nothing */
    PiPluginQtReleaseProc       release;         /* optional, NULL = nothing */
    void*                 user_data;       /* passed to all callbacks  */
} PiPluginQtViewDesc;

/* Create a Qt-backed IPiPluginView. The returned view starts with
 * refcount 1; release it with ->pi_release() (after pi_plugin_detach() or let
 * release handle a still-attached view). */
PI_PLUGIN_QT_API PiResult pi_plugin_qt_view_create(const PiPluginQtViewDesc* desc, IPiPluginView** out_view);

/* Tear down the live views of THIS PLUGIN, synchronously, on the calling (host
 * GUI) thread; the QApplication is destroyed when the last view in the process
 * goes away. Idempotent, and safe even with views still attached.
 *
 * `owner` is the value the plugin passed as PiPluginQtViewDesc::user_data when it
 * created those views - almost always the plugin instance (`this`). Scoping the
 * teardown is what makes the SHARED kit safe for several Qt plugins at once:
 * "every view in the process" would reach into the OTHER plugins that are still
 * loaded and delete their widgets.
 *
 * The host must make sure this has happened - by detaching and releasing the
 * views, or by calling this from the plugin's pi_plugin_terminate() - before it
 * unloads the plugin's module: widget destruction runs code compiled into the
 * PLUGIN, which is about to be unmapped. */
PI_PLUGIN_QT_API void pi_plugin_qt_view_shutdown_owner(void* owner);

/* The process-wide hammer: tear down every live view (whichever plugin it
 * belongs to) and destroy the QApplication. Only correct when the caller owns
 * every Qt plugin in the process; it is the diagnostic / last-resort path, and
 * what the per-module kits of 0.2.0 meant by "shutdown". Prefer
 * pi_plugin_qt_view_shutdown_owner(). */
PI_PLUGIN_QT_API void pi_plugin_qt_view_shutdown(void);

/* The plugin's root widget, or NULL if it has not been created yet.
 * Created on the host GUI thread; only touch it from there. */
PI_PLUGIN_QT_API QWidget* pi_plugin_qt_view_widget(IPiPluginView* view);

/* Run fn(user) on the host GUI thread (W-04).
 *
 * Callable from ANY thread: called on the host GUI thread itself the callback
 * runs inline (same order, no latency); called from any other thread the call
 * is queued and run by the host's next pi_plugin_on_idle() -> processEvents() slice,
 * WITHOUT blocking the caller. There is still no second Qt thread anywhere -
 * the callback always runs on the one thread that owns the widgets, which is
 * what makes it safe to touch Qt inside it.
 *
 * The queue is best effort by design: a call that has not run yet is dropped
 * when the view is detached or destroyed (the plugin must not be called back
 * after its widget is gone), and before the first attach there is no host GUI
 * thread to marshal to, so it is dropped too. Both cases leave a line in the
 * PI_PLUGIN_QT_VIEW_TRACE=1 log. */
PI_PLUGIN_QT_API void pi_plugin_qt_view_post(IPiPluginView* view, void (*fn)(void* user), void* user);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_QT_VIEW_H */
