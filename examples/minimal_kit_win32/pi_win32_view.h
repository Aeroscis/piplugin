/*
 * piplugin example - a minimal, toolkit-free UI adapter kit (Win32 GDI)
 *
 * This is the executable appendix of docs/design/adapter-spec.md: an adapter kit
 * written from that spec alone, with NO toolkit beyond Win32, in ~250 lines. Use
 * it to see what the contract actually demands of a kit, or as the skeleton for a
 * new one (gtk / webview / SDL / ...).
 *
 * What the kit does:
 *   - creates ONE child window inside the host's container when the host attaches;
 *   - paints by calling the plugin's paint callback with an HDC;
 *   - forwards resize / visibility / idle, and destroys everything on detach.
 *
 * What the kit does NOT do (the spec forbids it): create a top-level window,
 * decide layout, own a message loop, or run anything on a second thread.
 *
 * Contract from the spec that matters most:
 *   - NOTHING is created before pi_plugin_attach(): a view is a handle, not a resource;
 *   - detach is idempotent, and after it the view can be attached again;
 *   - every callback runs on the host's GUI thread (model A), so the plugin needs
 *     no locking;
 *   - destroy everything synchronously before the module is unloaded;
 *   - trace: PI_PLUGIN_WIN32_VIEW_TRACE=1 writes the lifecycle to <exe dir>\pi_win32_view.log.
 */
#ifndef PI_PLUGIN_WIN32_VIEW_H
#define PI_PLUGIN_WIN32_VIEW_H

#include "piplugin/pi_plugin.h"

#if !defined(_WIN32) && !defined(_WIN64)
#  error "the Win32 example kit is Windows-only (that is the point: no toolkit)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Paint the plugin's UI into the kit's window. Called on the host GUI thread
 * between BeginPaint/EndPaint; draw inside (0,0,w,h). Required. */
typedef void (*PiPluginWin32PaintProc)(void* user_data, void* hdc, int32_t width, int32_t height);

/* Optional refcount hooks around user_data (attach / teardown). */
typedef void (*PiPluginWin32RetainProc)(void* user_data);
typedef void (*PiPluginWin32ReleaseProc)(void* user_data);

typedef struct PiPluginWin32ViewDesc {
    PiPluginWin32PaintProc   paint;      /* required */
    PiPluginWin32RetainProc  retain;     /* optional */
    PiPluginWin32ReleaseProc release;    /* optional */
    void*              user_data;  /* passed to every callback */
} PiPluginWin32ViewDesc;

/* Create an IPiPluginView backed by a plain Win32 child window. Refcount 1.
 * Nothing is created until the host calls pi_plugin_attach(parent). */
PiResult pi_plugin_win32_view_create(const PiPluginWin32ViewDesc* desc, IPiPluginView** out_view);

/* Destroy every live view of this kit AND flush its window class. Idempotent.
 * Call it from the plugin's pi_plugin_terminate(): the window class and the WndProc
 * belong to the plugin's module, so they must be gone before the host unloads it. */
void pi_plugin_win32_view_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_WIN32_VIEW_H */
