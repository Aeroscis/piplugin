/*
 * piplugin - Host-side API
 *
 * Functions for loading, managing, and unloading plugins from DLLs.
 */
#ifndef PI_PLUGIN_HOST_H
#define PI_PLUGIN_HOST_H

#include "pi_plugin_base.h"
#include "pi_plugin_factory.h"
#include "pi_plugin_host_services.h"
#include "pi_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a loaded plugin module (DLL / .so / .dylib) */
typedef struct PiPluginModule PiPluginModule;

/* --------------------------------------------------------------------------
 * Plugin module management
 * -------------------------------------------------------------------------- */

/* Load a plugin from a shared library at the given path.
 * Returns NULL on failure (see pi_plugin_module_get_load_error). The module must
 * be unloaded with pi_plugin_module_unload(). */
PI_PLUGIN_API PiPluginModule* pi_plugin_module_load(char const* path);

/* Unload a plugin module and release all associated resources. Make sure
 * all instances created from the module have been released first. */
PI_PLUGIN_API void pi_plugin_module_unload(PiPluginModule* module);

/* Human-readable description of why the last pi_plugin_module_load failed
 * (e.g. "LoadLibrary failed (err=126)"). "no error" after a successful load.
 *
 * THREAD LOCALS (W-01): the string belongs to the CALLING THREAD, so a
 * multi-threaded host can diagnose several loads at once - each thread reads
 * back its own result instead of whichever thread wrote last. Valid until the
 * next pi_plugin_module_load ON THAT SAME THREAD; never NULL. */
PI_PLUGIN_API const char* pi_plugin_module_get_load_error(void);

/* Thread-safe variant of the above: copies the calling thread's current load
 * error into the caller's buffer (always NUL-terminated; truncated to `size`
 * if it does not fit) and returns PI_OK. The copy stays valid after later
 * loads, which is what a host wants when it stores the reason for a failure.
 *
 * Returns PI_E_INVALIDARG when buf is NULL or size is 0. */
PI_PLUGIN_API PiResult pi_plugin_module_get_load_error_r(char* buf, size_t size);

/* Get the factory from a loaded module. The factory is add-ref'd for the
 * caller; release it with ->pi_release(). */
PI_PLUGIN_API PiResult pi_plugin_module_get_factory(PiPluginModule*    module,
                                                    IPiPluginFactory** out_factory);

/* --------------------------------------------------------------------------
 * Convenience: load, create instance, initialize in one call.
 * Returns PI_OK on success. The caller owns *out_plugin and must ->release().
 * 失败时 *out_plugin 与 *out_module 均为 NULL（终审约定，BLK-08）。
 *
 * NOTE: the module is kept loaded for the lifetime of the plugin instance
 * (unloading the DLL while the plugin's code is on the stack anywhere is
 * undefined behavior). Destroy the plugin before calling pi_plugin_module_unload
 * on any module you loaded yourself; the module returned via out_module
 * (if non-NULL) must be unloaded by the caller after releasing the plugin.
 */
PI_PLUGIN_API PiResult pi_plugin_host_create_plugin(char const*            dll_path,
                                                    PiGuid const*          class_guid,
                                                    IPiPluginHostServices* host,
                                                    IPiPluginBase**        out_plugin,
                                                    PiPluginModule**       out_module);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_HOST_H */
