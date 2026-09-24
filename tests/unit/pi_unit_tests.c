/*
 * piplugin - 单元测试（裸 C，无第三方框架）
 *
 * 覆盖 roadmap BLK-06 列出的核心回归：
 *   pi_guid_equal / descriptor 帮助函数 / PiRefCountedBase 引用计数与 destroy
 *   回调 / pi_plugin_module_load 失败路径 / 默认宿主服务的 headless 与 GUI 两形态。
 *
 * 由 ctest 注册为 `unit`：`ctest -C Debug`（或 --preset）一条命令跑完。
 * 断言失败不中断，全部跑完后按失败数决定退出码（0 = 全过）。
 */
#include "piplugin/pi_plugin.h"
#include "pi_test_host_service_impl.h"
#include "pi_test_thread.h"   /* W-01/W-04 的并发用例：起线程 / 让出时间片 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>   /* GetCurrentThreadId：线程 id 契约的精确断言用 */
#endif

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
    CHECK_EQ_INT(pi_guid_equal(&PI_PLUGIN_IID_PLUGIN_VIEW, &PI_PLUGIN_IID_HOST_UI), 0);
    CHECK_EQ_INT(pi_guid_equal(&PI_PLUGIN_IID_HOST_UI, &PI_PLUGIN_IID_SERVICE), 0);
    CHECK_EQ_INT(pi_guid_equal(&PI_PLUGIN_IID_PLUGIN_FACTORY, &PI_PLUGIN_IID_PLUGIN_BASE), 0);
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
    caps[0].iid = PI_PLUGIN_IID_PLUGIN_VIEW; caps[0].flags = PI_PLUGIN_CAP_PROVIDES;
    caps[1].iid = PI_PLUGIN_IID_HOST_UI;     caps[1].flags = PI_PLUGIN_CAP_OPTIONAL;
    caps[2].iid = GUID_APP;           caps[2].flags = PI_PLUGIN_CAP_REQUIRED;
    desc.capabilities     = caps;
    desc.capability_count = 3;

    CHECK(pi_plugin_descriptor_find_capability(&desc, &PI_PLUGIN_IID_PLUGIN_VIEW) == &caps[0]);
    CHECK(pi_plugin_descriptor_find_capability(&desc, &PI_PLUGIN_IID_HOST_UI) == &caps[1]);
    CHECK(pi_plugin_descriptor_find_capability(&desc, &GUID_APP) == &caps[2]);
    CHECK(pi_plugin_descriptor_find_capability(&desc, &PI_PLUGIN_IID_SERVICE) == NULL);

    /* 注意：provides/requires 返回的是"标志位与非零"，不是布尔 1 */
    CHECK(pi_plugin_descriptor_provides(&desc, &PI_PLUGIN_IID_PLUGIN_VIEW) != 0);
    CHECK(pi_plugin_descriptor_provides(&desc, &PI_PLUGIN_IID_HOST_UI) == 0);      /* OPTIONAL 不是 PROVIDES */
    CHECK(pi_plugin_descriptor_provides(&desc, &GUID_APP) == 0);            /* REQUIRED 不是 PROVIDES */
    CHECK(pi_plugin_descriptor_requires(&desc, &GUID_APP) != 0);
    CHECK(pi_plugin_descriptor_requires(&desc, &PI_PLUGIN_IID_PLUGIN_VIEW) == 0);
    CHECK(pi_plugin_descriptor_requires(&desc, &PI_PLUGIN_IID_HOST_UI) == 0);      /* OPTIONAL 不是 REQUIRED */

    /* 同时带 PROVIDES|REQUIRED 的能力：两个查询都要命中 */
    caps[1].flags = PI_PLUGIN_CAP_PROVIDES | PI_PLUGIN_CAP_REQUIRED;
    CHECK(pi_plugin_descriptor_provides(&desc, &PI_PLUGIN_IID_HOST_UI) != 0);
    CHECK(pi_plugin_descriptor_requires(&desc, &PI_PLUGIN_IID_HOST_UI) != 0);

    /* 容错：NULL 描述符 / NULL iid / 空能力表 */
    CHECK(pi_plugin_descriptor_find_capability(NULL, &PI_PLUGIN_IID_PLUGIN_VIEW) == NULL);
    CHECK(pi_plugin_descriptor_find_capability(&desc, NULL) == NULL);
    CHECK(pi_plugin_descriptor_provides(NULL, &PI_PLUGIN_IID_PLUGIN_VIEW) == 0);
    CHECK(pi_plugin_descriptor_requires(NULL, &PI_PLUGIN_IID_PLUGIN_VIEW) == 0);

    {
        PiPluginDescriptor empty;
        memset(&empty, 0, sizeof(empty));
        CHECK(pi_plugin_descriptor_find_capability(&empty, &PI_PLUGIN_IID_PLUGIN_VIEW) == NULL);
        CHECK(pi_plugin_descriptor_provides(&empty, &PI_PLUGIN_IID_PLUGIN_VIEW) == 0);
        CHECK(pi_plugin_descriptor_requires(&empty, &PI_PLUGIN_IID_PLUGIN_VIEW) == 0);
    }
}

/* --------------------------------------------------------------------------
 * APP-04：descriptor 自由元数据（properties）
 * -------------------------------------------------------------------------- */
static void TestDescriptorProperties(void)
{
    PiPluginProperty   props[3];
    PiPluginDescriptor desc;

    Section("APP-04 descriptor properties");

    memset(&desc, 0, sizeof(desc));
    memset(props, 0, sizeof(props));
    props[0].key = "com.example.kind";   props[0].value = "qt-plugin";
    props[1].key = "com.example.blank";  props[1].value = "";          /* 空值合法 */
    props[2].key = "UTF8.\xE9\x94\xAE";  props[2].value = "\xE5\x80\xBC"; /* UTF-8 键与值 */

    desc.api_version = PI_PLUGIN_API_VERSION;

    /* 0.2 时代的 descriptor（memset 出来的：没有 properties）要安全返回 NULL ——
     * 这也正是"追加密钥对是**追加**字段"的意义：老代码零初始化即为"没有元数据"。 */
    CHECK(pi_plugin_descriptor_find_property(&desc, "com.example.kind") == NULL);

    desc.properties = props;
    desc.property_count = 3;

    CHECK(strcmp(pi_plugin_descriptor_find_property(&desc, "com.example.kind"), "qt-plugin") == 0);
    CHECK(strcmp(pi_plugin_descriptor_find_property(&desc, "com.example.blank"), "") == 0);
    CHECK(strcmp(pi_plugin_descriptor_find_property(&desc, "UTF8.\xE9\x94\xAE"), "\xE5\x80\xBC") == 0);

    /* 未声明的键 */
    CHECK(pi_plugin_descriptor_find_property(&desc, "com.example.missing") == NULL);
    /* 大小写敏感、且是整键比较（不是前缀匹配） */
    CHECK(pi_plugin_descriptor_find_property(&desc, "COM.EXAMPLE.KIND") == NULL);
    CHECK(pi_plugin_descriptor_find_property(&desc, "com.example") == NULL);
    CHECK(pi_plugin_descriptor_find_property(&desc, "com.example.kinds") == NULL);

    /* 布局判定（APP-04 的关键正确性点）：api_version < 0.3 的插件是按**更短的**
     * struct 编译的，它的 properties 字段根本不存在 —— 即使这里放了个像样的值，
     * 也必须报"没有这条属性"，而不是按新布局去读越界内存。
     * （版本门禁接受更老的插件，所以这件事只能在这里判。） */
    {
        PiPluginDescriptor old_layout = desc;
        old_layout.api_version = PI_PLUGIN_API_VERSION_MAKE(0, 2);
        CHECK(pi_plugin_descriptor_find_property(&old_layout, "com.example.kind") == NULL);
        old_layout.api_version = 0;              /* 完全没声明版本 */
        CHECK(pi_plugin_descriptor_find_property(&old_layout, "com.example.kind") == NULL);
    }

    /* 容错 */
    CHECK(pi_plugin_descriptor_find_property(NULL, "com.example.kind") == NULL);
    CHECK(pi_plugin_descriptor_find_property(&desc, NULL) == NULL);
    desc.property_count = 0;
    CHECK(pi_plugin_descriptor_find_property(&desc, "com.example.kind") == NULL);
    desc.property_count = 3;

    /* 条目 key 为 NULL：跳过该条目而不是崩溃 */
    {
        PiPluginProperty   rows[2];
        PiPluginDescriptor d;
        memset(&d, 0, sizeof(d));
        memset(rows, 0, sizeof(rows));
        rows[0].key = NULL;      rows[0].value = "ignored";
        rows[1].key = "com.x.k"; rows[1].value = "v";
        d.api_version = PI_PLUGIN_API_VERSION;
        d.properties = rows;
        d.property_count = 2;
        CHECK(pi_plugin_descriptor_find_property(&d, "com.x.k") != NULL);
        CHECK(strcmp(pi_plugin_descriptor_find_property(&d, "com.x.k"), "v") == 0);
    }

    /* 重复 key：第一个胜出（头文件写明） */
    {
        PiPluginProperty   rows[2];
        PiPluginDescriptor d;
        memset(&d, 0, sizeof(d));
        memset(rows, 0, sizeof(rows));
        rows[0].key = "k"; rows[0].value = "first";
        rows[1].key = "k"; rows[1].value = "second";
        d.api_version = PI_PLUGIN_API_VERSION;
        d.properties = rows;
        d.property_count = 2;
        CHECK(strcmp(pi_plugin_descriptor_find_property(&d, "k"), "first") == 0);
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
 * pi_plugin_module_load 失败路径
 * -------------------------------------------------------------------------- */
static const char* g_argv0 = NULL;
/* ctest 传进来的真实插件名（argv[1]）：加载的成功/失败两条路径都要用它 */
static const char* g_plugin_path = NULL;

static void TestModuleLoadFailure(void)
{
    Section("pi_plugin_module_load 失败路径");

    /* NULL 路径 */
    CHECK(pi_plugin_module_load(NULL) == NULL);
    CHECK(strcmp(pi_plugin_module_get_load_error(), "no error") != 0);
    CHECK(strlen(pi_plugin_module_get_load_error()) > 0);

    /* 不存在的路径 */
    CHECK(pi_plugin_module_load("Z:\\no\\such\\directory\\pi_missing_plugin.dll") == NULL);
    CHECK(strlen(pi_plugin_module_get_load_error()) > 0);
    CHECK(strstr(pi_plugin_module_get_load_error(), "pi_missing_plugin.dll") != NULL);

    /* 真实存在但不导出 pi_plugin_entry 的模块：用测试程序自身 */
    if (g_argv0 && g_argv0[0]) {
        CHECK(pi_plugin_module_load(g_argv0) == NULL);
        CHECK(strstr(pi_plugin_module_get_load_error(), PI_PLUGIN_ENTRY_NAME) != NULL);
    }

    /* 卸载 NULL 是安全的（卸载序列会走到这里） */
    pi_plugin_module_unload(NULL);
    CHECK_EQ_INT(pi_plugin_module_get_factory(NULL, NULL), PI_E_INVALIDARG);
}

/* --------------------------------------------------------------------------
 * W-01：加载错误串的线程隔离 + _r 变体
 *
 * 旧实现是一个进程级 static buffer：并发加载失败时各线程读到的都是"最后写
 * 入的那一条"。下面让 4 个线程各自反复加载**自己独有的**不存在路径，并在
 * 读写之间让出时间片，然后断言每个线程读回的串始终含自己的标记。
 * （进程级实现下这个断言会大量失败 —— 这正是它要抓的回归。）
 * -------------------------------------------------------------------------- */
#define PI_PLUGIN_LOAD_ERROR_RACE_THREADS 4
#define PI_PLUGIN_LOAD_ERROR_RACE_ROUNDS  32

/* 与 src/pi_plugin_host.c 的 PI_PLUGIN_LOAD_ERROR_MAX 一致（那是内部宏，测试只借用
 * 它表示"错误串最长就这么长"）。 */
#define PI_PLUGIN_LOAD_ERROR_MAX          256

typedef struct LoadErrorRaceWorker {
    int  index;
    int  rounds;     /* 真正跑完的轮数 */
    int  foreign;    /* 读到的错误串不含自己标记的次数（>0 = 被别的线程覆盖） */
    int  stale;      /* 让出时间片后再读，串变了或丢了标记的次数 */
    int  copy_bad;   /* _r 拷贝与旧 API 当次读到的不一致 */
    int  bad_load;   /* 本该失败却成功的加载（测试前提被破坏） */
} LoadErrorRaceWorker;

static volatile int g_load_error_go = 0;

static void LoadErrorRaceThread(void* user_data)
{
    LoadErrorRaceWorker* w = (LoadErrorRaceWorker*)user_data;
    int round;

    while (!g_load_error_go) PiPluginTestThreadYield();   /* 一起出发 */

    for (round = 0; round < PI_PLUGIN_LOAD_ERROR_RACE_ROUNDS; ++round) {
        char path[128];
        char copy[PI_PLUGIN_LOAD_ERROR_MAX + 32];
        const char* msg;

        /* 每个线程、每一轮都用不同的路径：错误串里只应出现自己的那一条 */
        snprintf(path, sizeof(path),
                 "Z:\\no\\such\\dir\\pi_plugin_race_w%d_r%d.dll", w->index, round);

        if (pi_plugin_module_load(path) != NULL) { ++w->bad_load; continue; }
        ++w->rounds;

        /* 把"写入 → 读取"的窗口拉开 1ms：进程级 buffer 下这 1ms 里足够别的
         * 线程各自写一遍，于是本线程必然读到别人的串。只让出时间片不够 ——
         * 加载路径本身带 loader 锁，天然串行，光靠 Sleep(0) 撞不上。 */
        PiPluginTestThreadSleepMs(1);

        msg = pi_plugin_module_get_load_error();
        if (!msg || strstr(msg, path) == NULL) ++w->foreign;

        /* _r 是"当次"的拷贝：必须与刚才那条一致，且自带 NUL */
        if (pi_plugin_module_get_load_error_r(copy, sizeof(copy)) != PI_OK ||
            msg == NULL || strcmp(copy, msg) != 0)
            ++w->copy_bad;

        /* 再让一轮，本线程的串必须还在（TLS 的全部意义） */
        PiPluginTestThreadSleepMs(1);
        msg = pi_plugin_module_get_load_error();
        if (!msg || strstr(msg, path) == NULL) ++w->stale;
    }
}

static void TestLoadErrorThreadSafety(void)
{
    LoadErrorRaceWorker workers[PI_PLUGIN_LOAD_ERROR_RACE_THREADS];
    PiPluginTestThread        threads[PI_PLUGIN_LOAD_ERROR_RACE_THREADS];
    char                buf[PI_PLUGIN_LOAD_ERROR_MAX + 32];
    int                 i;

    Section("W-01 加载错误串线程隔离 + pi_plugin_module_get_load_error_r");

    /* --- 1) 单线程：旧 API 语义不变（"null path"，与上面那条用例同源） --- */
    CHECK(pi_plugin_module_load(NULL) == NULL);
    CHECK(strcmp(pi_plugin_module_get_load_error(), "null path") == 0);

    /* _r 拷贝的是"当次"内容，且与旧 API 一致 */
    memset(buf, 0x7F, sizeof(buf));
    CHECK_EQ_INT(pi_plugin_module_get_load_error_r(buf, sizeof(buf)), PI_OK);
    CHECK(strcmp(buf, "null path") == 0);

    /* 参数非法：返回错误码，不崩 */
    CHECK_EQ_INT(pi_plugin_module_get_load_error_r(NULL, sizeof(buf)), PI_E_INVALIDARG);
    CHECK_EQ_INT(pi_plugin_module_get_load_error_r(buf, 0), PI_E_INVALIDARG);
    CHECK_EQ_INT(pi_plugin_module_get_load_error_r(NULL, 0), PI_E_INVALIDARG);

    /* 缓冲太小：截断且仍然 NUL 结尾（内容非空） */
    memset(buf, 0x7F, sizeof(buf));
    CHECK_EQ_INT(pi_plugin_module_get_load_error_r(buf, 5), PI_OK);
    CHECK_EQ_INT((unsigned char)buf[4], 0);
    CHECK(strncmp(buf, "null", 4) == 0);

    /* --- 2) 成功加载后回到 "no error"（拿真实插件验，没有就跳过） --- */
    if (g_plugin_path && g_plugin_path[0]) {
        PiPluginModule* module = pi_plugin_module_load(g_plugin_path);
        CHECK(module != NULL);
        if (module) {
            CHECK(strcmp(pi_plugin_module_get_load_error(), "no error") == 0);
            CHECK_EQ_INT(pi_plugin_module_get_load_error_r(buf, sizeof(buf)), PI_OK);
            CHECK(strcmp(buf, "no error") == 0);
            pi_plugin_module_unload(module);
        }
    }

    /* --- 3) 并发：4 个线程各读各的，谁都不能读到别人的失败原因 --- */
    g_load_error_go = 0;
    memset(workers, 0, sizeof(workers));
    memset(threads, 0, sizeof(threads));

    for (i = 0; i < PI_PLUGIN_LOAD_ERROR_RACE_THREADS; ++i) {
        workers[i].index = i;
        CHECK_EQ_INT(PiPluginTestThreadStart(&threads[i], &LoadErrorRaceThread, &workers[i]), 0);
    }
    PiPluginTestThreadYield();
    g_load_error_go = 1;                 /* 放行：4 个线程开始互相踩 */

    for (i = 0; i < PI_PLUGIN_LOAD_ERROR_RACE_THREADS; ++i) {
        PiPluginTestThreadJoin(&threads[i]);
        printf("  thread[%d]: rounds=%d foreign=%d stale=%d copy_bad=%d\n",
               i, workers[i].rounds, workers[i].foreign,
               workers[i].stale, workers[i].copy_bad);
        CHECK_EQ_INT(workers[i].bad_load, 0);
        CHECK_EQ_INT(workers[i].rounds, PI_PLUGIN_LOAD_ERROR_RACE_ROUNDS);
        CHECK_EQ_INT(workers[i].foreign, 0);    /* 读到的永远是自己的 */
        CHECK_EQ_INT(workers[i].stale, 0);      /* 让出后还是自己的 */
        CHECK_EQ_INT(workers[i].copy_bad, 0);   /* _r 与旧 API 同步 */
    }

    /* 主线程自己的槽位也没被那 4 个线程碰过：TLS 是"每线程一份"，不是"最后
     * 一条" —— 进程级实现下这句会失败（主线程读到某个 pi_plugin_race_wN 路径）。 */
    CHECK(strstr(pi_plugin_module_get_load_error(), "pi_plugin_race_w") == NULL);
}

/* --------------------------------------------------------------------------
 * pi_plugin_api_version_compatible（BLK-03 的边界覆盖）
 * -------------------------------------------------------------------------- */
static void TestApiVersion(void)
{
    Section("pi_plugin_api_version_compatible");

    /* 编码：高 16 位 major，低 16 位 minor */
    /* 当前 API 版本的 tripwire：改版本号时这里会失败，提醒同步
     * CHANGELOG.md 与 docs/design/interfaces.md 1.5 的 policy 说明。
     * 0.3 = APP-04（descriptor 追加 properties），0.4 = APP-06（新增事件接口），
     * 0.5 = 本库标识符改前缀 + 家族根词汇移到基础层 pibase。 */
    CHECK_EQ_INT(PI_PLUGIN_API_VERSION_MAJOR(PI_PLUGIN_API_VERSION), 0);
    CHECK_EQ_INT(PI_PLUGIN_API_VERSION_MINOR(PI_PLUGIN_API_VERSION), 5);
    CHECK_EQ_INT(PI_PLUGIN_API_VERSION_MAKE(1, 0), 0x00010000);
    CHECK_EQ_INT(PI_PLUGIN_API_VERSION_MAKE(2, 5), 0x00020005);
    CHECK_EQ_INT(PI_PLUGIN_API_VERSION_MAJOR(PI_PLUGIN_API_VERSION_MAKE(0xFFFF, 0xFFFF)), 0xFFFF);
    CHECK_EQ_INT(PI_PLUGIN_API_VERSION_MINOR(PI_PLUGIN_API_VERSION_MAKE(0xFFFF, 0xFFFF)), 0xFFFF);

    /* 相等 -> 兼容 */
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION, PI_PLUGIN_API_VERSION) != 0);

    /* major 不同 -> 两个方向都不兼容（ABI 已变） */
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION, PI_PLUGIN_API_VERSION_MAKE(2, 0)) == 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(2, 0), PI_PLUGIN_API_VERSION) == 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(1, 9), PI_PLUGIN_API_VERSION_MAKE(2, 0)) == 0);

    /* 同 major、插件 minor 更高 -> 拒绝（插件可能用到宿主没有的接口） */
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(1, 3), PI_PLUGIN_API_VERSION_MAKE(1, 4)) == 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(1, 0), PI_PLUGIN_API_VERSION_MAKE(1, 1)) == 0);

    /* 同 major、插件 minor 更低或相等 -> 接受 */
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(1, 3), PI_PLUGIN_API_VERSION_MAKE(1, 2)) != 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(1, 3), PI_PLUGIN_API_VERSION_MAKE(1, 3)) != 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(1, 0), PI_PLUGIN_API_VERSION_MAKE(1, 0)) != 0);

    /* major 0（未版本化）只与 major 0 相容 —— 用确定的 major 1 来验，不要用
     * PI_PLUGIN_API_VERSION 本身（它的 major 随发布版本走，改版时会变）。 */
    CHECK(pi_plugin_api_version_compatible(0u, 0u) != 0);
    CHECK(pi_plugin_api_version_compatible(0u, PI_PLUGIN_API_VERSION_MAKE(1, 0)) == 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(1, 0), 0u) == 0);

    /* pre-1.0 语义（当前 API 0.4 / 发布 0.2.0）：同一 major 0 内，低 minor 兼容、
     * 高 minor 拒绝，所以插件应随宿主一起升级 —— 这正是 1.0 之前不承诺 ABI 的表现。 */
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(0, 4), PI_PLUGIN_API_VERSION_MAKE(0, 3)) != 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(0, 3), PI_PLUGIN_API_VERSION_MAKE(0, 4)) == 0);

    /* 测试所用的负向插件常量：必须被判为不兼容（与 BLK-03 的 ctest 用例呼应） */
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION, PI_PLUGIN_API_VERSION_MAKE(2, 0)) == 0);
    CHECK(pi_plugin_api_version_compatible(PI_PLUGIN_API_VERSION_MAKE(0, 2), PI_PLUGIN_API_VERSION_MAKE(2, 0)) == 0);
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
    IPiPluginHostServices* headless = NULL;
    IPiPluginHostServices* gui = NULL;
    void* out = NULL;

    Section("默认宿主服务（headless / GUI 两形态）");

    /* --- headless：传 PI_INVALID_WINDOW，不暴露 IPiPluginHostUI --- */
    CHECK_EQ_INT(pi_plugin_host_services_create_default(&OnInitPostMessage, NULL,
                                                 PI_INVALID_WINDOW, &headless), PI_OK);
    CHECK(headless != NULL);

    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_PLUGIN_IID_HOST_UI, &out),
                 PI_E_NOINTERFACE);
    CHECK(out == NULL);

    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_PLUGIN_IID_HOST_SERVICES, &out),
                 PI_OK);
    CHECK(out != NULL);
    if (out) pi_iunknown_release((IPiUnknown*)out);

    /* 消息回调（插件 → 宿主） */
    g_posted_msgs = 0;
    g_last_msg    = 0;
    pi_plugin_host_post_message(headless, 0x8001u, 7u, 0);
    CHECK_EQ_INT(g_posted_msgs, 1);
    CHECK_EQ_INT(g_last_msg, 0x8001);

    /* 宿主分配器 */
    {
        void* mem = pi_plugin_host_alloc(headless, 64);
        CHECK(mem != NULL);
        if (mem) memset(mem, 0xAB, 64);
        pi_plugin_host_free(headless, mem);
        pi_plugin_host_free(headless, NULL);   /* NULL 安全 */
    }

    /* --- 运行时切换到 GUI 形态：给了窗口就暴露 IPiPluginHostUI --- */
    pi_plugin_host_default_set_ui_window(headless, (PiNativeWindow)(uintptr_t)0x1234);
    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_PLUGIN_IID_HOST_UI, &out), PI_OK);
    CHECK(out != NULL);
    if (out) {
        IPiPluginHostUI* ui = (IPiPluginHostUI*)out;
        CHECK_EQ_INT((uintptr_t)pi_plugin_host_ui_get_parent_window(ui), (uintptr_t)0x1234);
        CHECK(pi_plugin_host_ui_thread_id(ui) != 0);
#if defined(_WIN32) || defined(_WIN64)
        /* BLK-08：Windows 分支必须给出**真正的线程 id**。非 Windows 分支曾经
         * 错给进程 id（getpid）；本仓库没有 Linux/macOS 构建可跑，那部分只能
         * 靠代码审查，见 docs/design/interface-freeze-review.md 的 F2。 */
        CHECK_EQ_INT(pi_plugin_host_ui_thread_id(ui), (uint64_t)GetCurrentThreadId());
#endif

        /* 活值语义：包装持有 owner 的引用，改窗口后已发出的 UI 指针立刻反映新值
         * （曾经这里是越界读、返回垃圾 —— 见 src/pi_plugin_host.c 的说明） */
        pi_plugin_host_default_set_ui_window(headless, (PiNativeWindow)(uintptr_t)0x9ABC);
        CHECK_EQ_INT((uintptr_t)pi_plugin_host_ui_get_parent_window(ui), (uintptr_t)0x9ABC);
        pi_plugin_host_default_set_ui_window(headless, (PiNativeWindow)(uintptr_t)0x1234);

        /* IPiPluginHostUI 自身 QI */
        {
            void* ui_out = NULL;
            CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)ui, &PI_PLUGIN_IID_HOST_UI, &ui_out),
                         PI_OK);
            if (ui_out) {
                /* 注意：本实现每次都新建一个包装对象（不是 COM 标识规则意义上的
                 * 同一指针）—— 这里只断言两个包装读到的宿主状态一致。
                 * "QI 是否必须返回同一指针"留待 BLK-08 接口终审定性。 */
                CHECK_EQ_INT((uintptr_t)pi_plugin_host_ui_get_parent_window((IPiPluginHostUI*)ui_out),
                             (uintptr_t)0x1234);
                pi_iunknown_release((IPiUnknown*)ui_out);
            }
        }
        pi_iunknown_release((IPiUnknown*)out);
    }

    /* --- 再切回 headless --- */
    pi_plugin_host_default_set_ui_window(headless, PI_INVALID_WINDOW);
    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)headless, &PI_PLUGIN_IID_HOST_UI, &out),
                 PI_E_NOINTERFACE);

    /* --- 直接以 GUI 形态创建 --- */
    CHECK_EQ_INT(pi_plugin_host_services_create_default(NULL, NULL,
                                                 (PiNativeWindow)(uintptr_t)0x5678, &gui), PI_OK);
    CHECK(gui != NULL);
    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)gui, &PI_PLUGIN_IID_HOST_UI, &out), PI_OK);
    if (out) pi_iunknown_release((IPiUnknown*)out);
    if (gui) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)gui), 0);

    /* --- 参数校验与"非本实现对象"防御 --- */
    CHECK_EQ_INT(pi_plugin_host_services_create_default(NULL, NULL, PI_INVALID_WINDOW, NULL),
                 PI_E_INVALIDARG);
    pi_plugin_host_default_set_ui_window(NULL, PI_INVALID_WINDOW);   /* NULL 安全 */

    if (headless) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)headless), 0);
}

/* --------------------------------------------------------------------------
 * APP-01 可组合宿主服务：pi_plugin_host_services_create_ex
 *
 * 用的是 tests/common 里那份"app 自定义宿主服务"实现 —— 与两个测试宿主、
 * 两个测试插件用的是同一份代码，所以这里验证的就是产品路径本身。
 * -------------------------------------------------------------------------- */
static uint32_t              g_unit_messages = 0;
static PiPluginTestHostServiceImpl g_unit_service;

/* 只数调用次数、一律不认的钩子：用来证明"框架 IID 不会转给钩子"。 */
static int g_unit_hook_calls = 0;

static PiResult UnitCountingHook(void* ctx, const PiGuid* iid, void** out)
{
    (void)ctx; (void)iid;
    ++g_unit_hook_calls;
    if (out) *out = NULL;
    return PI_E_NOINTERFACE;
}

/* 失败但**故意**往 *out 里写了脏值：框架必须把它清回 NULL（终审 2.4）。 */
static PiResult UnitFailingHook(void* ctx, const PiGuid* iid, void** out)
{
    (void)ctx; (void)iid;
    if (out) *out = (void*)&g_unit_service;
    return PI_E_OUTOFMEMORY;
}

/* 谎报成功却不给指针：按"不是我的"处理，不能让插件拿到一个 NULL 的"成功"。 */
static PiResult UnitSilentHook(void* ctx, const PiGuid* iid, void** out)
{
    (void)ctx; (void)iid; (void)out;
    return PI_OK;
}

static const PiGuid UNIT_UNKNOWN_IID = PI_GUID(0xDEADBEEF, 0x1234, 0x5678,
                                               0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44);

static void TestHostServicesCreateEx(void)
{
    IPiPluginHostServices* host = NULL;
    void* out = NULL;

    Section("APP-01 create_ex：app 自定义宿主服务 + create_default 回归");

    /* --- 1) 带 extra_qi：插件 QI 到 app 的服务，并成功调用它的方法 --- */
    g_unit_messages = 0;
    PiPluginTestHostServiceImpl_Init(&g_unit_service, "unit-test-host", &g_unit_messages);
    CHECK_EQ_INT(pi_plugin_host_services_create_ex(&OnInitPostMessage, NULL, PI_INVALID_WINDOW,
                                            &PiPluginTestHostServiceImpl_ExtraQi, &g_unit_service,
                                            &host), PI_OK);
    CHECK(host != NULL);
    CHECK_EQ_INT(g_unit_service.name_calls, 0);

    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)host, &PI_PLUGIN_TEST_IID_HOST_SERVICE, &out),
                 PI_OK);
    CHECK(out != NULL);
    if (out) {
        IPiPluginTestHostService* svc = (IPiPluginTestHostService*)out;
        CHECK(strcmp(pi_plugin_test_host_service_name(svc), "unit-test-host") == 0);
        CHECK_EQ_INT(g_unit_service.name_calls, 1);   /* 宿主侧真的被调到了 */
        CHECK_EQ_INT(pi_plugin_test_host_service_messages_seen(svc), 0);
        g_unit_messages = 7;
        CHECK_EQ_INT(pi_plugin_test_host_service_messages_seen(svc), 7);  /* 读到的是宿主活值 */
        CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)out), 1);    /* QI 返回 add-ref 过的 */
    }

    /* 消息回调（宿主 → 插件方向之外的既有行为）不受影响 */
    g_posted_msgs = 0;
    pi_plugin_host_post_message(host, 0x8001u, 7u, 0);
    CHECK_EQ_INT(g_posted_msgs, 1);

    /* 宿主默认实现的 UI 行为照旧：给了窗口就暴露 IPiPluginHostUI */
    pi_plugin_host_default_set_ui_window(host, (PiNativeWindow)(uintptr_t)0x1234);
    out = NULL;
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)host, &PI_PLUGIN_IID_HOST_UI, &out), PI_OK);
    if (out) pi_iunknown_release((IPiUnknown*)out);
    if (host) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)host), 0);
    host = NULL;

    /* --- 2) 钩子不认的 IID：NOINTERFACE，且 *out 必为 NULL（2.4 约定） --- */
    CHECK_EQ_INT(pi_plugin_host_services_create_ex(NULL, NULL, PI_INVALID_WINDOW,
                                            &PiPluginTestHostServiceImpl_ExtraQi, &g_unit_service,
                                            &host), PI_OK);
    out = (void*)(uintptr_t)0x1234;   /* 故意留脏值 */
    CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)host, &UNIT_UNKNOWN_IID, &out),
                 PI_E_NOINTERFACE);
    CHECK(out == NULL);

    /* --- 3) 框架自己的 IID 由框架先答掉，不转给钩子 --- */
    {
        IPiPluginHostServices* counted = NULL;
        g_unit_hook_calls = 0;
        CHECK_EQ_INT(pi_plugin_host_services_create_ex(NULL, NULL, (PiNativeWindow)(uintptr_t)0x5,
                                                &UnitCountingHook, NULL, &counted), PI_OK);
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)counted, &PI_IID_UNKNOWN, &out),
                     PI_OK);
        if (out) pi_iunknown_release((IPiUnknown*)out);
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)counted, &PI_PLUGIN_IID_HOST_SERVICES, &out),
                     PI_OK);
        if (out) pi_iunknown_release((IPiUnknown*)out);
        out = NULL;
        /* 有窗口时 IPiPluginHostUI 也是框架答的 */
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)counted, &PI_PLUGIN_IID_HOST_UI, &out),
                     PI_OK);
        if (out) pi_iunknown_release((IPiUnknown*)out);
        CHECK_EQ_INT(g_unit_hook_calls, 0);   /* 一个框架 IID 都没转出去 */

        /* 未知 IID 才转交；headless 的 IPiPluginHostUI 也算未命中（文档写明，
         * 这样 app 可以自己提供远端/代理式 UI 服务） */
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)counted, &UNIT_UNKNOWN_IID, &out),
                     PI_E_NOINTERFACE);
        CHECK_EQ_INT(g_unit_hook_calls, 1);
        pi_plugin_host_default_set_ui_window(counted, PI_INVALID_WINDOW);
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)counted, &PI_PLUGIN_IID_HOST_UI, &out),
                     PI_E_NOINTERFACE);
        CHECK_EQ_INT(g_unit_hook_calls, 2);
        if (counted) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)counted), 0);
    }

    /* --- 4) 钩子的失败语义：错误码原样上抛，*out 一定回到 NULL --- */
    {
        IPiPluginHostServices* failing = NULL;
        CHECK_EQ_INT(pi_plugin_host_services_create_ex(NULL, NULL, PI_INVALID_WINDOW,
                                                &UnitFailingHook, NULL, &failing), PI_OK);
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)failing, &UNIT_UNKNOWN_IID, &out),
                     PI_E_OUTOFMEMORY);
        CHECK(out == NULL);
        if (failing) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)failing), 0);
    }
    {
        IPiPluginHostServices* silent = NULL;
        CHECK_EQ_INT(pi_plugin_host_services_create_ex(NULL, NULL, PI_INVALID_WINDOW,
                                                &UnitSilentHook, NULL, &silent), PI_OK);
        out = (void*)(uintptr_t)0x99;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)silent, &UNIT_UNKNOWN_IID, &out),
                     PI_E_NOINTERFACE);
        CHECK(out == NULL);
        if (silent) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)silent), 0);
    }

    /* --- 5) 回归：extra_qi == NULL 与 create_default 完全一致 --- */
    {
        IPiPluginHostServices* plain_ex = NULL;
        IPiPluginHostServices* plain_default = NULL;

        CHECK_EQ_INT(pi_plugin_host_services_create_ex(NULL, NULL, PI_INVALID_WINDOW,
                                                NULL, NULL, &plain_ex), PI_OK);
        CHECK_EQ_INT(pi_plugin_host_services_create_default(NULL, NULL, PI_INVALID_WINDOW,
                                                     &plain_default), PI_OK);

        /* headless：两者都给不出 IPiPluginHostUI，都可 QI 到框架服务 */
        out = (void*)(uintptr_t)0x1;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)plain_ex, &PI_PLUGIN_IID_HOST_UI, &out),
                     PI_E_NOINTERFACE);
        CHECK(out == NULL);
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)plain_default, &PI_PLUGIN_IID_HOST_UI, &out),
                     PI_E_NOINTERFACE);
        CHECK(out == NULL);

        /* app 自定义服务：没有钩子就是没有 */
        out = (void*)(uintptr_t)0x1;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)plain_ex,
                                                 &PI_PLUGIN_TEST_IID_HOST_SERVICE, &out),
                     PI_E_NOINTERFACE);
        CHECK(out == NULL);

        /* 给了窗口后两者都暴露 IPiPluginHostUI（且都能被 set_ui_window 切换） */
        pi_plugin_host_default_set_ui_window(plain_ex, (PiNativeWindow)(uintptr_t)0x1234);
        pi_plugin_host_default_set_ui_window(plain_default, (PiNativeWindow)(uintptr_t)0x1234);
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)plain_ex, &PI_PLUGIN_IID_HOST_UI, &out),
                     PI_OK);
        if (out) {
            CHECK_EQ_INT((uintptr_t)pi_plugin_host_ui_get_parent_window((IPiPluginHostUI*)out),
                         (uintptr_t)0x1234);
            pi_iunknown_release((IPiUnknown*)out);
        }
        out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)plain_default, &PI_PLUGIN_IID_HOST_UI, &out),
                     PI_OK);
        if (out) {
            CHECK_EQ_INT((uintptr_t)pi_plugin_host_ui_get_parent_window((IPiPluginHostUI*)out),
                         (uintptr_t)0x1234);
            pi_iunknown_release((IPiUnknown*)out);
        }

        if (plain_ex) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)plain_ex), 0);
        if (plain_default) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)plain_default), 0);
    }

    /* --- 6) 参数校验 --- */
    CHECK_EQ_INT(pi_plugin_host_services_create_ex(NULL, NULL, PI_INVALID_WINDOW,
                                            NULL, NULL, NULL), PI_E_INVALIDARG);

    if (host) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)host), 0);
}

/* --------------------------------------------------------------------------
 * ECO-08：工厂的负向用例（未知 class GUID / 非法参数）
 *
 * 需要一个真实插件 DLL；由 ctest 把插件名当 argv[1] 传进来。没传就打印一行说明
 * 并跳过 —— 但正向控制也在这里（真 GUID 必须成功），避免出现"全都失败"式的假通过。
 * -------------------------------------------------------------------------- */
static void TestFactoryNegative(const char* plugin_path)
{
    Section("ECO-08 工厂负向用例（未知 GUID / 非法参数）");

    if (!plugin_path || !plugin_path[0]) {
        printf("  (no plugin path given: skipping the factory negative cases)\n");
        return;
    }

    {
        static const PiGuid UNKNOWN_CLASS_GUID =
            PI_GUID(0x0BADF00D, 0x1234, 0x5678, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78);
        PiPluginModule* module = pi_plugin_module_load(plugin_path);
        IPiPluginFactory* factory = NULL;
        IPiPluginHostServices* host = NULL;
        IPiPluginBase* out = NULL;
        PiGuid real_guid;
        PiGuid guid_out;

        CHECK(module != NULL);
        if (!module) { printf("  load error: %s\n", pi_plugin_module_get_load_error()); return; }

        CHECK_EQ_INT(pi_plugin_module_get_factory(module, &factory), PI_OK);
        CHECK(factory != NULL);
        CHECK_EQ_INT(pi_plugin_host_services_create_default(NULL, NULL, PI_INVALID_WINDOW, &host), PI_OK);

        /* 1) 未知 class GUID -> PI_E_NOINTERFACE，且 *out 被置 NULL */
        out = (IPiPluginBase*)(uintptr_t)0x1;    /* 故意留脏值 */
        CHECK_EQ_INT(pi_plugin_factory_create_instance(factory, &UNKNOWN_CLASS_GUID, host, &out),
                     PI_E_NOINTERFACE);
        CHECK(out == NULL);

        /* 2) 非法参数 */
        out = NULL;
        CHECK_EQ_INT(pi_plugin_factory_create_instance(factory, NULL, host, &out), PI_E_INVALIDARG);
        CHECK_EQ_INT(pi_plugin_factory_create_instance(factory, &UNKNOWN_CLASS_GUID, host, NULL),
                     PI_E_INVALIDARG);
        memset(&guid_out, 0, sizeof(guid_out));
        CHECK_EQ_INT(pi_plugin_factory_get_class_guid(factory, 99u, &guid_out), PI_E_INVALIDARG);
        CHECK_EQ_INT(pi_plugin_factory_get_class_guid(factory, 0u, NULL), PI_E_INVALIDARG);

        /* 3) 正向控制：真 GUID 必须成功 —— 否则上面那些"失败"什么都证明不了 */
        memset(&real_guid, 0, sizeof(real_guid));
        CHECK_EQ_INT(pi_plugin_factory_get_class_guid(factory, 0u, &real_guid), PI_OK);
        out = NULL;
        CHECK_EQ_INT(pi_plugin_factory_create_instance(factory, &real_guid, host, &out), PI_OK);
        CHECK(out != NULL);
        if (out) {
            CHECK_EQ_INT(pi_plugin_initialize(out, host), PI_OK);
            CHECK_EQ_INT(pi_plugin_terminate(out), PI_OK);
            CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)out), 0);
        }

        if (host) CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)host), 0);
        if (factory) pi_iunknown_release((IPiUnknown*)factory);
        pi_plugin_module_unload(module);
    }
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char** argv)
{
    g_argv0       = (argc > 0) ? argv[0] : NULL;
    g_plugin_path = (argc > 1) ? argv[1] : NULL;

    printf("== piplugin unit tests ==\n");
    TestGuidEqual();
    TestDescriptorHelpers();
    TestDescriptorProperties();
    TestRefCounted();
    TestModuleLoadFailure();
    TestLoadErrorThreadSafety();
    TestApiVersion();
    TestHostServices();
    TestHostServicesCreateEx();
    TestFactoryNegative(g_plugin_path);

    printf("== checks=%u failures=%u ==\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
