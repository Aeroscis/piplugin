/*
 * piplugin - ImGui UI adapter kit (piplugin_imgui)
 *
 * Lets a plugin draw its UI with Dear ImGui inside ANY piplugin
 * host (Qt, wxWidgets, imgui itself, ...), without the host knowing
 * Dear ImGui.
 *
 * Threading model - HOST GUI THREAD (the exact opposite of the Qt kit):
 *   ImGui is immediate-mode and has no event loop of its own, so there
 *   is no "foreign loop" to merge. The kit simply creates a native child
 *   window inside the host's container, renders one ImGui frame on every
 *   IPiPluginView::pi_on_idle() call, and relies on the host's message
 *   pump to dispatch input to the child window. No threads anywhere.
 *
 *   Contract: pi_attach / pi_on_idle / pi_on_resize / pi_detach must all
 *   be called from the host's GUI thread (which is the natural way hosts
 *   drive views anyway).
 *
 * Plugin authors only write a draw callback:
 *
 *   static void DrawUi(void* user) {
 *       ImGui::Begin("My Plugin");
 *       ... ImGui::Text / Slider / Button ...
 *       ImGui::End();
 *   }
 *
 *   // inside IPiPluginBase::pi_get_view:
 *   PiImGuiViewDesc desc = {};
 *   desc.init = &SetupStyle;   // optional, runs once
 *   desc.draw = &DrawUi;
 *   desc.user_data = this;
 *   pi_imgui_view_create(&desc, out);
 *
 * The kit creates its OWN ImGui context and swaps it in for the duration
 * of init/draw calls, restoring whatever context was current before -
 * so it composes even inside a host that itself uses Dear ImGui.
 */
#ifndef PI_IMGUI_VIEW_H
#define PI_IMGUI_VIEW_H

#include "piplugin/pi_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * View descriptor
 * -------------------------------------------------------------------------- */

/* One-time setup, called after the ImGui context exists (styles, fonts).
 * The plugin's context is current during the call. Optional. */
typedef void (*PiImGuiInitProc)(void* user_data);

/* Per-frame drawing, called between ImGui::NewFrame() and Render().
 * The plugin's context is current during the call. Required. */
typedef void (*PiImGuiDrawProc)(void* user_data);

/* Optional refcount hooks around user_data (attach / teardown). */
typedef void (*PiImGuiRetainProc)(void* user_data);
typedef void (*PiImGuiReleaseProc)(void* user_data);

typedef struct PiImGuiViewDesc {
    PiImGuiInitProc   init;      /* optional */
    PiImGuiDrawProc   draw;      /* required */
    PiImGuiRetainProc retain;    /* optional */
    PiImGuiReleaseProc release;  /* optional */
    void*             user_data; /* passed to all callbacks */
} PiImGuiViewDesc;

/* Create an ImGui-backed IPiPluginView. Nothing is created until the
 * host calls pi_attach(parent_window); teardown happens on pi_detach()
 * or when the last reference is released. Refcount starts at 1. */
PiResult pi_imgui_view_create(const PiImGuiViewDesc* desc, IPiPluginView** out_view);

#ifdef __cplusplus
}
#endif

#endif /* PI_IMGUI_VIEW_H */
