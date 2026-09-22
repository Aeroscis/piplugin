/*
 * piplugin - Core Type Definitions
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
#  ifdef PIPLUGIN_BUILDING
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
#define PI_E_VERSIONMISMATCH   ((PiResult)-9)  /* plugin/host api_version incompatible */

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
 * Free-form plugin metadata (descriptor key/value pairs)
 *
 * Capabilities answer "what can this plugin do in the framework's vocabulary".
 * They are a poor fit for purely descriptive facts a host may want to show or
 * filter on - supported file formats, a homepage, a licence, the toolkit the UI
 * is built with - and abusing a GUID for those never works well.
 *
 * Keys and values are UTF-8, NUL-terminated. The `pi.` prefix is reserved for
 * the framework: an app or plugin is free to use anything else (use your own
 * reverse-DNS style prefix, e.g. `com.example.thing`). Keys are compared
 * byte-for-byte (case-sensitive).
 * -------------------------------------------------------------------------- */
typedef struct PiPluginProperty {
    const char* key;    /* UTF-8, NUL-terminated, non-NULL */
    const char* value;  /* UTF-8, NUL-terminated, non-NULL */
} PiPluginProperty;

/* --------------------------------------------------------------------------
 * Plugin descriptor returned by the shared library entry point
 * -------------------------------------------------------------------------- */
typedef struct PiPluginDescriptor {
    const char* name;        /* Human-readable plugin name */
    const char* vendor;      /* Plugin vendor/author */
    const char* version;     /* Semantic version string, e.g. "1.0.0" */
    const char* category;    /* Plugin category */
    uint32_t    api_version; /* piplugin API version used */

    /* Capability declaration list (may be NULL if capability_count == 0) */
    const PiPluginCapability* capabilities;
    uint32_t                  capability_count;

    /* Free-form metadata (may be NULL if property_count == 0).
     *
     * APPENDED, not inserted: this is a binary layout change (0.x is allowed to
     * change ABI; 1.0 is where the freezing promise starts - interfaces.md 1.5),
     * so a module compiled against 0.2 and loaded by a 0.3 host would read
     * garbage here. The api_version gate is what keeps that from happening:
     * plugins and hosts must be rebuilt together across an x release. */
    const PiPluginProperty*   properties;
    uint32_t                  property_count;
} PiPluginDescriptor;

/* Check whether the descriptor declares capability `iid` with the wanted
 * flags. Returns the matching entry, or NULL. */
PI_EXPORT const PiPluginCapability* pi_descriptor_find_capability(
    const PiPluginDescriptor* desc, const PiGuid* iid);

/* Convenience: does the plugin provide / require the given capability? */
PI_EXPORT int pi_descriptor_provides(const PiPluginDescriptor* desc, const PiGuid* iid);
PI_EXPORT int pi_descriptor_requires(const PiPluginDescriptor* desc, const PiGuid* iid);

/* Value of the descriptor property `key`, or NULL when the plugin declares no
 * such property. NULL-safe for every argument (desc, its properties array, the
 * key). Duplicate keys: the first one wins.
 *
 * `properties` was APPENDED in API 0.3, so this also checks the plugin's own
 * api_version: a module compiled against 0.2 has a shorter struct, and reading
 * the appended fields out of it would read past the end of its descriptor. The
 * version gate accepts older plugins (same major), so the layout has to be
 * decided here rather than assumed. A pre-0.3 plugin therefore reports "no
 * properties" - which is the truth - instead of handing out garbage. */
PI_EXPORT const char* pi_descriptor_find_property(const PiPluginDescriptor* desc,
                                                  const char* key);

/* --------------------------------------------------------------------------
 * API 版本与协商
 *
 * 编码：高 16 位 = major（ABI 不兼容级变更），低 16 位 = minor（新增接口）。
 *
 * 宿主接受一个插件的条件（roadmap BLK-03）：
 *     major 相同  且  插件版本 <= 宿主版本
 * major 不同 = vtbl 布局可能已变，一律拒绝；插件 minor 高于宿主 = 插件可能用到
 * 宿主还没有的接口，同样拒绝。判定用 pi_api_version_compatible()。
 *
 * "谁迁就谁"：**插件迁就宿主**。宿主是自己进程的主人，不会为了某个插件升级；
 * 插件应尽量按较低的 API 版本编译，被拒时提示用户升级宿主。
 *
 * 注意：这里管的是**框架 API** 版本，不覆盖 app 自己定义的接口 —— 后者的演进
 * 方式见 docs/design/interfaces.md 5.7。
 * -------------------------------------------------------------------------- */
#define PIPLUGIN_API_VERSION_MAJOR(v) ((uint32_t)(((uint32_t)(v) >> 16) & 0xFFFFu))
#define PIPLUGIN_API_VERSION_MINOR(v) ((uint32_t)((uint32_t)(v) & 0xFFFFu))
#define PIPLUGIN_API_VERSION_MAKE(major, minor) \
    ((uint32_t)((((uint32_t)(major) & 0xFFFFu) << 16) | ((uint32_t)(minor) & 0xFFFFu)))

/* 本库自己的 API 版本，取值与发布版本的 major.minor 一致。
 *
 * 当前状态：**API 0.3 / 发布 0.2.0** —— APP-04 追加了 descriptor 字段
 * （二进制布局变化），按政策 minor 前进一位；发布版本号与 CHANGELOG 在切 0.3.0
 * 时才跟上（发布是一条单独的 release 提交，见 CHANGELOG 顶部）。
 *
 * 1.0 是"ABI 冻结承诺"的时刻：在那之前每个 x 版本都可以改 ABI，
 * 所以插件应随宿主一起升级；升级时同步 CHANGELOG.md 与 interfaces.md 1.5。 */
#define PIPLUGIN_API_VERSION PIPLUGIN_API_VERSION_MAKE(0, 3)

/* 宿主版本与插件版本是否兼容。返回非 0 = 可以加载。 */
PI_EXPORT int pi_api_version_compatible(uint32_t host_version, uint32_t plugin_version);

/* --------------------------------------------------------------------------
 * Host-side message posted by plugins via IPiHostServices::pi_post_message.
 * Framework reserves codes below 0x80000000; plugins may use anything else.
 * -------------------------------------------------------------------------- */
#define PI_MSG_NONE 0u

/* --------------------------------------------------------------------------
 * Entry point that every plugin DLL must export
 * -------------------------------------------------------------------------- */
typedef PiResult (*PiPluginEntryProc)(IPiPluginFactory** out_factory);

/* 插件侧导出宏：插件 DLL 永远是"导出方"，与 PI_EXPORT 相反 —— PI_EXPORT 在
 * 非 PIPLUGIN_BUILDING 的翻译单元里展开成 dllimport，直接拿它去**定义**
 * 入口会编译失败（"definition of dllimport function not allowed"）。 */
#if PI_PLATFORM_WINDOWS
#  define PI_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define PI_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define PI_PLUGIN_ENTRY_NAME "pi_plugin_entry"

/* 在插件里定义入口就用这个宏：
 *     PI_PLUGIN_ENTRY_DECL
 *     {
 *         if (!out_factory) return PI_E_INVALIDARG;
 *         ...
 *         return PI_OK;
 *     }
 */
#define PI_PLUGIN_ENTRY_DECL PI_PLUGIN_EXPORT PiResult pi_plugin_entry(IPiPluginFactory** out_factory)

/* --------------------------------------------------------------------------
 * Known interface GUIDs (for IPiUnknown::pi_query_interface)
 *
 * GUID 分配规则（规范与完整示例见 docs/design/interfaces.md §5）：
 *
 *   1) 框架接口 IID：data1 < 0x80000000，由框架在 src/pi_plugin_unknown.c 中
 *      集中分配（当前已用 data1 = 0x00000000/01/02/03/10/11/20）。
 *      **第三方不得在这个区间自行编号** —— 那是框架未来的接口保留区。
 *
 *   2) app / 第三方接口 IID、以及插件 class GUID：必须使用**随机生成的
 *      128 位 UUID**（`uuidgen`、`python -c "import uuid;print(uuid.uuid4())"`、
 *      任意 GUID 生成器都行），作为常量写进你自己的头文件：
 *
 *          static const PiGuid MY_IID =
 *              PI_GUID(0x9F3C1D42, 0x7B08, 0x4E55, 0xA1, 0x6C, 0x0D, 0xF2, 0x88, 0x37, 0x51, 0xBE);
 *
 *      判定由 pi_guid_equal 对**完整 128 位**比较，随机值的碰撞概率可忽略；
 *      不要手工编造"看起来像框架编号"的小整数 GUID（例如 0x00000021）。
 *
 *   3) IID 一旦随 PUBLIC 版本发布就不可再改（COM 规则：已发布接口不可变，
 *      要改只能新增一个 IID 并新增接口）。
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
