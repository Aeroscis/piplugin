/*
 * pipluginframework - Host-side API
 *
 * Functions for loading, managing, and unloading plugins from DLLs.
 */
#ifndef PI_PLUGIN_HOST_H
#define PI_PLUGIN_HOST_H

#include "pi_plugin_types.h"
#include "pi_plugin_factory.h"
#include "pi_plugin_base.h"
#include "pi_plugin_host_services.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a loaded plugin module (DLL / .so / .dylib) */
typedef struct PiPluginModule PiPluginModule;

/* --------------------------------------------------------------------------
 * Plugin module management
 * -------------------------------------------------------------------------- */

/* Load a plugin from a shared library at the given path.
 * Returns NULL on failure (see pi_module_get_load_error). The module must
 * be unloaded with pi_module_unload(). */
PI_EXPORT PiPluginModule* pi_module_load(const char* path);

/* Unload a plugin module and release all associated resources. Make sure
 * all instances created from the module have been released first. */
PI_EXPORT void pi_module_unload(PiPluginModule* module);

/* Human-readable description of why the last pi_module_load failed
 * (e.g. "LoadLibrary failed (err=126)"). Valid until the next call. */
PI_EXPORT const char* pi_module_get_load_error(void);

/* Get the factory from a loaded module. The factory is add-ref'd for the
 * caller; release it with ->pi_release(). */
PI_EXPORT PiResult pi_module_get_factory(PiPluginModule* module,
                                          IPiPluginFactory** out_factory);

/* --------------------------------------------------------------------------
 * Convenience: load, create instance, initialize in one call.
 * Returns PI_OK on success. The caller owns *out_plugin and must ->release().
 * 失败时 *out_plugin 与 *out_module 均为 NULL（终审约定，BLK-08）。
 *
 * NOTE: the module is kept loaded for the lifetime of the plugin instance
 * (unloading the DLL while the plugin's code is on the stack anywhere is
 * undefined behavior). Destroy the plugin before calling pi_module_unload
 * on any module you loaded yourself; the module returned via out_module
 * (if non-NULL) must be unloaded by the caller after releasing the plugin.
 */
PI_EXPORT PiResult pi_host_create_plugin(const char* dll_path,
                                          const PiGuid* class_guid,
                                          IPiHostServices* host,
                                          IPiPluginBase** out_plugin,
                                          PiPluginModule** out_module);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_HOST_H */
