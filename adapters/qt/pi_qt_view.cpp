#include "pi_qt_view.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QVariant>
#include <QWidget>
#include <QWindow>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if PI_PLATFORM_WINDOWS
#  include <windows.h>
#endif

/* ==========================================================================
 * Diagnostics
 *
 * PI_QT_VIEW_TRACE=1 in the environment turns on a line per lifecycle step in
 * <exe dir>/pi_qt_view.log, with the thread id of the thread running it. Every
 * bug this file has ever had was "plugin code ran on the wrong thread", so the
 * trace is the first thing to check.
 * ======================================================================== */
namespace {

bool piqt_trace_enabled()
{
    static const bool enabled = []() {
        char buf[8] = { 0 };
        size_t n = 0;
#if PI_PLATFORM_WINDOWS
        n = GetEnvironmentVariableA("PI_QT_VIEW_TRACE", buf, sizeof(buf));
#else
        const char* env = getenv("PI_QT_VIEW_TRACE");
        if (env) {
            n = strlen(env);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, env, n);
        }
#endif
        return n > 0 && buf[0] != '0';
    }();
    return enabled;
}

void piqt_trace_impl(const char* fmt, va_list ap)
{
#if PI_PLATFORM_WINDOWS
    static FILE* f = nullptr;
    if (!f) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;
        char* slash = strrchr(path, '\\');
        if (!slash) return;
        *(slash + 1) = 0;
        strncat_s(path, sizeof(path), "pi_qt_view.log", _TRUNCATE);
        if (fopen_s(&f, path, "a") != 0) { f = nullptr; return; }
    }
    char line[256];
    vsnprintf(line, sizeof(line), fmt, ap);
    fprintf(f, "[qtview %6lu %8lu] %s\n", (unsigned long)GetCurrentThreadId(),
            (unsigned long)GetTickCount64(), line);
    fflush(f);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
}

void piqt_trace(const char* fmt, ...)
{
    if (!piqt_trace_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    piqt_trace_impl(fmt, ap);
    va_end(ap);
}

} // namespace

/* ==========================================================================
 * Embedding a plugin widget into a foreign (non-Qt) host window
 *
 * There is no QWidget behind the host's container HWND, so the widget cannot
 * simply be QWidget::setParent()ed into it. Handing a finished top-level Qt
 * window to Win32 with a raw SetParent() does not work either: Qt keeps
 * treating the widget as a top-level window, so every geometry request gets the
 * top-level frame margins added and is computed in SCREEN coordinates - which
 * a child window reads as parent-relative ones. The embedded UI ends up
 * offset and clipped inside the host, and only happens to look right when the
 * geometry is re-read from the window manager instead of computed by Qt
 * (which is exactly the "correct after minimize/restore, wrong again after a
 * resize" symptom).
 *
 * Qt's Windows platform plugin has a supported way to be told about a foreign
 * native parent: the dynamic property below. QWidgetPrivate::createTLSysExtra()
 * copies it from the widget onto the widget's QWindow, and
 * WindowCreationData::fromWindow() reads it to decide that the window is NOT
 * top level and must be created as a WS_CHILD of that handle - no frame
 * margins, parent-relative geometry, plain MoveWindow() on resize. This is the
 * same mechanism ActiveQt servers and Qt's own QtWinMigrate use to embed Qt
 * into an MFC/Win32 application.
 *
 * It is only consulted while the native window is created, so it must be set
 * before anything (winId(), show(), ...) forces one - see PiQtView::attach().
 * ======================================================================== */
#if PI_PLATFORM_WINDOWS
static const char kEmbeddedNativeParentHandle[] = "_q_embedded_native_parent_handle";
#endif

/* ==========================================================================
 * PiQtView - the process module's single QApplication
 *
 * THREADING MODEL (this is the part that must not regress):
 *
 *   Everything Qt happens on the HOST'S GUI THREAD. The host creates its
 *   window, its message loop and therefore all of its child HWNDs there, and
 *   the adapter embeds the plugin widget into that hierarchy. Qt requires the
 *   GUI thread to be the thread QApplication was created on, and Windows
 *   requires a window's parent/child relationship to be serviced by one
 *   thread: destroying or reparenting a child sends synchronous
 *   WM_PARENTNOTIFY/WM_DESTROY messages to its parent window. If the widget
 *   lived on a second thread, Qt's SetParent/DestroyWindow on that thread
 *   would block on a message that only the host thread could dispatch - while
 *   the host thread was blocked waiting for Qt to finish. That is a deadlock
 *   (and, when the blocking call is not a wait but a raw cross-thread call
 *   into a module that is being unloaded, a crash).
 *
 *   Earlier revisions ran QApplication::exec() on a private QThread and
 *   marshaled every call with queued invocations plus a "wait for the widget
 *   to die" semaphore. Do not go back to that: it produced exactly those
 *   crashes on plugin unload and hangs on detach.
 *
 *   Instead, the host drives Qt from its own loop via pi_on_idle(), which
 *   calls processEvents() with a bounded slice. pi_attach/pi_detach/set_visible
 *   are all synchronous on the host thread.
 * ======================================================================== */
namespace {

class PiQtView;

/* Live views of this module: needed so a plugin can force a complete teardown
 * from pi_terminate() even if the host forgets to detach. */
QMutex            g_views_mutex;
QList<PiQtView*>* g_live_views = nullptr;

/* The module's QApplication. Non-null only while at least one view is
 * attached, and only ever touched on the host GUI thread. */
QApplication* g_app = nullptr;
QThread*      g_app_thread = nullptr;   /* thread that created it */
bool          g_app_shutdown = false;   /* teardown in progress */

void piqt_views_register(PiQtView* v);
void piqt_views_unregister(PiQtView* v);

bool piqt_on_gui_thread()
{
    return g_app_thread == nullptr || g_app_thread == QThread::currentThread();
}

/* Drain everything Qt has queued while all the participating modules are
 * still mapped. */
void piqt_drain_events()
{
    QCoreApplication::sendPostedEvents(NULL, QEvent::DeferredDelete);
    QCoreApplication::sendPostedEvents(NULL, 0);
    if (QCoreApplication* app = QCoreApplication::instance())
        app->processEvents(QEventLoop::AllEvents, 50);
}

class PiQtView {
public:
    PiRefCountedBase base;              /* MUST be first data member */

    PiQtView(const PiQtViewDesc& desc)
        : m_desc(desc), m_attached(false), m_parent(PI_INVALID_WINDOW),
          m_widget(nullptr), m_hwnd(PI_INVALID_WINDOW) {}

    QMutex                m_mutex;      /* only for get_native_window queries */
    PiQtViewDesc          m_desc;
    bool                  m_attached;
    PiNativeWindow        m_parent;
    QWidget*              m_widget;
    PiNativeWindow        m_hwnd;

    static const IPiPluginViewVtbl s_vtbl;
    static PiQtView* from_iface(void* self_ptr) { return (PiQtView*)self_ptr; }

    /* All of these run on the host GUI thread, synchronously. */
    bool attach(PiNativeWindow parent);
    void detach();
    void embed_into_host();
    void apply_geometry();
    void repaint_embedded();
    void destroy_widget();

#if PI_PLATFORM_WINDOWS
    /* Last-resort embedding: adopt an already finished top-level window with
     * SetParent(). Only used when Qt did not create the window as a child of
     * the host container itself (see embed_into_host()). */
    void adopt_host_window_raw(HWND parent_hwnd);
#endif
};

/* ---------- live-view registry ---------- */

void piqt_views_register(PiQtView* v)
{
    QMutexLocker lock(&g_views_mutex);
    if (!g_live_views) g_live_views = new QList<PiQtView*>();
    g_live_views->append(v);
}

void piqt_views_unregister(PiQtView* v)
{
    QMutexLocker lock(&g_views_mutex);
    if (g_live_views) g_live_views->removeAll(v);
}

/* ---------- QApplication lifetime (host GUI thread only) ---------- */

bool piqt_app_create()
{
    if (g_app) return true;
    if (g_app_shutdown) return false;
    if (!piqt_on_gui_thread()) {
        piqt_trace("app_create: refused - not on the creating thread");
        return false;
    }

    static int argc = 0;
    static char* argv[] = { nullptr };
    g_app = new QApplication(argc, argv);
    if (!g_app) return false;

    g_app_thread = QThread::currentThread();
    piqt_trace("app: QApplication created on host GUI thread");
    return true;
}

void piqt_app_destroy()
{
    if (!g_app) return;
    if (!piqt_on_gui_thread()) {
        /* Never destroy Qt objects from the wrong thread. */
        piqt_trace("app_destroy: SKIPPED - wrong thread");
        return;
    }

    piqt_trace("app_destroy: begin");
    piqt_drain_events();
    delete g_app;
    g_app = nullptr;
    g_app_thread = nullptr;
    piqt_trace("app_destroy: done");
}

} // namespace

/* ==========================================================================
 * PiQtView bodies - host GUI thread
 * ======================================================================== */

namespace {

/* --------------------------------------------------------------------------
 * Embedding: tell Qt about the host's container window BEFORE it creates the
 * widget's native window, so Qt creates it as a WS_CHILD of that container
 * (see the comment block near kEmbeddedNativeParentHandle).
 * ------------------------------------------------------------------------ */
void PiQtView::embed_into_host()
{
#if PI_PLATFORM_WINDOWS
    if (!m_widget || !PI_IS_VALID_WINDOW(m_parent)) return;

    /* The property is read exactly once, while the native window is created.
     * A widget that already owns one was created before we could tell Qt about
     * the host; QWidget::destroy() (the only public-ish way to force a
     * re-creation) is protected, so such a widget takes the raw path instead. */
    if (m_widget->internalWinId()) {
        piqt_trace("embed: widget %p already owns native window %p - "
                   "cannot pre-announce the host, will use SetParent",
                   (void*)m_widget, (void*)(uintptr_t)m_widget->internalWinId());
        return;
    }

    m_widget->setProperty(kEmbeddedNativeParentHandle,
                          QVariant::fromValue((WId)(quintptr)m_parent));
    piqt_trace("embed: widget %p will be created as a child of container %p",
               (void*)m_widget, (void*)(uintptr_t)m_parent);
#endif
}

#if PI_PLATFORM_WINDOWS
void PiQtView::adopt_host_window_raw(HWND parent_hwnd)
{
    HWND hwnd = (HWND)m_widget->winId();
    if (!hwnd) return;

    piqt_trace("embed: Qt did not parent %p to %p - falling back to SetParent",
               (void*)hwnd, (void*)parent_hwnd);

    /* Order matters: make it a child *before* reparenting, so the window
     * manager never briefly treats it as a top-level window inside the host.
     * The top-level frame is stripped too - a child window has none, and Qt
     * would otherwise keep adding it to the sizes we ask for. */
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    SetWindowLongPtr(hwnd, GWL_STYLE,
                     (style | WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN)
                     & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME
                         | WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
    if (GetParent(hwnd) != parent_hwnd)
        SetParent(hwnd, parent_hwnd);

    RECT r; GetClientRect(parent_hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return;
    SetWindowPos(hwnd, NULL, 0, 0, r.right, r.bottom,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    /* Deliberately no QWidget::resize() here: Qt still believes this is a
     * top-level window, and letting it re-apply its own (framed, screen
     * coordinate) geometry would undo exactly what we just set up. The window
     * manager's WM_WINDOWPOSCHANGED already told Qt the real client-relative
     * rectangle, which is what the widget's layout uses. */
}
#endif

void PiQtView::apply_geometry()
{
#if PI_PLATFORM_WINDOWS
    if (!m_widget || !PI_IS_VALID_WINDOW(m_parent)) return;
    HWND parent_hwnd = (HWND)m_parent;

    RECT r; GetClientRect(parent_hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return;

    /* QWidget geometry is in device-independent pixels, the host hands us
     * native ones: convert, so the embedded window matches the container at
     * any screen scaling factor. */
    qreal dpr = 1.0;
    if (QWindow* window = m_widget->windowHandle())
        dpr = window->devicePixelRatio();
    if (dpr <= 0.0) dpr = 1.0;
    const int w = qRound(r.right / dpr);
    const int h = qRound(r.bottom / dpr);

    /* (0,0) is the container's client origin - which is exactly where a window
     * Qt created as a child of it belongs. Instantiates the native window on
     * the first call, as a child of the host thanks to embed_into_host(). */
    m_widget->setGeometry(0, 0, w, h);

    HWND hwnd = (HWND)m_widget->winId();
    if (!hwnd) return;
    m_hwnd = (PiNativeWindow)hwnd;

    if (GetParent(hwnd) != parent_hwnd)
        adopt_host_window_raw(parent_hwnd);

    piqt_trace("geometry: container=%ldx%ld widget=%dx%d hwnd=%p parent=%p",
               r.right, r.bottom, w, h, (void*)hwnd,
               (void*)GetParent(hwnd));
#endif
}

void PiQtView::repaint_embedded()
{
    if (!m_widget) return;
    m_widget->update();
#if PI_PLATFORM_WINDOWS
    if (HWND hwnd = (HWND)m_widget->winId())
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                     RDW_UPDATENOW | RDW_FRAME);
#endif
}

bool PiQtView::attach(PiNativeWindow parent)
{
    piqt_trace("attach: enter parent=%p", (void*)(uintptr_t)parent);
    if (m_attached) return true;
    if (!piqt_app_create()) return false;

    m_parent = parent;
    m_attached = true;

    if (m_desc.retain)
        m_desc.retain(m_desc.user_data);

    QWidget* w = m_desc.create_widget(m_desc.user_data);
    if (!w) {
        piqt_trace("attach: widget factory returned NULL");
        if (m_desc.release) m_desc.release(m_desc.user_data);
        m_attached = false;
        return false;
    }

    m_widget = w;
    /* Tell Qt about the host container before anything creates the widget's
     * native window - embed_into_host() must run first. */
    embed_into_host();
    apply_geometry();
    w->show();
    repaint_embedded();
    piqt_trace("attach: widget=%p hwnd=%p embedded", (void*)w,
               (void*)(uintptr_t)m_hwnd);
    return true;
}

void PiQtView::destroy_widget()
{
    QWidget* w = m_widget;
    m_widget = nullptr;
    m_hwnd = PI_INVALID_WINDOW;
    if (!w) return;

    piqt_trace("destroy: deleting widget %p", (void*)w);
    /* Runs on the host GUI thread - the thread that owns the parent window -
     * so Qt's window destruction cannot deadlock against the host's loop. */
    if (m_desc.destroy_widget)
        m_desc.destroy_widget(m_desc.user_data, w);
    else
        delete w;
    piqt_drain_events();
    piqt_trace("destroy: widget gone");
}

void PiQtView::detach()
{
    if (!m_attached && !m_widget) return;
    m_attached = false;
    destroy_widget();
    if (m_desc.release) {
        m_desc.release(m_desc.user_data);
        piqt_trace("detach: user_data released");
    }
    m_parent = PI_INVALID_WINDOW;
}

/* ---------- vtable slots ---------- */

PiResult PI_CALL piqt_qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    PiQtView* me = PiQtView::from_iface(self_ptr);
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_VIEW)) {
        *out = me;
        me->base.unk.lpVtbl->pi_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

PiResult PI_CALL piqt_attach(void* self_ptr, PiNativeWindow parent)
{
    if (!PI_IS_VALID_WINDOW(parent)) return PI_E_INVALIDARG;
    PiQtView* me = PiQtView::from_iface(self_ptr);
    return me->attach(parent) ? PI_OK : PI_FAIL;
}

PiResult PI_CALL piqt_detach(void* self_ptr)
{
    PiQtView* me = PiQtView::from_iface(self_ptr);
    me->detach();
    piqt_trace("detach: done");
    return PI_OK;
}

PiNativeWindow PI_CALL piqt_get_native_window(void* self_ptr)
{
    PiQtView* me = PiQtView::from_iface(self_ptr);
    QMutexLocker lock(&me->m_mutex);
    return me->m_hwnd;
}

PiResult PI_CALL piqt_on_resize(void* self_ptr, int32_t w, int32_t h)
{
    PiQtView* me = PiQtView::from_iface(self_ptr);
    if (!me->m_attached || !me->m_widget) return PI_OK;
#if PI_PLATFORM_WINDOWS
    /* The container's real client rectangle is authoritative - sizing the
     * widget (instead of SetWindowPos()-ing the HWND behind Qt's back) is what
     * keeps Qt's idea of the window in sync with reality, which is the fix for
     * the "layout is offset / clipped until the geometry happens to be re-read"
     * behaviour. */
    (void)w; (void)h;
    me->apply_geometry();
#else
    if (w > 0 && h > 0) me->m_widget->resize(w, h);
#endif
    return PI_OK;
}

PiResult PI_CALL piqt_on_idle(void* self_ptr)
{
    (void)self_ptr;
    /* The host owns the message loop; we only hand Qt its slice of it.
     * Without this call nothing in the plugin that depends on the event loop
     * (timers, painting, input) ever runs - the widget would be drawn once
     * and then stay frozen. */
    if (QCoreApplication* app = QCoreApplication::instance()) {
        app->sendPostedEvents();
        app->processEvents(QEventLoop::AllEvents, 4);
    }
    return PI_OK;
}

PiResult PI_CALL piqt_get_preferred_size(void* self_ptr, int32_t* w, int32_t* h)
{
    (void)self_ptr;
    if (w) *w = 400;
    if (h) *h = 300;
    return PI_OK;
}

PiResult PI_CALL piqt_set_visible(void* self_ptr, int32_t visible)
{
    PiQtView* me = PiQtView::from_iface(self_ptr);
    if (!me->m_widget) return PI_OK;
    me->m_widget->setVisible(visible != 0);
    if (visible) me->repaint_embedded();
    return PI_OK;
}

const IPiPluginViewVtbl PiQtView::s_vtbl = {
    { &piqt_qi, &pi_refcounted_add_ref, &pi_refcounted_release },
    &piqt_attach,
    &piqt_detach,
    &piqt_get_native_window,
    &piqt_on_resize,
    &piqt_on_idle,
    &piqt_get_preferred_size,
    &piqt_set_visible
};

static void piqt_view_destroy(void* self_ptr)
{
    PiQtView* me = PiQtView::from_iface(self_ptr);
    piqt_trace("view_destroy: enter");
    me->detach();
    piqt_views_unregister(me);
    delete me;

    /* Last view gone: drop the QApplication now, on this thread, while the
     * module that owns its code is still mapped. The host is free to
     * FreeLibrary() as soon as this returns. */
    {
        QMutexLocker lock(&g_views_mutex);
        if (g_live_views && !g_live_views->isEmpty()) return;
    }
    if (g_app) {
        g_app_shutdown = true;
        piqt_drain_events();
        piqt_app_destroy();
    }
    piqt_trace("view_destroy: done");
}
} // namespace

/* ==========================================================================
 * Public API
 * ======================================================================== */

extern "C" PiResult pi_qt_view_create(const PiQtViewDesc* desc, IPiPluginView** out_view)
{
    if (!desc || !desc->create_widget || !out_view)
        return PI_E_INVALIDARG;
    *out_view = NULL;

    PiQtView* view = new PiQtView(*desc);
    if (!view) return PI_E_OUTOFMEMORY;
    pi_refcounted_init_with_destroy(&view->base,
                                    (const IPiUnknownVtbl*)&PiQtView::s_vtbl,
                                    &piqt_view_destroy);
    piqt_views_register(view);
    *out_view = (IPiPluginView*)&view->base;
    return PI_OK;
}

extern "C" void pi_qt_view_shutdown(void)
{
    piqt_trace("shutdown: begin");
    /* Force every view of this module through a full detach while the plugin
     * is still mapped. The host normally detaches first; this covers hosts
     * that simply drop the module. View ownership stays with the host, so the
     * view objects themselves are released through their normal refcount. */
    for (int round = 0; round < 1000; ++round) {
        bool any = false;
        {
            QMutexLocker lock(&g_views_mutex);
            if (g_live_views) {
                for (int i = 0; i < g_live_views->size(); ++i) {
                    PiQtView* v = g_live_views->at(i);
                    if (v && (v->m_attached || v->m_widget)) { v->detach(); any = true; }
                }
            }
        }
        if (!any) break;
    }

    g_app_shutdown = true;
    piqt_drain_events();
    piqt_app_destroy();
    piqt_trace("shutdown: done");
}

extern "C" QWidget* pi_qt_view_widget(IPiPluginView* view)
{
    if (!view) return NULL;
    /* The vtbl pointer sits at offset 0 of PiQtView; the interface pointer we
     * were given is exactly that address. */
    PiQtView* v = (PiQtView*)(void*)view;
    QMutexLocker lock(&v->m_mutex);
    return v->m_widget;
}

extern "C" void pi_qt_view_post(IPiPluginView* view, void (*fn)(void* user), void* user)
{
    if (!view || !fn) return;
    /* Single-threaded model: run it inline. */
    fn(user);
}
