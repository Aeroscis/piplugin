/*
 * piplugin - core type definitions
 *
 * Pure C, COM-style vtables, C ABI. This library's own names carry the
 * `pi_plugin_` / `PiPlugin` / `PI_PLUGIN_` prefix. The vocabulary shared by
 * every PI library - result codes, the 128-bit GUID, the native window handle,
 * the IPiUnknown root interface, and the ABI/platform plumbing - comes from
 * pibase and keeps its bare `pi_` / `Pi` / `PI_` prefix; see
 * <pibase/pi_base.h> for that rule and for where the boundary is drawn.
 *
 * Design principle (LV2-inspired): the core is minimal and GUI/network
 * features are OPTIONAL capabilities, discovered at runtime via
 * QueryInterface on both sides:
 *
 *   Plugin side:  PiPluginDescriptor declares capabilities
 *                 (required / optional / provided) identified by GUID.
 *   Host side:    the host is passed to the plugin as an IPiPluginHostServices
 *                 object; GUI-specific services are queried through
 *                 IPiPluginHostUI. A headless host simply does not expose it.
 */
#ifndef PI_PLUGIN_TYPES_H
#define PI_PLUGIN_TYPES_H

#include <pibase/pi_base.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Export marker for this library's own public symbols
 *
 * The visibility *attribute* is family vocabulary (pibase provides PI_EXPORT /
 * PI_IMPORT); which one applies is this library's business, because only it
 * knows whether it is being built or consumed.
 * -------------------------------------------------------------------------- */
#ifdef PI_PLUGIN_BUILDING
    #define PI_PLUGIN_API PI_EXPORT
#else
    #define PI_PLUGIN_API PI_IMPORT
#endif

/* --------------------------------------------------------------------------
 * Forward declarations of core interfaces
 *
 * IPiUnknown is deliberately absent: it is family vocabulary and comes from
 * pibase, which also defines PI_IID_UNKNOWN.
 * -------------------------------------------------------------------------- */
typedef struct IPiPluginHostServices IPiPluginHostServices;
typedef struct IPiPluginHostUI       IPiPluginHostUI;
typedef struct IPiPluginFactory      IPiPluginFactory;
typedef struct IPiPluginBase         IPiPluginBase;
typedef struct IPiPluginView         IPiPluginView;
typedef struct IPiPluginService      IPiPluginService;

/* --------------------------------------------------------------------------
 * Capability declaration (LV2-style feature negotiation)
 *
 * A plugin declares, in its descriptor, which interfaces it
 *   - REQUIRES from the host   (e.g. PI_PLUGIN_IID_HOST_UI: needs a GUI host)
 *   - OPTIONALLY uses          (degrades gracefully if absent)
 *   - PROVIDES                 (e.g. PI_PLUGIN_IID_PLUGIN_VIEW, PI_PLUGIN_IID_SERVICE)
 *
 * The host can inspect this list BEFORE instantiating the plugin, e.g. a
 * headless task server only instantiates plugins that PROVIDE PI_PLUGIN_IID_SERVICE,
 * and skips (or rejects) plugins that REQUIRE PI_PLUGIN_IID_HOST_UI.
 * -------------------------------------------------------------------------- */
#define PI_PLUGIN_CAP_REQUIRED ((uint32_t)1) /* host must provide, else init fails */
#define PI_PLUGIN_CAP_OPTIONAL ((uint32_t)2) /* plugin uses it if the host has it  */
#define PI_PLUGIN_CAP_PROVIDES ((uint32_t)4) /* plugin implements this interface   */

typedef struct PiPluginCapability {
    PiGuid   iid;   /* capability / interface GUID */
    uint32_t flags; /* combination of PI_CAP_* */
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
    char const* key;   /* UTF-8, NUL-terminated, non-NULL */
    char const* value; /* UTF-8, NUL-terminated, non-NULL */
} PiPluginProperty;

/* --------------------------------------------------------------------------
 * Plugin descriptor returned by the shared library entry point
 * -------------------------------------------------------------------------- */
typedef struct PiPluginDescriptor {
    char const* name;        /* Human-readable plugin name */
    char const* vendor;      /* Plugin vendor/author */
    char const* version;     /* Semantic version string, e.g. "1.0.0" */
    char const* category;    /* Plugin category */
    uint32_t    api_version; /* piplugin API version used */

    /* Capability declaration list (may be NULL if capability_count == 0) */
    PiPluginCapability const* capabilities;
    uint32_t                  capability_count;

    /* Free-form metadata (may be NULL if property_count == 0).
     *
     * APPENDED, not inserted: this is a binary layout change (0.x is allowed to
     * change ABI; 1.0 is where the freezing promise starts - interfaces.md 1.5),
     * so a module compiled against 0.2 and loaded by a 0.3 host would read
     * garbage here. The api_version gate is what keeps that from happening:
     * plugins and hosts must be rebuilt together across an x release. */
    PiPluginProperty const* properties;
    uint32_t                property_count;
} PiPluginDescriptor;

/* --------------------------------------------------------------------------
 * Initializing a descriptor
 *
 * FILL IT IN ZEROED, then set the fields you care about:
 *
 *     PiPluginDescriptor desc;
 *     pi_plugin_descriptor_init(&desc);      // memset(0): every optional field is "absent"
 *     desc.name = "My Plugin";
 *     ...
 *
 * Why it matters: the descriptor has OPTIONAL fields that get APPENDED over time
 * (properties/property_count arrived in API 0.3). A descriptor with automatic or
 * dynamic storage keeps whatever the memory held - in a Debug build that is
 * 0xCDCDCDCD - and a host that walks `properties` because it read a garbage
 * `property_count` will crash inside ITSELF, which is a miserable thing to debug
 * from the plugin author's side. (Found the hard way: examples/minimal_plugin_imgui
 * crashed the imgui test host this way.) Static/global descriptors are zeroed by
 * the language and are fine either way.
 * -------------------------------------------------------------------------- */
static inline void pi_plugin_descriptor_init(PiPluginDescriptor* desc)
{
    if (!desc)
    {
        return;
    }
    desc->name             = NULL;
    desc->vendor           = NULL;
    desc->version          = NULL;
    desc->category         = NULL;
    desc->api_version      = 0;
    desc->capabilities     = NULL;
    desc->capability_count = 0;
    desc->properties       = NULL;
    desc->property_count   = 0;
}

/* Check whether the descriptor declares capability `iid` with the wanted
 * flags. Returns the matching entry, or NULL. */
PI_PLUGIN_API const PiPluginCapability* pi_plugin_descriptor_find_capability(
    PiPluginDescriptor const* desc, PiGuid const* iid);

/* Convenience: does the plugin provide / require the given capability? */
PI_PLUGIN_API int pi_plugin_descriptor_provides(PiPluginDescriptor const* desc, PiGuid const* iid);
PI_PLUGIN_API int pi_plugin_descriptor_requires(PiPluginDescriptor const* desc, PiGuid const* iid);

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
PI_PLUGIN_API const char* pi_plugin_descriptor_find_property(PiPluginDescriptor const* desc,
                                                             char const*               key);

/* --------------------------------------------------------------------------
 * API 版本与协商
 *
 * 编码：高 16 位 = major（ABI 不兼容级变更），低 16 位 = minor（新增接口）。
 *
 * 宿主接受一个插件的条件（roadmap BLK-03）：
 *     major 相同  且  插件版本 <= 宿主版本
 * major 不同 = vtbl 布局可能已变，一律拒绝；插件 minor 高于宿主 = 插件可能用到
 * 宿主还没有的接口，同样拒绝。判定用 pi_plugin_api_version_compatible()。
 *
 * "谁迁就谁"：**插件迁就宿主**。宿主是自己进程的主人，不会为了某个插件升级；
 * 插件应尽量按较低的 API 版本编译，被拒时提示用户升级宿主。
 *
 * 注意：这里管的是**框架 API** 版本，不覆盖 app 自己定义的接口 —— 后者的演进
 * 方式见 docs/design/interfaces.md 5.7。
 * -------------------------------------------------------------------------- */
#define PI_PLUGIN_API_VERSION_MAJOR(v) ((uint32_t)(((uint32_t)(v) >> 16) & 0xFFFFu))
#define PI_PLUGIN_API_VERSION_MINOR(v) ((uint32_t)((uint32_t)(v) & 0xFFFFu))
#define PI_PLUGIN_API_VERSION_MAKE(major, minor) \
    ((uint32_t)((((uint32_t)(major) & 0xFFFFu) << 16) | ((uint32_t)(minor) & 0xFFFFu)))

/* 本库自己的 API 版本，**始终与发布版本一致**（当前发布 0.4.0 → API 0.4）。
 *
 * 历史：0.3 来自 APP-04（descriptor 追加 properties，二进制布局变化），
 * 0.4 来自 APP-06（新增事件接口 IPiPluginEventSink / IPiPluginHostEvents）。
 * 三处版本（CMakeLists 的 project(VERSION)、conanfile.py 的 version、这里的
 * major.minor）必须一起改 —— 单测里的版本 tripwire 就是提醒这件事的机制。
 *
 * 1.0 是"ABI 冻结承诺"的时刻：在那之前每个 x 版本都可以改 ABI，
 * 所以插件应随宿主一起升级；升级时同步 CHANGELOG.md 与 interfaces.md 1.5。 */
#define PI_PLUGIN_API_VERSION PI_PLUGIN_API_VERSION_MAKE(0, 5)

/* 宿主版本与插件版本是否兼容。返回非 0 = 可以加载。 */
PI_PLUGIN_API int pi_plugin_api_version_compatible(uint32_t host_version, uint32_t plugin_version);

/* --------------------------------------------------------------------------
 * Host-side message posted by plugins via IPiPluginHostServices::pi_plugin_post_message.
 * Framework reserves codes below 0x80000000; plugins may use anything else.
 * -------------------------------------------------------------------------- */
#define PI_PLUGIN_MSG_NONE 0u

/* --------------------------------------------------------------------------
 * Entry point that every plugin DLL must export
 * -------------------------------------------------------------------------- */
typedef PiResult (*PiPluginEntryProc)(IPiPluginFactory** out_factory);

/* 插件侧导出宏：插件 DLL 永远是"导出方"，与 PI_PLUGIN_API 相反 —— PI_PLUGIN_API 在
 * 非 PI_PLUGIN_BUILDING 的翻译单元里展开成 dllimport，直接拿它去**定义**
 * 入口会编译失败（"definition of dllimport function not allowed"）。 */
#if PI_PLATFORM_WINDOWS
    #define PI_PLUGIN_ENTRY_EXPORT __declspec(dllexport)
#else
    #define PI_PLUGIN_ENTRY_EXPORT __attribute__((visibility("default")))
#endif

#define PI_PLUGIN_ENTRY_NAME "pi_plugin_entry"

/* 入口必须是 **C 链接**：宿主是按名字 "pi_plugin_entry" 去找它的，而 C++ 的名字
 * 修饰会把它导成 `?pi_plugin_entry@@YA...`，宿主就找不到（报 "does not export"）。
 * C 里写 extern "C" 不合法，所以按语言条件展开 —— 也就是说，用这个宏的 C++ 插件
 * 不需要自己再写 extern "C"。 */
#ifdef __cplusplus
    #define PI_PLUGIN_ENTRY_LINKAGE extern "C"
#else
    #define PI_PLUGIN_ENTRY_LINKAGE
#endif

/* 在插件里定义入口就用这个宏：
 *     PI_PLUGIN_ENTRY_DECL
 *     {
 *         if (!out_factory) return PI_E_INVALIDARG;
 *         ...
 *         return PI_OK;
 *     }
 */
#define PI_PLUGIN_ENTRY_DECL \
    PI_PLUGIN_ENTRY_LINKAGE PI_PLUGIN_ENTRY_EXPORT PiResult pi_plugin_entry(IPiPluginFactory** out_factory)

/* --------------------------------------------------------------------------
 * Known interface GUIDs (for IPiUnknown::pi_query_interface)
 *
 * GUID 分配规则（规范与完整示例见 docs/design/interfaces.md §5）：
 *
 *   1) 框架接口 IID：data1 < 0x80000000，由框架在 src/pi_plugin_unknown.c 中
 *      集中分配（当前已用 data1 = 0x00000000/01/02/03/10/11/20/30/31；
 *      完整表见 docs/design/architecture.md §3，以那张表为准）。
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
 *
 * PI_IID_UNKNOWN 不在这里：它标识根接口 IPiUnknown，属家族根词汇，由基础层
 * 与 IPiUnknown 一起提供（见 <pibase/pi_base.h>）。
 * -------------------------------------------------------------------------- */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_HOST_SERVICES;  /* IPiPluginHostServices   */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_HOST_UI;        /* IPiPluginHostUI         */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_PLUGIN_FACTORY; /* IPiPluginFactory  */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_PLUGIN_BASE;    /* IPiPluginBase     */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_PLUGIN_VIEW;    /* IPiPluginView     */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_SERVICE;        /* IPiPluginService        */
/* 0.4 additions (roadmap APP-06, channel C) - interfaces first, nothing changed */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_EVENT_SINK;  /* IPiPluginEventSink      */
extern PI_PLUGIN_API const PiGuid PI_PLUGIN_IID_HOST_EVENTS; /* IPiPluginHostEvents     */

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_TYPES_H */
