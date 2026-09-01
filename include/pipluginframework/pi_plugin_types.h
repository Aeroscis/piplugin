/*
 * pipluginframework - Core Type Definitions
 *
 * Pure C cross-platform plugin framework with COM-style vtables.
 * All public symbols use the "pi_" prefix.
 *
 * Design principle (LV2-inspired): the core is minimal and GUI/network
 * features are OPTIONAL capabilities, discovered at runtime via
 * QueryInterface on both sides:
 *
 *   Plugin side:  PiPluginDescriptor declares capabilities
 *                 (required / optional / provided) identified by GUID.
 *   Host side:    the host is passed to the plugin as an IPiHostServices
 *                 object; GUI-specific services are queried through
 *                 IPiHostUI. A headless host simply does not expose it.
 */
#ifndef PI_PLUGIN_TYPES_H
#define PI_PLUGIN_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Platform detection
 * -------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
#  define PI_PLATFORM_WINDOWS 1
#  ifdef PI_BUILDING_FRAMEWORK
#    define PI_EXPORT __declspec(dllexport)
#  else
#    define PI_EXPORT __declspec(dllimport)
#  endif
#  define PI_LOCAL
#elif defined(__APPLE__)
#  define PI_PLATFORM_MACOS 1
#  define PI_EXPORT __attribute__((visibility("default")))
#  define PI_LOCAL  __attribute__((visibility("hidden")))
#else
#  define PI_PLATFORM_LINUX 1
#  define PI_EXPORT __attribute__((visibility("default")))
#  define PI_LOCAL  __attribute__((visibility("hidden")))
#endif

/* --------------------------------------------------------------------------
 * 128-bit GUID (COM-compatible layout)
 * -------------------------------------------------------------------------- */
typedef struct PiGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} PiGuid;

/* Macro to define a GUID inline */
#define PI_GUID(l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    { (uint32_t)(l), (uint16_t)(w1), (uint16_t)(w2), \
      { (uint8_t)(b1), (uint8_t)(b2), (uint8_t)(b3), (uint8_t)(b4), \
        (uint8_t)(b5), (uint8_t)(b6), (uint8_t)(b7), (uint8_t)(b8) } }

PI_EXPORT int pi_guid_equal(const PiGuid* a, const PiGuid* b);

/* --------------------------------------------------------------------------
 * Result codes
 * -------------------------------------------------------------------------- */
typedef int32_t PiResult;

#define PI_OK                  ((PiResult)0)
#define PI_FAIL                ((PiResult)-1)
#define PI_E_NOINTERFACE       ((PiResult)-2)
#define PI_E_INVALIDARG        ((PiResult)-3)
#define PI_E_OUTOFMEMORY       ((PiResult)-4)
#define PI_E_NOTIMPL           ((PiResult)-5)
#define PI_E_UNEXPECTED        ((PiResult)-6)
#define PI_E_NOTFOUND          ((PiResult)-7)
#define PI_E_MISSINGCAPABILITY ((PiResult)-8)  /* required host capability absent */

#define PI_SUCCEEDED(r)        ((PiResult)(r) >= 0)
#define PI_FAILED(r)           ((PiResult)(r) < 0)

/* --------------------------------------------------------------------------
 * Opaque native window handle
 * -------------------------------------------------------------------------- */
#if PI_PLATFORM_WINDOWS
typedef void* PiNativeWindow;
#elif PI_PLATFORM_MACOS
typedef void* PiNativeWindow;
#elif PI_PLATFORM_LINUX
typedef unsigned long PiNativeWindow;
#endif

#define PI_INVALID_WINDOW ((PiNativeWindow)0)
#define PI_IS_VALID_WINDOW(h) ((h) != PI_INVALID_WINDOW)

/* --------------------------------------------------------------------------
 * Forward declarations of core interfaces
 * -------------------------------------------------------------------------- */
typedef struct IPiUnknown       IPiUnknown;
typedef struct IPiHostServices  IPiHostServices;
typedef struct IPiHostUI        IPiHostUI;
typedef struct IPiPluginFactory IPiPluginFactory;
typedef struct IPiPluginBase    IPiPluginBase;
typedef struct IPiPluginView    IPiPluginView;
typedef struct IPiService       IPiService;

/* --------------------------------------------------------------------------
 * Capability declaration (LV2-style feature negotiation)
 *
 * A plugin declares, in its descriptor, which interfaces it
 *   - REQUIRES from the host   (e.g. PI_IID_HOST_UI: needs a GUI host)
 *   - OPTIONALLY uses          (degrades gracefully if absent)
 *   - PROVIDES                 (e.g. PI_IID_PLUGIN_VIEW, PI_IID_SERVICE)
 *
 * The host can inspect this list BEFORE instantiating the plugin, e.g. a
 * headless task server only instantiates plugins that PROVIDE PI_IID_SERVICE,
 * and skips (or rejects) plugins that REQUIRE PI_IID_HOST_UI.
 * -------------------------------------------------------------------------- */
#define PI_CAP_REQUIRED  ((uint32_t)1)  /* host must provide, else init fails */
#define PI_CAP_OPTIONAL  ((uint32_t)2)  /* plugin uses it if the host has it  */
#define PI_CAP_PROVIDES  ((uint32_t)4)  /* plugin implements this interface   */

typedef struct PiPluginCapability {
    PiGuid   iid;    /* capability / interface GUID */
    uint32_t flags;  /* combination of PI_CAP_* */
} PiPluginCapability;

/* --------------------------------------------------------------------------
 * Plugin descriptor returned by the shared library entry point
 * -------------------------------------------------------------------------- */
typedef struct PiPluginDescriptor {
    const char* name;        /* Human-readable plugin name */
    const char* vendor;      /* Plugin vendor/author */
    const char* version;     /* Semantic version string, e.g. "1.0.0" */
    const char* category;    /* Plugin category */
    uint32_t    api_version; /* pipluginframework API version used */

    /* Capability declaration list (may be NULL if capability_count == 0) */
    const PiPluginCapability* capabilities;
    uint32_t                  capability_count;
} PiPluginDescriptor;

/* Check whether the descriptor declares capability `iid` with the wanted
 * flags. Returns the matching entry, or NULL. */
PI_EXPORT const PiPluginCapability* pi_descriptor_find_capability(
    const PiPluginDescriptor* desc, const PiGuid* iid);

/* Convenience: does the plugin provide / require the given capability? */
PI_EXPORT int pi_descriptor_provides(const PiPluginDescriptor* desc, const PiGuid* iid);
PI_EXPORT int pi_descriptor_requires(const PiPluginDescriptor* desc, const PiGuid* iid);

#define PI_API_VERSION 0x00010000  /* major.minor.patch -> 1.1.0 */

/* --------------------------------------------------------------------------
 * Host-side message posted by plugins via IPiHostServices::pi_post_message.
 * Framework reserves codes below 0x80000000; plugins may use anything else.
 * -------------------------------------------------------------------------- */
#define PI_MSG_NONE 0u

/* --------------------------------------------------------------------------
 * Entry point that every plugin DLL must export
 * -------------------------------------------------------------------------- */
typedef PiResult (*PiPluginEntryProc)(IPiPluginFactory** out_factory);

#define PI_PLUGIN_ENTRY_NAME "pi_plugin_entry"
#define PI_PLUGIN_ENTRY_DECL PI_EXPORT PiResult pi_plugin_entry(IPiPluginFactory** out_factory)

/* --------------------------------------------------------------------------
 * Known interface GUIDs (for IPiUnknown::pi_query_interface)
 * -------------------------------------------------------------------------- */
extern PI_EXPORT const PiGuid PI_IID_UNKNOWN;         /* IPiUnknown        */
extern PI_EXPORT const PiGuid PI_IID_HOST_SERVICES;   /* IPiHostServices   */
extern PI_EXPORT const PiGuid PI_IID_HOST_UI;         /* IPiHostUI         */
extern PI_EXPORT const PiGuid PI_IID_PLUGIN_FACTORY;  /* IPiPluginFactory  */
extern PI_EXPORT const PiGuid PI_IID_PLUGIN_BASE;     /* IPiPluginBase     */
extern PI_EXPORT const PiGuid PI_IID_PLUGIN_VIEW;     /* IPiPluginView     */
extern PI_EXPORT const PiGuid PI_IID_SERVICE;         /* IPiService        */

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_TYPES_H */
