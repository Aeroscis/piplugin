/*
 * piplugin - 故意声明不兼容 api_version 的测试插件（BLK-03 负向用例）
 *
 * 除了 descriptor 里的 api_version 之外，这个插件是**完全合法**的：有工厂、
 * 有一个 class、声明了 PROVIDES PLUGIN_VIEW。因此宿主拒绝它的**唯一**理由就是
 * 版本门禁；ctest 用例据此断言"宿主在 create_instance 之前就拒绝"。
 *
 * 纯 C，不依赖 Qt 与任何适配器套件。
 */
#include "piplugin/pi_plugin.h"

/* 比当前 PIPLUGIN_API_VERSION 高一个 major：major 不同 = ABI 不兼容。
 * 用 MAKE 宏表达，避免手写十六进制。 */
#define BADVERSION_API_VERSION PIPLUGIN_API_VERSION_MAKE(2, 0)

/* 完整随机的 128 位 UUID 风格 class GUID（不是框架那种小整数编号；
 * 判定按完整 128 位比较，见 docs/design/interfaces.md 5.1） */
static const PiGuid BADVERSION_CLASS_GUID =
    PI_GUID(0x2B7E4C91, 0x0D35, 0x4A88, 0x8E, 0x21, 0x5C, 0x77, 0xB0, 0x3F, 0x9D, 0x64);

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */
typedef struct BadVersionFactory {
    PiRefCountedBase base;      /* 必须是第一个数据成员 */
} BadVersionFactory;

static BadVersionFactory   s_factory;
static PiPluginCapability  s_caps[1];
static PiPluginDescriptor  s_desc;
static int                 s_initialized = 0;

static PiResult PI_CALL Factory_Qi(void* self_ptr, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &PI_IID_PLUGIN_FACTORY)) {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static const PiPluginDescriptor* PI_CALL Factory_GetDescriptor(void* self_ptr)
{
    (void)self_ptr;
    return &s_desc;
}

static uint32_t PI_CALL Factory_GetClassCount(void* self_ptr)
{
    (void)self_ptr;
    return 1;
}

static PiResult PI_CALL Factory_GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid)
{
    (void)self_ptr;
    if (index != 0 || !guid) return PI_E_INVALIDARG;
    *guid = BADVERSION_CLASS_GUID;
    return PI_OK;
}

static PiResult PI_CALL Factory_CreateInstance(void* self_ptr, const PiGuid* guid,
                                                IPiHostServices* host, IPiPluginBase** out)
{
    /* 永远不该被调用：宿主必须在 create_instance 之前就因版本不兼容拒绝。
     * 若某天真被调用到，说明版本门禁失效了 —— 返回 NOTIMPL 让用例更明显地失败。 */
    (void)self_ptr; (void)guid; (void)host;
    if (out) *out = NULL;
    return PI_E_NOTIMPL;
}

/* 本模块内的薄封装：直接把框架导出的 pi_refcounted_add_ref/release 的地址填进
 * vtbl，在 C 里会触发 warning C4232（取 dllimport 函数地址不保证跨模块标识，
 * 见 docs/design/interfaces.md 5.3）。这里用文档给出的第二种解法消掉它。 */
static uint32_t PI_CALL Factory_AddRef(void* self_ptr)
{
    return pi_refcounted_add_ref(self_ptr);
}

static uint32_t PI_CALL Factory_Release(void* self_ptr)
{
    return pi_refcounted_release(self_ptr);
}

static const IPiPluginFactoryVtbl s_factory_vtbl = {
    { &Factory_Qi, &Factory_AddRef, &Factory_Release },
    &Factory_GetDescriptor,
    &Factory_GetClassCount,
    &Factory_GetClassGuid,
    &Factory_CreateInstance
};
/* --------------------------------------------------------------------------
 * Entry point
 *
 * 用公共头的 PI_PLUGIN_ENTRY_DECL 宏（它展开成 PI_PLUGIN_EXPORT，即插件侧的
 * dllexport）——本插件同时也是这个宏的编译验证件：终审前该宏错用了 PI_EXPORT，
 * 在插件里直接拿去定义入口会编译失败（见 interface-freeze-review.md F10）。
 * -------------------------------------------------------------------------- */
PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory) return PI_E_INVALIDARG;
    *out_factory = NULL;

    if (!s_initialized) {
        pi_refcounted_init(&s_factory.base, (const IPiUnknownVtbl*)&s_factory_vtbl);

        s_caps[0].iid   = PI_IID_PLUGIN_VIEW;
        s_caps[0].flags = PI_CAP_PROVIDES;

        s_desc.name             = "Bad Version Test Plugin";
        s_desc.vendor           = "piplugin";
        s_desc.version          = "1.0.0";
        s_desc.category         = "Test/Negative";
        s_desc.api_version      = BADVERSION_API_VERSION;
        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 1;
        s_initialized = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
