/*
 * pipluginframework - 单元测试（裸 C，无第三方框架）
 *
 * 覆盖 roadmap BLK-06 列出的核心回归：
 *   pi_guid_equal / descriptor 帮助函数 / PiRefCountedBase 引用计数与 destroy
 *   回调 / pi_module_load 失败路径 / 默认宿主服务的 headless 与 GUI 两形态。
 *
 * 由 ctest 注册为 `unit`：`ctest -C Debug`（或 --preset）一条命令跑完。
 * 断言失败不中断，全部跑完后按失败数决定退出码（0 = 全过）。
 */
#include "pipluginframework/pi_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * 极简断言框架
 * -------------------------------------------------------------------------- */
static unsigned g_checks   = 0;
static unsigned g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                  \
    do {                                                                \
        long long _a = (long long)(actual);                             \
        long long _e = (long long)(expected);                           \
        ++g_checks;                                                     \
        if (_a != _e) {                                                 \
            ++g_failures;                                               \
            printf("  FAIL %s:%d: %s == %lld, expected %lld\n",         \
                   __FILE__, __LINE__, #actual, _a, _e);                \
        }                                                               \
    } while (0)

static void Section(const char* name)
{
    printf("- %s\n", name);
}

/* --------------------------------------------------------------------------
 * pi_guid_equal
 * -------------------------------------------------------------------------- */
static const PiGuid GUID_A  = PI_GUID(0x11223344, 0x5566, 0x7788,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00);
static const PiGuid GUID_A2 = PI_GUID(0x11223344, 0x5566, 0x7788,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00);
/* 只有 data1 不同 */
static const PiGuid GUID_B  = PI_GUID(0x11223345, 0x5566, 0x7788,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00);
/* 前三个字段相同，只有 data4 不同 */
static const PiGuid GUID_C  = PI_GUID(0x11223344, 0x5566, 0x7788,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01);
/* 只有 data2/data3 不同 */
static const PiGuid GUID_D  = PI_GUID(0x11223344, 0x5567, 0x7789,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00);
/* app 侧接口 GUID（随机风格，见 docs/design/interfaces.md 5.1） */
static const PiGuid GUID_APP = PI_GUID(0x9F3C1D42, 0x7B08, 0x4E55,
                                       0xA1, 0x6C, 0x0D, 0xF2, 0x88, 0x37, 0x51, 0xBE);

static void TestGuidEqual(void)
{
    Section("pi_guid_equal");
    CHECK_EQ_INT(pi_guid_equal(&GUID_A, &GUID_A2), 1);
    CHECK_EQ_INT(pi_guid_equal(&GUID_A, &GUID_A), 1);
    CHECK_EQ_INT(pi_guid_equal(&GUID_A, &GUID_B), 0);   /* data1 */
    CHECK_EQ_INT(pi_guid_equal(&GUID_A, &GUID_C), 0);   /* data4 */
    CHECK_EQ_INT(pi_guid_equal(&GUID_A, &GUID_D), 0);   /* data2/data3 */
    CHECK_EQ_INT(pi_guid_equal(&GUID_A, &GUID_APP), 0);
    CHECK_EQ_INT(pi_guid_equal(NULL, &GUID_A), 0);
    CHECK_EQ_INT(pi_guid_equal(&GUID_A, NULL), 0);
    CHECK_EQ_INT(pi_guid_equal(NULL, NULL), 0);

    /* 框架内建 IID 互不相等（防撞的最低要求） */
    CHECK_EQ_INT(pi_guid_equal(&PI_IID_UNKNOWN, &PI_IID_UNKNOWN), 1);
    CHECK_EQ_INT(pi_guid_equal(&PI_IID_PLUGIN_VIEW, &PI_IID_HOST_UI), 0);
    CHECK_EQ_INT(pi_guid_equal(&PI_IID_HOST_UI, &PI_IID_SERVICE), 0);
    CHECK_EQ_INT(pi_guid_equal(&PI_IID_PLUGIN_FACTORY, &PI_IID_PLUGIN_BASE), 0);
}

/* --------------------------------------------------------------------------
 * descriptor 帮助函数
 * -------------------------------------------------------------------------- */
static void TestDescriptorHelpers(void)
{
    PiPluginCapability caps[3];
    PiPluginDescriptor desc;

    Section("descriptor 帮助函数");

    memset(caps, 0, sizeof(caps));
    memset(&desc, 0, sizeof(desc));
    caps[0].iid = PI_IID_PLUGIN_VIEW; caps[0].flags = PI_CAP_PROVIDES;
    caps[1].iid = PI_IID_HOST_UI;     caps[1].flags = PI_CAP_OPTIONAL;
    caps[2].iid = GUID_APP;           caps[2].flags = PI_CAP_REQUIRED;
    desc.capabilities     = caps;
    desc.capability_count = 3;

    CHECK(pi_descriptor_find_capability(&desc, &PI_IID_PLUGIN_VIEW) == &caps[0]);
    CHECK(pi_descriptor_find_capability(&desc, &PI_IID_HOST_UI) == &caps[1]);
    CHECK(pi_descriptor_find_capability(&desc, &GUID_APP) == &caps[2]);
    CHECK(pi_descriptor_find_capability(&desc, &PI_IID_SERVICE) == NULL);

    /* 注意：provides/requires 返回的是"标志位与非零"，不是布尔 1 */
    CHECK(pi_descriptor_provides(&desc, &PI_IID_PLUGIN_VIEW) != 0);
    CHECK(pi_descriptor_provides(&desc, &PI_IID_HOST_UI) == 0);      /* OPTIONAL 不是 PROVIDES */
    CHECK(pi_descriptor_provides(&desc, &GUID_APP) == 0);            /* REQUIRED 不是 PROVIDES */
    CHECK(pi_descriptor_requires(&desc, &GUID_APP) != 0);
    CHECK(pi_descriptor_requires(&desc, &PI_IID_PLUGIN_VIEW) == 0);
    CHECK(pi_descriptor_requires(&desc, &PI_IID_HOST_UI) == 0);      /* OPTIONAL 不是 REQUIRED */

    /* 同时带 PROVIDES|REQUIRED 的能力：两个查询都要命中 */
    caps[1].flags = PI_CAP_PROVIDES | PI_CAP_REQUIRED;
    CHECK(pi_descriptor_provides(&desc, &PI_IID_HOST_UI) != 0);
    CHECK(pi_descriptor_requires(&desc, &PI_IID_HOST_UI) != 0);

    /* 容错：NULL 描述符 / NULL iid / 空能力表 */
    CHECK(pi_descriptor_find_capability(NULL, &PI_IID_PLUGIN_VIEW) == NULL);
    CHECK(pi_descriptor_find_capability(&desc, NULL) == NULL);
    CHECK(pi_descriptor_provides(NULL, &PI_IID_PLUGIN_VIEW) == 0);
    CHECK(pi_descriptor_requires(NULL, &PI_IID_PLUGIN_VIEW) == 0);

    {
        PiPluginDescriptor empty;
        memset(&empty, 0, sizeof(empty));
        CHECK(pi_descriptor_find_capability(&empty, &PI_IID_PLUGIN_VIEW) == NULL);
        CHECK(pi_descriptor_provides(&empty, &PI_IID_PLUGIN_VIEW) == 0);
        CHECK(pi_descriptor_requires(&empty, &PI_IID_PLUGIN_VIEW) == 0);
    }
}

/* --------------------------------------------------------------------------
 * PiRefCountedBase：引用计数 + destroy 回调
 * -------------------------------------------------------------------------- */
static int g_destroy_calls = 0;

/* 本模块内的一层薄封装。直接把框架导出的 pi_refcounted_add_ref/release 的地址
 * 填进 vtbl，在 C 里会触发 warning C4232（取 dllimport 函数地址不保证跨模块标识，
 * 见 docs/design/interfaces.md 5.3）。测试要在 /W4 下零警告，故用薄封装。 */
static uint32_t PI_CALL TestAddRef(void* self_ptr) { return pi_refcounted_add_ref(self_ptr); }
static uint32_t PI_CALL TestRelease(void* self_ptr) { return pi_refcounted_release(self_ptr); }

static const IPiUnknownVtbl s_test_vtbl = {
    NULL,                        /* pi_query_interface: 本用例不测 QI */
    &TestAddRef,
    &TestRelease
};

static void TestDestroy(void* self_ptr)
{
    ++g_destroy_calls;
    free(self_ptr);
}

static void TestRefCounted(void)
{
    Section("PiRefCountedBase 引用计数与 destroy 回调");

    /* 1) 带 destroy：refcount 归零时恰好调用一次 */
    {
        PiRefCountedBase* obj = (PiRefCountedBase*)calloc(1, sizeof(PiRefCountedBase));
        CHECK(obj != NULL);
        g_destroy_calls = 0;
        pi_refcounted_init_with_destroy(obj, &s_test_vtbl, &TestDestroy);

        CHECK_EQ_INT(obj->ref_count, 1);              /* init 后为 1 */
        CHECK_EQ_INT(pi_refcounted_add_ref(obj), 2);  /* 返回新计数 */
        CHECK_EQ_INT(pi_refcounted_add_ref(obj), 3);
        CHECK_EQ_INT(pi_refcounted_release(obj), 2);
        CHECK_EQ_INT(g_destroy_calls, 0);             /* 还没归零 */
        CHECK_EQ_INT(pi_refcounted_release(obj), 1);
        CHECK_EQ_INT(g_destroy_calls, 0);
        CHECK_EQ_INT(pi_refcounted_release(obj), 0);
        CHECK_EQ_INT(g_destroy_calls, 1);             /* 归零销毁一次 */
    }

    /* 2) 无 destroy：归零后对象依然存在（静态/自管生命周期对象靠这个） */
    {
        PiRefCountedBase obj;
        memset(&obj, 0, sizeof(obj));
        pi_refcounted_init(&obj, &s_test_vtbl);
        CHECK_EQ_INT(obj.destroy == NULL, 1);
        CHECK_EQ_INT(pi_refcounted_add_ref(&obj), 2);
        CHECK_EQ_INT(pi_refcounted_release(&obj), 1);
        CHECK_EQ_INT(pi_refcounted_release(&obj), 0);
        CHECK_EQ_INT(obj.ref_count, 0);               /* 未释放，但计数归零 */
    }

    /* 3) NULL 安全 */
    CHECK_EQ_INT(pi_refcounted_add_ref(NULL), 0);
    CHECK_EQ_INT(pi_refcounted_release(NULL), 0);

    /* 4) 通过 vtable 槽位调用（框架与插件的实际用法） */
    {
        PiRefCountedBase* obj = (PiRefCountedBase*)calloc(1, sizeof(PiRefCountedBase));
        CHECK(obj != NULL);
        g_destroy_calls = 0;
        pi_refcounted_init_with_destroy(obj, &s_test_vtbl, &TestDestroy);
        CHECK_EQ_INT(obj->unk.lpVtbl->pi_add_ref(obj), 2);
        CHECK_EQ_INT(obj->unk.lpVtbl->pi_release(obj), 1);
        CHECK_EQ_INT(obj->unk.lpVtbl->pi_release(obj), 0);
        CHECK_EQ_INT(g_destroy_calls, 1);
    }
}

/* --------------------------------------------------------------------------
 * pi_module_load 失败路径
 * -------------------------------------------------------------------------- */
static const char* g_argv0 = NULL;

static void TestModuleLoadFailure(void)
{
    Section("pi_module_load 失败路径");

    /* NULL 路径 */
    CHECK(pi_module_load(NULL) == NULL);
    CHECK(strcmp(pi_module_get_load_error(), "no error") != 0);
    CHECK(strlen(pi_module_get_load_error()) > 0);

    /* 不存在的路径 */
    CHECK(pi_module_load("Z:\\no\\such\\directory\\pi_missing_plugin.dll") == NULL);
    CHECK(strlen(pi_module_get_load_error()) > 0);
    CHECK(strstr(pi_module_get_load_error(), "pi_missing_plugin.dll") != NULL);

    /* 真实存在但不导出 pi_plugin_entry 的模块：用测试程序自身 */
    if (g_argv0 && g_argv0[0]) {
        CHECK(pi_module_load(g_argv0) == NULL);
        CHECK(strstr(pi_module_get_load_error(), PI_PLUGIN_ENTRY_NAME) != NULL);
    }

    /* 卸载 NULL 是安全的（卸载序列会走到这里） */
    pi_module_unload(NULL);
    CHECK_EQ_INT(pi_module_get_factory(NULL, NULL), PI_E_INVALIDARG);
}

/* --------------------------------------------------------------------------
 * 默认宿主服务：headless 与 GUI 两形态
 * -------------------------------------------------------------------------- */
static int      g_posted_msgs = 0;
static uint32_t g_last_msg    = 0;

static void OnInitPostMessage(void* user_data, uint32_t msg,
                              uintptr_t wparam, intptr_t lparam)
{
    (void)user_data; (void)wparam; (void)lparam;
    ++g_posted_msgs;
    g_last_msg = msg;
}

static void TestHostServices(void)
{
    IPiHostServices* headless = NULL;
    IPiHostServices* gui = NULL;
    void* out = NULL;

    Section("默认宿主服务（headless / GUI 两形态）");

    /* --- headless：传 PI_INVALID_WINDOW，不暴露 IPiHostUI --- */
    CHECK_EQ_INT(pi_host_services_create_default(&OnInitPostMessage, NULL,
                                                 PI_INVALID_WINDOW, &headless), PI_OK);
    CHECK(headless != NULL);

    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_IID_HOST_UI, &out),
                 PI_E_NOINTERFACE);
    CHECK(out == NULL);

    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_IID_HOST_SERVICES, &out),
                 PI_OK);
    CHECK(out != NULL);
    if (out) pi_iunknown_release((IPiUnknown*)out);

    /* 消息回调（插件 → 宿主） */
    g_posted_msgs = 0;
    g_last_msg    = 0;
    pi_host_post_message(headless, 0x8001u, 7u, 0);
    CHECK_EQ_INT(g_posted_msgs, 1);
    CHECK_EQ_INT(g_last_msg, 0x8001);

    /* 宿主分配器 */
    {
        void* mem = pi_host_alloc(headless, 64);
        CHECK(mem != NULL);
        if (mem) memset(mem, 0xAB, 64);
        pi_host_free(headless, mem);
        pi_host_free(headless, NULL);   /* NULL 安全 */
    }

    /* --- 运行时切换到 GUI 形态：给了窗口就暴露 IPiHostUI --- */
    pi_host_default_set_ui_window(headless, (PiNativeWindow)(uintptr_t)0x1234);
    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_IID_HOST_UI, &out), PI_OK);
    CHECK(out != NULL);
    if (out) {
        IPiHostUI* ui = (IPiHostUI*)out;
        CHECK_EQ_INT((uintptr_t)pi_host_ui_get_parent_window(ui), (uintptr_t)0x1234);
        CHECK(pi_host_ui_thread_id(ui) != 0);

        /* 活值语义：包装持有 owner 的引用，改窗口后已发出的 UI 指针立刻反映新值
         * （曾经这里是越界读、返回垃圾 —— 见 src/pi_plugin_host.c 的说明） */
        pi_host_default_set_ui_window(headless, (PiNativeWindow)(uintptr_t)0x9ABC);
        CHECK_EQ_INT((uintptr_t)pi_host_ui_get_parent_window(ui), (uintptr_t)0x9ABC);
        pi_host_default_set_ui_window(headless, (PiNativeWindow)(uintptr_t)0x1234);

        /* IPiHostUI 自身 QI */
        {
            void* ui_out = NULL;
            CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)ui, &PI_IID_HOST_UI, &ui_out),
                         PI_OK);
            if (ui_out) {
                /* 注意：本实现每次都新建一个包装对象（不是 COM 标识规则意义上的
                 * 同一指针）—— 这里只断言两个包装读到的宿主状态一致。
                 * "QI 是否必须返回同一指针"留待 BLK-08 接口终审定性。 */
                CHECK_EQ_INT((uintptr_t)pi_host_ui_get_parent_window((IPiHostUI*)ui_out),
                             (uintptr_t)0x1234);
                pi_iunknown_release((IPiUnknown*)ui_out);
            }
        }
        pi_iunknown_release((IPiUnknown*)out);
    }

    /* --- 再切回 headless --- */
    pi_host_default_set_ui_window(headless, PI_INVALID_WINDOW);
    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_IID_HOST_UI, &out),
                 PI_E_NOINTERFACE);

    /* --- 直接以 GUI 形态创建 --- */
    CHECK_EQ_INT(pi_host_services_create_default(NULL, NULL,
                                                 (PiNativeWindow)(uintptr_t)0x5678, &gui), PI_OK);
    CHECK(gui != NULL);
    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)gui, &PI_IID_HOST_UI, &out), PI_OK);
    if (out) pi_iunknown_release((IPiUnknown*)out);
    if (gui) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)gui), 0);

    /* --- 参数校验与"非本实现对象"防御 --- */
    CHECK_EQ_INT(pi_host_services_create_default(NULL, NULL, PI_INVALID_WINDOW, NULL),
                 PI_E_INVALIDARG);
    pi_host_default_set_ui_window(NULL, PI_INVALID_WINDOW);   /* NULL 安全 */

    if (headless) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)headless), 0);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char** argv)
{
    g_argv0 = (argc > 0) ? argv[0] : NULL;

    printf("== pipluginframework unit tests ==\n");
    TestGuidEqual();
    TestDescriptorHelpers();
    TestRefCounted();
    TestModuleLoadFailure();
    TestHostServices();

    printf("== checks=%u failures=%u ==\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
