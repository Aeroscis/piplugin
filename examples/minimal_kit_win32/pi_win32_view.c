/*
 * piplugin example - minimal Win32 adapter kit (implementation)
 *
 * See pi_win32_view.h and docs/design/adapter-spec.md. This file is deliberately
 * plain C: a kit does not need C++ (the Qt kit does because Qt is C++, which is
 * exactly the coupling this framework exists to avoid).
 *
 * Lifecycle (one child window per view, all on the host GUI thread):
 *
 *   pi_win32_view_create()  -> just an object; no window, no class yet
 *   pi_attach(parent)       -> register class (once per module), create the child
 *                              window, retain(user_data), paint once
 *   pi_on_idle()            -> invalidate + update -> WM_PAINT -> plugin paints
 *   pi_on_resize(w, h)      -> SetWindowPos (the child follows the container)
 *   pi_set_visible(flag)    -> ShowWindow
 *   pi_detach()             -> destroy window, release(user_data); idempotent
 *   pi_release()            -> detach if still attached, then free the object
 *   pi_win32_view_shutdown()-> force every live view through detach (called from
 *                              the plugin's pi_terminate, before FreeLibrary)
 */
#include "pi_win32_view.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_WIN32_VIEW_CLASS_PREFIX L"PiWin32ExampleView"

typedef struct PiWin32View {
    PiRefCountedBase      base;        /* MUST be first */
    PiWin32ViewDesc       desc;
    HWND                  hwnd;
    int                   attached;
} PiWin32View;

/* Live views of this module (a kit must be able to force a teardown from
 * pi_terminate even if the host forgets to detach - see the spec's shutdown
 * contract). Single-threaded, so a plain list is enough. */
#define PI_WIN32_VIEW_MAX_LIVE 16
static PiWin32View* g_live[PI_WIN32_VIEW_MAX_LIVE];
static int          g_class_registered = 0;
/* Unique per MODULE: a fixed class name is a process-wide resource, and Windows
 * does not drop a class when the registering module is unloaded - the next plugin
 * using this kit would then create its window with the previous module's WndProc. */
static WCHAR        g_class_name[64];

static LRESULT CALLBACK PiWin32ViewWndProc(HWND hwnd, UINT msg,
                                           WPARAM wparam, LPARAM lparam);

/* The module that owns this kit's code - i.e. the PLUGIN DLL that linked the kit.
 * The window class must be registered against that module: Windows then
 * unregisters it when the module goes away, and the WndProc pointer cannot
 * outlive its own code. */
static HMODULE PiWin32KitModule(void)
{
    HMODULE module = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&PiWin32ViewWndProc, &module);
    return module;
}

/* --------------------------------------------------------------------------
 * Optional lifecycle trace (the spec's PI_<KIT>_TRACE=1 convention)
 * -------------------------------------------------------------------------- */
static int PiWin32TraceEnabled(void)
{
    char value[8] = { 0 };
    DWORD n = GetEnvironmentVariableA("PI_WIN32_VIEW_TRACE", value, sizeof(value));
    return n > 0 && value[0] != '0';
}

static void PiWin32Trace(const char* fmt, ...)
{
    static FILE* log = NULL;
    va_list ap;
    char line[256];

    if (!log) {
        char path[MAX_PATH];
        DWORD len;
        if (!PiWin32TraceEnabled()) return;              /* disabled: do nothing */
        len = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;
        {
            char* slash = strrchr(path, '\\');
            if (!slash) return;
            *(slash + 1) = 0;
        }
        strncat_s(path, sizeof(path), "pi_win32_view.log", _TRUNCATE);
        if (fopen_s(&log, path, "a") != 0) { log = NULL; return; }
    }

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(log, "[win32view %6lu] %s\n", (unsigned long)GetCurrentThreadId(), line);
    fflush(log);
}

static void PiWin32RegisterClass(void)
{
    WNDCLASSEXW wc;
    HMODULE module = PiWin32KitModule();
    if (g_class_registered) return;

    _snwprintf_s(g_class_name, _countof(g_class_name), _TRUNCATE,
                 PI_WIN32_VIEW_CLASS_PREFIX L"_%p", (void*)module);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &PiWin32ViewWndProc;
    wc.hInstance     = module;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = g_class_name;
    if (!RegisterClassExW(&wc)) {
        /* Same name still registered (this module reloaded at the same base):
         * drop it and try once more. It has no windows left, otherwise this
         * view's own shutdown would not have run. */
        UnregisterClassW(g_class_name, module);
        if (!RegisterClassExW(&wc)) return;
    }
    g_class_registered = 1;
}

/* --------------------------------------------------------------------------
 * Window plumbing
 * -------------------------------------------------------------------------- */
static void PiWin32PaintNow(PiWin32View* me)
{
    PAINTSTRUCT ps;
    HDC dc;
    RECT rc;

    if (!me->hwnd || !me->desc.paint) return;
    GetClientRect(me->hwnd, &rc);
    dc = BeginPaint(me->hwnd, &ps);
    if (dc) {
        me->desc.paint(me->desc.user_data, (void*)dc, rc.right, rc.bottom);
        EndPaint(me->hwnd, &ps);
    }
}

static LRESULT CALLBACK PiWin32ViewWndProc(HWND hwnd, UINT msg,
                                           WPARAM wparam, LPARAM lparam)
{
    PiWin32View* me = (PiWin32View*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT:
        if (me) { PiWin32PaintNow(me); return 0; }
        break;
    case WM_ERASEBKGND:
        if (me) return 1;                  /* the paint callback owns the pixels */
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* --------------------------------------------------------------------------
 * Live-view registry
 * -------------------------------------------------------------------------- */
static void PiWin32AddLive(PiWin32View* me)
{
    int i;
    for (i = 0; i < PI_WIN32_VIEW_MAX_LIVE; ++i) {
        if (g_live[i] == me) return;
    }
    for (i = 0; i < PI_WIN32_VIEW_MAX_LIVE; ++i) {
        if (!g_live[i]) { g_live[i] = me; return; }
    }
}

static void PiWin32RemoveLive(PiWin32View* me)
{
    int i;
    for (i = 0; i < PI_WIN32_VIEW_MAX_LIVE; ++i) {
        if (g_live[i] == me) g_live[i] = NULL;
    }
}

/* --------------------------------------------------------------------------
 * IPiPluginView slots (host GUI thread)
 * -------------------------------------------------------------------------- */
static uint32_t PI_CALL Win32View_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL Win32View_Release(void* self) { return pi_refcounted_release(self); }

static PiResult PI_CALL Win32View_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_IID_PLUGIN_VIEW)) {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiResult PI_CALL Win32View_Attach(void* self_ptr, PiNativeWindow parent)
{
    PiWin32View* me = (PiWin32View*)self_ptr;
    RECT rc;

    if (!PI_IS_VALID_WINDOW(parent)) return PI_E_INVALIDARG;
    if (me->attached) return PI_FAIL;               /* attach once per detach */

    PiWin32RegisterClass();
    GetClientRect((HWND)parent, &rc);

    me->hwnd = CreateWindowExW(0, g_class_name, L"",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, rc.right, rc.bottom,
                               (HWND)parent, NULL, PiWin32KitModule(), NULL);
    if (!me->hwnd) return PI_FAIL;

    SetWindowLongPtr(me->hwnd, GWLP_USERDATA, (LONG_PTR)me);
    me->attached = 1;
    PiWin32AddLive(me);
    if (me->desc.retain) me->desc.retain(me->desc.user_data);

    PiWin32Trace("attach: hwnd=%p container=%p size=%ldx%ld",
                 (void*)me->hwnd, (void*)parent, (long)rc.right, (long)rc.bottom);
    UpdateWindow(me->hwnd);                        /* first paint right away */
    return PI_OK;
}

static PiResult PI_CALL Win32View_Detach(void* self_ptr)
{
    PiWin32View* me = (PiWin32View*)self_ptr;
    if (!me->attached) return PI_OK;               /* idempotent */

    me->attached = 0;
    PiWin32Trace("detach: hwnd=%p", (void*)me->hwnd);

    /* Order matters: unhook the WndProc before DestroyWindow re-enters it, and
     * release the plugin's reference only after the window is gone. */
    if (me->hwnd) {
        SetWindowLongPtr(me->hwnd, GWLP_USERDATA, (LONG_PTR)NULL);
        DestroyWindow(me->hwnd);
        me->hwnd = NULL;
    }
    PiWin32RemoveLive(me);
    if (me->desc.release) me->desc.release(me->desc.user_data);
    return PI_OK;
}

static PiNativeWindow PI_CALL Win32View_GetNativeWindow(void* self_ptr)
{
    PiWin32View* me = (PiWin32View*)self_ptr;
    return (PiNativeWindow)me->hwnd;               /* borrowed handle */
}

static PiResult PI_CALL Win32View_OnResize(void* self_ptr, int32_t w, int32_t h)
{
    PiWin32View* me = (PiWin32View*)self_ptr;
    if (!me->attached || !me->hwnd) return PI_OK;
    if (w <= 0 || h <= 0) return PI_OK;
    SetWindowPos(me->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    PiWin32Trace("resize: %ldx%ld", (long)w, (long)h);
    return PI_OK;
}

static PiResult PI_CALL Win32View_OnIdle(void* self_ptr)
{
    PiWin32View* me = (PiWin32View*)self_ptr;
    if (!me->attached || !me->hwnd) return PI_OK;

    /* The host's loop is our clock: one invalidate + update per call. The host
     * decides the cadence; the kit never creates a timer of its own. */
    InvalidateRect(me->hwnd, NULL, FALSE);
    UpdateWindow(me->hwnd);
    return PI_OK;
}

static PiResult PI_CALL Win32View_GetPreferredSize(void* self_ptr, int32_t* w, int32_t* h)
{
    (void)self_ptr;
    if (w) *w = 0;
    if (h) *h = 0;
    /* The honest answer for "no opinion". The spec recommends this over inventing
     * a number (freeze review F7 records that both official kits hard-coded
     * 400x300 and should not have). */
    return PI_E_NOTIMPL;
}

static PiResult PI_CALL Win32View_SetVisible(void* self_ptr, int32_t visible)
{
    PiWin32View* me = (PiWin32View*)self_ptr;
    if (me->hwnd) ShowWindow(me->hwnd, visible ? SW_SHOW : SW_HIDE);
    return PI_OK;
}

static const IPiPluginViewVtbl s_win32_view_vtbl = {
    { &Win32View_Qi, &Win32View_AddRef, &Win32View_Release },
    &Win32View_Attach,
    &Win32View_Detach,
    &Win32View_GetNativeWindow,
    &Win32View_OnResize,
    &Win32View_OnIdle,
    &Win32View_GetPreferredSize,
    &Win32View_SetVisible
};

static void Win32View_Destroy(void* self)
{
    PiWin32View* me = (PiWin32View*)self;
    Win32View_Detach(me);       /* no-op when already detached */
    PiWin32Trace("view destroyed");
    free(me);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
PiResult pi_win32_view_create(const PiWin32ViewDesc* desc, IPiPluginView** out_view)
{
    PiWin32View* me;

    if (!desc || !desc->paint || !out_view) return PI_E_INVALIDARG;
    *out_view = NULL;

    me = (PiWin32View*)calloc(1, sizeof(PiWin32View));
    if (!me) return PI_E_OUTOFMEMORY;

    pi_refcounted_init_with_destroy(&me->base, (const IPiUnknownVtbl*)&s_win32_view_vtbl,
                                    &Win32View_Destroy);
    me->desc = *desc;
    *out_view = (IPiPluginView*)&me->base;
    return PI_OK;
}

void pi_win32_view_shutdown(void)
{
    int i;
    HMODULE module = PiWin32KitModule();

    for (i = 0; i < PI_WIN32_VIEW_MAX_LIVE; ++i) {
        if (g_live[i]) Win32View_Detach(g_live[i]);
    }
    if (g_class_registered) {
        UnregisterClassW(g_class_name, module);
        g_class_registered = 0;
    }
    PiWin32Trace("shutdown done");
}
