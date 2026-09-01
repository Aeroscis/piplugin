#include "pi_qt_view.h"

#include <QApplication>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QMetaObject>
#include <QAbstractEventDispatcher>
#include <QSemaphore>

#include <atomic>

#if PI_PLATFORM_WINDOWS
#  include <windows.h>
#endif

/* ==========================================================================
 * PiQtRuntime - process-wide Qt event loop
 *
 * One background thread owns THE QApplication of this process module and
 * runs exec(). Views acquire/release the runtime; the loop quits when the
 * last user goes away. All widget work is marshaled onto this thread via
 * a context QObject living in it.
 * ======================================================================== */
namespace {

QMutex                 g_rt_mutex;
int                    g_rt_users = 0;
QThread*               g_rt_thread = nullptr;
std::atomic<QObject*>  g_rt_context{ nullptr };   /* lives in rt thread  */

void piqt_runtime_start_locked()
{
    g_rt_thread = QThread::create([]() {
        int argc = 0;
        char* argv[] = { nullptr };
        QApplication* app = new QApplication(argc, argv);
        QObject* context = new QObject();       /* affinity: this thread */
        g_rt_context.store(context, std::memory_order_release);

        app->exec();

        delete context;
        g_rt_context.store(nullptr, std::memory_order_release);
        delete app;
    });
    g_rt_thread->start();

    /* Wait until QApplication + context exist. */
    while (!g_rt_context.load(std::memory_order_acquire))
        QThread::msleep(1);
}

/* Wake the shared runtime loop so queued work runs promptly. */
void piqt_runtime_wake()
{
    QObject* context = g_rt_context.load(std::memory_order_acquire);
    if (!context) return;
    QAbstractEventDispatcher* dispatcher =
        QAbstractEventDispatcher::instance(context->thread());
    if (dispatcher) dispatcher->wakeUp();
}

} // namespace

static void piqt_runtime_acquire()
{
    QMutexLocker lock(&g_rt_mutex);
    if (g_rt_users++ == 0) {
        /* A previous shutdown may have left a quitting (or finished)
         * thread behind; make sure it is fully gone before creating a
         * new QApplication. */
        if (g_rt_thread) {
            g_rt_thread->wait();
            delete g_rt_thread;
            g_rt_thread = nullptr;
        }
        piqt_runtime_start_locked();
    }
}

/* May be called from ANY thread, including the runtime thread itself.
 * When the last user leaves, the loop is asked to quit (asynchronously,
 * so it is safe to call from inside a slot running on the rt thread). */
static void piqt_runtime_release()
{
    QMutexLocker lock(&g_rt_mutex);
    if (--g_rt_users == 0) {
        QObject* context = g_rt_context.load(std::memory_order_acquire);
        if (context)
            QMetaObject::invokeMethod(context, []() {
                QApplication::quit();
            }, Qt::QueuedConnection);
    }
}

/* Host-thread call: if the runtime has no users left, block until its
 * thread has fully exited (including the QApplication teardown that
 * runs AFTER exec() returns). The kit's code usually lives inside the
 * plugin DLL, so callers may FreeLibrary() once this returns. */
static void piqt_runtime_wait_idle()
{
    QMutexLocker lock(&g_rt_mutex);
    if (g_rt_users == 0 && g_rt_thread) {
        g_rt_thread->wait(5000);
        delete g_rt_thread;
        g_rt_thread = nullptr;
    }
}

/* ==========================================================================
 * PiQtView - IPiPluginView over a user widget factory
 * ======================================================================== */
namespace {

class PiQtView {
public:
    PiRefCountedBase base;              /* MUST be first data member */

    PiQtView(const PiQtViewDesc& desc)
        : m_desc(desc), m_attached(false), m_runtime_held(false),
          m_pending_delete(false), m_destroy_posted(false),
          m_teardown_sem(nullptr),
          m_parent(PI_INVALID_WINDOW),
          m_widget(nullptr), m_hwnd(PI_INVALID_WINDOW), m_context(nullptr) {}

    /* ---- state shared between host thread and Qt runtime thread ---- */
    QMutex                m_mutex;
    PiQtViewDesc          m_desc;
    bool                  m_attached;
    bool                  m_runtime_held;
    bool                  m_pending_delete;
    bool                  m_destroy_posted;
    QSemaphore*           m_teardown_sem; /* set while finalize() waits */
    PiNativeWindow        m_parent;
    QWidget*              m_widget;
    PiNativeWindow        m_hwnd;       /* cached native handle */
    QObject*              m_context;    /* runtime context (stable while held) */

    /* ---- vtable ---- */
    static const IPiPluginViewVtbl s_vtbl;

    static PiQtView* from_iface(void* self_ptr) { return (PiQtView*)self_ptr; }

    /* ---- helpers (host thread) ---- */
    template <typename F>
    void post_to_qt(F fn) {
        QObject* context = m_context;
        if (!context) return;
        QMetaObject::invokeMethod(context, [this, fn]() { fn(this); },
                                  Qt::QueuedConnection);
    }

    /* ---- Qt runtime thread work ---- */
    void create_and_embed();
    void destroy_widget_now();

    /* ---- lifecycle plumbing ---- */
    void schedule_destroy();   /* host thread */
    void finalize();           /* refcount hit zero */
};

/* ---------- Qt runtime thread bodies ---------- */

void PiQtView::create_and_embed()
{
    /* The host may have detached again before we got scheduled; a destroy
     * body is queued behind us, so simply do nothing here. */
    {
        QMutexLocker lock(&m_mutex);
        if (!m_attached) return;
    }

    QWidget* w = m_desc.create_widget(m_desc.user_data);
    if (!w) {
        /* Attach failed: unwind exactly like a destroy. */
        QMutexLocker lock(&m_mutex);
        m_attached = false;
        if (m_desc.release) m_desc.release(m_desc.user_data);
        m_runtime_held = false;
        lock.unlock();
        piqt_runtime_release();
        return;
    }

#if PI_PLATFORM_WINDOWS
    /* Foreign-window embedding: make the Qt widget a child of the host's
     * container. The HWND stays owned by the Qt thread (message routing
     * works because this thread runs the event loop below). */
    HWND hwnd = (HWND)w->winId();
    if (hwnd && PI_IS_VALID_WINDOW(m_parent)) {
        SetParent(hwnd, (HWND)m_parent);
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        SetWindowLongPtr(hwnd, GWL_STYLE,
                         (style | WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN)
                         & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME
                             | WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
        RECT r; GetClientRect((HWND)m_parent, &r);
        SetWindowPos(hwnd, NULL, 0, 0, r.right, r.bottom,
                     SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        w->resize(r.right, r.bottom);
    }
    {
        QMutexLocker lock(&m_mutex);
        m_widget = w;
        m_hwnd = (PiNativeWindow)hwnd;
    }
#else
    /* TODO: X11 XEmbed / macOS NSView embedding. */
    {
        QMutexLocker lock(&m_mutex);
        m_widget = w;
        m_hwnd = PI_INVALID_WINDOW;
    }
#endif

    w->show();
}

void PiQtView::destroy_widget_now()
{
    QWidget* w;
    {
        QMutexLocker lock(&m_mutex);
        w = m_widget;
        m_widget = nullptr;
        m_hwnd = PI_INVALID_WINDOW;
    }
    if (w) {
        if (m_desc.destroy_widget)
            m_desc.destroy_widget(m_desc.user_data, w);
        else
            delete w;
    }
}

static void piqt_create_body(PiQtView* v) { v->create_and_embed(); }

static void piqt_destroy_body(PiQtView* v)
{
    v->destroy_widget_now();
    if (v->m_desc.release)
        v->m_desc.release(v->m_desc.user_data);
    piqt_runtime_release();

    bool delete_now = false;
    QSemaphore* sem = nullptr;
    {
        QMutexLocker lock(&v->m_mutex);
        v->m_runtime_held = false;
        v->m_destroy_posted = false;
        v->m_context = nullptr;          /* runtime may quit after this */
        delete_now = v->m_pending_delete;
        sem = v->m_teardown_sem;
        v->m_teardown_sem = nullptr;
    }
    if (delete_now) delete v;
    /* Wake the waiting finalize() only after the object is gone, so the
     * waiter never touches the view again. */
    if (sem) sem->release();
}

/* ---------- vtable slots (host thread) ---------- */

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
    piqt_runtime_acquire();

    QMutexLocker lock(&me->m_mutex);
    if (me->m_runtime_held && me->m_attached) {
        /* Re-attach of a live view: just move the existing window. */
        me->m_parent = parent;
        lock.unlock();
        me->post_to_qt([](PiQtView* v) {
            QWidget* w;
            {
                QMutexLocker lock(&v->m_mutex);
                w = v->m_widget;
            }
            if (!w) return;
#if PI_PLATFORM_WINDOWS
            HWND hwnd = (HWND)w->winId();
            SetParent(hwnd, (HWND)v->m_parent);
            RECT r; GetClientRect((HWND)v->m_parent, &r);
            SetWindowPos(hwnd, NULL, 0, 0, r.right, r.bottom,
                         SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            w->resize(r.right, r.bottom);
#else
            (void)w;
#endif
        });
        return PI_OK;
    }

    me->m_parent = parent;
    me->m_attached = true;
    me->m_context = g_rt_context.load(std::memory_order_acquire);
    bool already_held = me->m_runtime_held;
    me->m_runtime_held = true;
    lock.unlock();

    /* Keep user_data alive across the Qt thread boundary. */
    if (!already_held && me->m_desc.retain)
        me->m_desc.retain(me->m_desc.user_data);

    me->post_to_qt(&piqt_create_body);
    return PI_OK;
}

PiResult PI_CALL piqt_detach(void* self_ptr)
{
    PiQtView* me = PiQtView::from_iface(self_ptr);
    {
        QMutexLocker lock(&me->m_mutex);
        me->m_attached = false;
    }
    me->schedule_destroy();
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
    {
        QMutexLocker lock(&me->m_mutex);
        if (!me->m_attached) return PI_OK;
    }
    me->post_to_qt([w, h](PiQtView* v) {
        QWidget* widget;
        {
            QMutexLocker lock(&v->m_mutex);
            widget = v->m_widget;
        }
        if (widget) widget->resize(w, h);
    });
    return PI_OK;
}

PiResult PI_CALL piqt_on_idle(void* self_ptr)
{
    PiQtView* me = PiQtView::from_iface(self_ptr);
    /* The Qt loop lives in its own thread; the host's idle call only
     * nudges the dispatcher. (Calling processEvents() from the host
     * thread would be cross-thread UB — never do that.) */
    QObject* context;
    {
        QMutexLocker lock(&me->m_mutex);
        context = me->m_context;
    }
    if (context) piqt_runtime_wake();
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
    me->post_to_qt([visible](PiQtView* v) {
        QWidget* widget;
        {
            QMutexLocker lock(&v->m_mutex);
            widget = v->m_widget;
        }
        if (widget) widget->setVisible(visible != 0);
    });
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

void PiQtView::schedule_destroy()
{
    /* Deduplicate: detach() and a refcount-zero finalize() may both want
     * to post the destroy body; only one lambda may run per hold. */
    bool need_schedule = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_runtime_held && !m_destroy_posted) {
            m_destroy_posted = true;
            need_schedule = true;
        }
    }
    if (need_schedule)
        post_to_qt(&piqt_destroy_body);
}

void PiQtView::finalize()
{
    /* Refcount reached zero while runtime work may still be pending on
     * the Qt thread. This kit is typically statically linked into the
     * plugin DLL, and the host may FreeLibrary() as soon as we return —
     * so unlike detach() (which stays asynchronous) we MUST block here
     * until the Qt thread has finished executing our code. */
    bool pending = false;
    {
        QMutexLocker lock(&m_mutex);
        pending = m_runtime_held;
        m_pending_delete = pending;
    }
    if (!pending) {
        delete this;
        return;
    }

    /* If we happen to run ON the runtime thread, finish inline. */
    QObject* context = m_context;
    if (context && QThread::currentThread() == context->thread()) {
        piqt_destroy_body(this);
        return;
    }

    /* Host thread: ensure a destroy body is queued, then wait for it. */
    QSemaphore done;
    {
        QMutexLocker lock(&m_mutex);
        m_teardown_sem = &done;
        if (!m_destroy_posted) {
            m_destroy_posted = true;
            post_to_qt(&piqt_destroy_body);
        }
    }
    /* The body releases the semaphore after deleting this object; on
     * pathological timeout we deliberately leak instead of touching
     * freed memory. */
    done.tryAcquire(1, 5000);

    /* The runtime's post-exec cleanup (deleting QApplication) still runs
     * on the Qt thread AFTER our destroy body; it is code inside this
     * DLL, so join the thread before letting the host unload us. */
    piqt_runtime_wait_idle();
}

static void piqt_view_destroy(void* self_ptr)
{
    PiQtView::from_iface(self_ptr)->finalize();
}

} // namespace

/* ==========================================================================
 * Public API
 * ======================================================================== */

PiResult pi_qt_view_create(const PiQtViewDesc* desc, IPiPluginView** out_view)
{
    if (!desc || !desc->create_widget || !out_view)
        return PI_E_INVALIDARG;
    *out_view = NULL;

    PiQtView* view = new PiQtView(*desc);
    if (!view) return PI_E_OUTOFMEMORY;
    pi_refcounted_init_with_destroy(&view->base,
                                    (const IPiUnknownVtbl*)&PiQtView::s_vtbl,
                                    &piqt_view_destroy);
    *out_view = (IPiPluginView*)&view->base;
    return PI_OK;
}

QWidget* pi_qt_view_widget(IPiPluginView* view)
{
    if (!view) return NULL;
    /* The vtbl pointer sits at offset 0 of PiQtView; the interface
     * pointer we were given is exactly that address. */
    PiQtView* v = (PiQtView*)(void*)view;
    QMutexLocker lock(&v->m_mutex);
    return v->m_widget;
}

void pi_qt_view_post(IPiPluginView* view, void (*fn)(void* user), void* user)
{
    if (!view || !fn) return;
    PiQtView* v = (PiQtView*)(void*)view;
    QMetaObject::invokeMethod(v->m_context, [fn, user]() {
        fn(user);
    }, Qt::QueuedConnection);
}
