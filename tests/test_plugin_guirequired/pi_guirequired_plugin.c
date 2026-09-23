/*
 * piplugin - 声明 "需要 GUI 宿主" 的测试插件（roadmap ECO-08 负向用例）
 *
 * 它是一个**完全合法**的插件：工厂、class GUID、能力声明都正常，
 * 唯一的特点是 descriptor 里写了 `PI_IID_HOST_UI (REQUIRED)`。
 *
 * 于是 headless 宿主必须在**实例化之前**就拒绝它 —— 这正是能力协商存在的理由：
 * 插件不去猜宿主有没有 GUI，宿主也不用等插件崩了才发现它需要窗口。
 * ctest 用例 `capability_gate_rejects_gui_required_plugin` 断言宿主打印了
 * "requires capability ... does not provide it" 并以此轮次退出（退出码非 0 是期望结果）。
 *
 * 纯 C，不依赖 Qt 与任何适配器套件。
 */
#include "piplugin/pi_plugin.h"

/* 完整随机的 128 位 UUID 风格 class GUID（不是框架保留区里的小整数编号）。 */
static const PiGuid GUIREQUIRED_CLASS_GUID =
    PI_GUID(0x51C7E8A3, 0x2B64, 0x4F90, 0x8D, 0x35, 0x71, 0xC0, 0x46, 0x9E, 0x12, 0xBB);

typedef struct GuiRequiredFactory {
    PiRefCountedBase base;      /* 必须是第一个数据成员 */
} GuiRequiredFactory;

/* 工厂与 descriptor 的状态（静态存储 —— 语言保证清零，但下面仍显式
 * pi_descriptor_init()，把"可选字段=不存在"写成明确意图）。 */
static GuiRequiredFactory  s_factory;
static PiPluginCapability  s_caps[2];
static PiPluginProperty    s_props[1];
static PiPluginDescriptor  s_desc;
static int                 s_initialized = 0;

/* 本模块内的薄封装：直接在 C 里取框架导出的 add_ref/release 地址会触发
 * warning C4232（见 docs/design/interfaces.md 5.3）。 */
static uint32_t PI_CALL Factory_AddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL Factory_Release(void* self) { return pi_refcounted_release(self); }

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

static uint32_t PI_CALL Factory_GetClassCount(void* self_ptr) { (void)self_ptr; return 1; }

static PiResult PI_CALL Factory_GetClassGuid(void* self_ptr, uint32_t index, PiGuid* guid)
{
    (void)self_ptr;
    if (index != 0 || !guid) return PI_E_INVALIDARG;
    *guid = GUIREQUIRED_CLASS_GUID;
    return PI_OK;
}

static PiResult PI_CALL Factory_CreateInstance(void* self_ptr, const PiGuid* guid,
                                               IPiHostServices* host, IPiPluginBase** out)
{
    /* 永远不该被调用：任何宿主都必须在 create_instance 之前，因为 HOST_UI 是
     * REQUIRED 而它给不出，就拒绝这个插件。若真被调用到，说明门禁失效了 ——
     * 返回 NOTIMPL 让用例更明显地失败。 */
    (void)self_ptr; (void)guid; (void)host;
    if (out) *out = NULL;
    return PI_E_NOTIMPL;
}

static const IPiPluginFactoryVtbl s_factory_vtbl = {
    { &Factory_Qi, &Factory_AddRef, &Factory_Release },
    &Factory_GetDescriptor,
    &Factory_GetClassCount,
    &Factory_GetClassGuid,
    &Factory_CreateInstance
};

PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory) return PI_E_INVALIDARG;
    *out_factory = NULL;

    if (!s_initialized) {
        pi_refcounted_init(&s_factory.base, (const IPiUnknownVtbl*)&s_factory_vtbl);

        /* 关键的一行：REQUIRED 而不是 OPTIONAL。headless 宿主给不出 IPiHostUI，
         * 于是双向门禁的第一向就会拒绝（方向二 = 宿主生态要求插件 PROVIDES 什么）。 */
        s_caps[0].iid   = PI_IID_PLUGIN_VIEW;
        s_caps[0].flags = PI_CAP_PROVIDES;
        s_caps[1].iid   = PI_IID_HOST_UI;
        s_caps[1].flags = PI_CAP_REQUIRED;

        pi_descriptor_init(&s_desc);
        s_desc.name             = "GUI Required Test Plugin";
        s_desc.vendor           = "piplugin";
        s_desc.version          = "1.0.0";
        s_desc.category         = "Test/Negative";
        s_desc.api_version      = PI_PLUGIN_API_VERSION;
        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 2;

        s_props[0].key   = "com.example.kind";
        s_props[0].value = "guirequired-plugin";
        s_desc.properties     = s_props;
        s_desc.property_count = 1;

        s_initialized = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
