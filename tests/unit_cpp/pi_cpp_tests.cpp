/*
 * piplugin - C++ RAII layer tests (tests/unit_cpp)
 *
 * Covers include/piplugin/pi_cpp.h - the optional C++ sugar over the C ABI:
 *   PiPluginPtr<T>          adopt / add_ref / put / reset / detach / move / qi_to
 *   PiPluginIidOf<T>        interface type -> framework IID
 *   PiPluginUniqueModule    module RAII + load + factory
 *   pi_plugin_cpp_destroy<T> the PiRefCountedBase destroy thunk
 *
 * Two kinds of assertion, on purpose:
 *
 *   1. Behaviour. A counted C object reports its own refcount, so "the handle
 *      released exactly what it took" is asserted as a number rather than
 *      inferred from "it did not crash";
 *   2. Leaks. With the Debug CRT (MSVC) the leak check runs after everything
 *      else has been torn down and the exit code depends on it: that is the
 *      acceptance the roadmap asks for ("no leak"), and it is why this is an
 *      executable of its own instead of a section inside the C unit suite.
 *
 * Run with a plugin DLL as argv[1] to exercise PiPluginUniqueModule against a real
 * module; without one, only the loader's failure path is checked.
 */
#include "piplugin/pi_cpp.h"

#include <stdio.h>
#include <string.h>
#include <type_traits>

#if defined(_MSC_VER) && defined(_DEBUG)
#  define PI_PLUGIN_CPP_CRT_LEAK_CHECK 1
#  include <crtdbg.h>
#endif

/* --------------------------------------------------------------------------
 * Minimal assertion framework, same shape as tests/unit/pi_unit_tests.c
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
        long long a_ = (long long)(actual);                             \
        long long e_ = (long long)(expected);                           \
        ++g_checks;                                                     \
        if (a_ != e_) {                                                 \
            ++g_failures;                                               \
            printf("  FAIL %s:%d: %s == %lld, expected %lld\n",         \
                   __FILE__, __LINE__, #actual, a_, e_);                \
        }                                                               \
    } while (0)

static void Section(const char* name)
{
    printf("- %s\n", name);
}

/* --------------------------------------------------------------------------
 * A counted COM-style object: a PiRefCountedBase, a vtable, and a refcount the
 * assertions can read. It answers two IIDs so that qi_to() has both a hit and a
 * miss to check.
 * -------------------------------------------------------------------------- */
static int g_live_objects  = 0;
static int g_destroy_calls = 0;

static const PiGuid COUNTED_IID = PI_GUID(0x5C2B77E1, 0x0F4A, 0x4D19,
                                          0xB3, 0x62, 0x9D, 0x18, 0x77, 0xE4, 0x0A, 0x30);
static const PiGuid COUNTED_ALT_IID = PI_GUID(0x8A31D0F7, 0x6C22, 0x4B08,
                                              0x9E, 0x14, 0x33, 0xAB, 0x52, 0x6F, 0xC8, 0x11);

struct Counted {
    PiRefCountedBase base;   /* MUST be first */
};

static uint32_t PI_CALL CountedAddRef(void* self) { return pi_refcounted_add_ref(self); }
static uint32_t PI_CALL CountedRelease(void* self) { return pi_refcounted_release(self); }

static PiResult PI_CALL CountedQi(void* self, const PiGuid* iid, void** out)
{
    if (!out) return PI_E_INVALIDARG;
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) ||
        pi_guid_equal(iid, &COUNTED_IID) ||
        pi_guid_equal(iid, &COUNTED_ALT_IID)) {
        *out = self;
        pi_iunknown_add_ref((IPiUnknown*)self);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static const IPiUnknownVtbl s_counted_vtbl = {
    &CountedQi, &CountedAddRef, &CountedRelease
};

static void CountedDestroy(void* self)
{
    ++g_destroy_calls;
    --g_live_objects;
    delete static_cast<Counted*>(self);
}

static Counted* CountedNew()
{
    Counted* obj = new Counted();
    pi_refcounted_init_with_destroy(&obj->base, &s_counted_vtbl, &CountedDestroy);
    ++g_live_objects;
    return obj;
}

/* 应用侧给自定义接口补 PiPluginIidOf 特化（pi_cpp.h 顶部注释里的用法）：
 * 有了它，qi_to<Counted>() 不带 IID 也能编译。 */
template <> struct PiPluginIidOf<Counted> {
    static const PiGuid& get() { return COUNTED_IID; }
};

/* --------------------------------------------------------------------------
 * Compile-time contract of the handle types
 * -------------------------------------------------------------------------- */
static_assert(!std::is_copy_constructible<PiPluginPtr<IPiUnknown> >::value,
              "PiPluginPtr must not be copyable - an implicit copy is an unbalanced AddRef");
static_assert(!std::is_copy_assignable<PiPluginPtr<IPiUnknown> >::value,
              "PiPluginPtr must not be copy-assignable");
static_assert(std::is_move_constructible<PiPluginPtr<IPiUnknown> >::value,
              "PiPluginPtr must be movable");
static_assert(std::is_move_assignable<PiPluginPtr<IPiUnknown> >::value,
              "PiPluginPtr must be move-assignable");
static_assert(!std::is_convertible<IPiUnknown*, PiPluginPtr<IPiUnknown> >::value,
              "PiPluginPtr's constructor is explicit: adopting a reference must be spelled out");
static_assert(!std::is_copy_constructible<PiPluginUniqueModule>::value,
              "PiPluginUniqueModule must not be copyable - unloading twice is a double free");
static_assert(std::is_move_constructible<PiPluginUniqueModule>::value,
              "PiPluginUniqueModule must be movable");

/* --------------------------------------------------------------------------
 * PiPluginPtr
 * -------------------------------------------------------------------------- */
static void TestPiPtr()
{
    Section("PiPluginPtr：接管 / 加引用 / 移动 / detach / put / reset / qi_to");

    /* 每个用例各自从零开始数：这样"放了几个引用"就是每个用例里的绝对数字，
     * 而不是靠累加推断。 */
    g_live_objects = 0;
    g_destroy_calls = 0;

    /* 1) 接管一个已有引用：析构时恰好放掉一次 -> 对象销毁 */
    {
        Counted* raw = CountedNew();
        CHECK_EQ_INT(raw->base.ref_count, 1);
        {
            PiPluginPtr<Counted> held(raw);            /* adopt：不再加引用 */
            CHECK_EQ_INT(raw->base.ref_count, 1);
            CHECK(held.is_valid());
            CHECK(held.get() == raw);
            CHECK(static_cast<bool>(held));
        }
        CHECK_EQ_INT(g_destroy_calls, 1);
        CHECK_EQ_INT(g_live_objects, 0);
    }

    /* 2) add_ref：给借用指针造第二个持有者，两个都放掉才销毁 */
    {
        g_live_objects = 0; g_destroy_calls = 0;
        Counted* raw = CountedNew();
        PiPluginPtr<Counted> first(raw);               /* adopt */
        {
            PiPluginPtr<Counted> second = PiPluginPtr<Counted>::add_ref(first.get());
            CHECK_EQ_INT(raw->base.ref_count, 2);
            CHECK(second.get() == raw);
        }
        CHECK_EQ_INT(raw->base.ref_count, 1);
        CHECK_EQ_INT(g_destroy_calls, 0);        /* first 还活着 */
    }
    CHECK_EQ_INT(g_destroy_calls, 1);
    CHECK_EQ_INT(g_live_objects, 0);

    /* 3) 移动构造 / 移动赋值：源被清空，同一时刻只有一个句柄持有那个引用 */
    {
        g_live_objects = 0; g_destroy_calls = 0;
        Counted* raw = CountedNew();
        PiPluginPtr<Counted> source(raw);
        PiPluginPtr<Counted> moved(static_cast<PiPluginPtr<Counted>&&>(source));
        CHECK(!source.is_valid());
        CHECK(moved.get() == raw);
        CHECK_EQ_INT(raw->base.ref_count, 1);

        PiPluginPtr<Counted> assigned;
        assigned = static_cast<PiPluginPtr<Counted>&&>(moved);
        CHECK(!moved.is_valid());
        CHECK(assigned.get() == raw);
        CHECK_EQ_INT(raw->base.ref_count, 1);
    }
    CHECK_EQ_INT(g_destroy_calls, 1);
    CHECK_EQ_INT(g_live_objects, 0);

    /* 4) detach：把引用交回调用方，句柄清空且不释放 */
    {
        g_live_objects = 0; g_destroy_calls = 0;
        Counted* raw = CountedNew();
        PiPluginPtr<Counted> held(raw);
        Counted* out = held.detach();
        CHECK(out == raw);
        CHECK(!held.is_valid());
        CHECK_EQ_INT(g_destroy_calls, 0);
        pi_iunknown_release((IPiUnknown*)out);   /* 现在归调用方 */
        CHECK_EQ_INT(g_destroy_calls, 1);
    }

    /* 5) put()：先释放自己持有的，再给出参地址（框架失败时会把 *out 置 NULL）。
     *    "我们自己的那一份引用"与"QI 刚给插件的那一份"都要交代清楚 —— 本用例
     *    第一版就是漏了后者，Debug CRT 的泄漏检查把它抓了出来。 */
    {
        g_live_objects = 0; g_destroy_calls = 0;
        Counted* first = CountedNew();            /* 我们持有 first 的初始引用 */
        PiPluginPtr<Counted> held(first);

        Counted* second = CountedNew();           /* 我们持有 second 的初始引用 */
        void* out = NULL;
        CHECK_EQ_INT(pi_iunknown_query_interface((IPiUnknown*)second, &PI_IID_UNKNOWN, &out),
                     PI_OK);                      /* QI 又加了一份给调用方 */
        CHECK_EQ_INT(second->base.ref_count, 2);

        *held.put() = static_cast<Counted*>(out); /* 旧的 first 在这里被放掉（已析构，
                                                   * 所以不再读它的 ref_count） */
        CHECK_EQ_INT(g_destroy_calls, 1);
        CHECK(held.get() == second);

        pi_iunknown_release((IPiUnknown*)second); /* 放掉 CountedNew 给的那一份 */
        CHECK_EQ_INT(g_destroy_calls, 1);         /* 句柄那份还在，所以还没销毁 */
        CHECK_EQ_INT(second->base.ref_count, 1);
        CHECK_EQ_INT(g_live_objects, 1);

        held.reset();
        CHECK_EQ_INT(g_destroy_calls, 2);
        CHECK_EQ_INT(g_live_objects, 0);
    }

    /* 6) qi_to：命中 -> 持有新引用；未命中 -> 空句柄（不是野指针）。
     *    带 IID 与不带 IID（走 PiPluginIidOf 特化）两种写法都要对。 */
    {
        g_live_objects = 0; g_destroy_calls = 0;
        Counted* raw = CountedNew();
        PiPluginPtr<Counted> held(raw);

        PiPluginPtr<Counted> by_iid = held.qi_to<Counted>(COUNTED_ALT_IID);
        CHECK(by_iid.is_valid());
        CHECK(by_iid.get() == raw);
        CHECK_EQ_INT(raw->base.ref_count, 2);

        PiPluginPtr<Counted> by_trait = held.qi_to<Counted>();
        CHECK(by_trait.is_valid());
        CHECK_EQ_INT(raw->base.ref_count, 3);

        PiPluginPtr<Counted> miss = held.qi_to<Counted>(PI_PLUGIN_IID_PLUGIN_VIEW);
        CHECK(!miss.is_valid());
        CHECK(miss.get() == NULL);
        CHECK_EQ_INT(raw->base.ref_count, 3);

        /* 空句柄 QI 也是空句柄，不会解引用 */
        PiPluginPtr<Counted> empty;
        CHECK(!empty.qi_to<Counted>().is_valid());
    }
    CHECK_EQ_INT(g_destroy_calls, 1);
    CHECK_EQ_INT(g_live_objects, 0);

    /* 7) pi_plugin_cpp_destroy<T>：引用归零时真的跑 C++ 析构函数（插件的实际用法） */
    {
        struct WithDtor {
            PiRefCountedBase base;   /* MUST be first */
            int* dtor_calls;
            ~WithDtor() { if (dtor_calls) ++(*dtor_calls); }
        };

        int dtor_calls = 0;
        WithDtor* obj = new WithDtor();
        obj->dtor_calls = &dtor_calls;
        pi_refcounted_init_with_destroy(&obj->base, &s_counted_vtbl,
                                        &pi_plugin_cpp_destroy<WithDtor>);
        CHECK_EQ_INT(dtor_calls, 0);

        pi_iunknown_release((IPiUnknown*)&obj->base);
        CHECK_EQ_INT(dtor_calls, 1);
    }
}

/* --------------------------------------------------------------------------
 * PiPluginUniqueModule
 * -------------------------------------------------------------------------- */
static void TestPiUniqueModule(const char* plugin_path)
{
    Section("PiPluginUniqueModule：加载失败路径 + 真实模块生命周期");

    /* 1) 失败路径：不存在的 DLL -> PI_E_NOTFOUND，句柄保持为空 */
    {
        PiPluginUniqueModule module;
        CHECK_EQ_INT(PiPluginUniqueModule::load("Z:\\no\\such\\piplugin\\missing.dll", module),
                     PI_E_NOTFOUND);
        CHECK(!module);
        CHECK(module.get() == NULL);
        CHECK(!module.factory().is_valid());
        module.reset();                      /* 空句柄 reset 安全 */
        CHECK(!module);
    }

    if (!plugin_path || !plugin_path[0]) {
        printf("  (no plugin path given: skipping the real-module part)\n");
        return;
    }

    /* 2) 真实模块：加载 -> 工厂 -> 实例 -> 初始化，再按加载的逆序析构。
     *
     * 声明顺序即加载顺序（host, module, factory, plugin），而 C++ 按声明逆序
     * 析构：plugin -> factory -> module -> host 正好就是七步卸载序列要求的顺序，
     * 模块一定在它创建的每个对象之后卸载。 */
    {
        PiPluginPtr<IPiPluginHostServices> host;
        CHECK_EQ_INT(pi_plugin_host_services_create_default(NULL, NULL, PI_INVALID_WINDOW,
                                                     host.put()), PI_OK);
        CHECK(host.is_valid());

        PiPluginUniqueModule module;
        CHECK_EQ_INT(PiPluginUniqueModule::load(plugin_path, module), PI_OK);
        CHECK(module);
        if (!module) return;

        PiPluginPtr<IPiPluginFactory> factory = module.factory();
        CHECK(factory.is_valid());

        const PiPluginDescriptor* desc = NULL;   /* 借用指针：禁止包进 PiPluginPtr */
        pi_plugin_factory_get_descriptor(factory.get(), &desc);
        CHECK(desc != NULL);
        if (desc) {
            printf("  plugin: %s %s\n",
                   desc->name ? desc->name : "?", desc->version ? desc->version : "?");
        }

        PiGuid class_guid;
        memset(&class_guid, 0, sizeof(class_guid));
        CHECK_EQ_INT(pi_plugin_factory_get_class_guid(factory.get(), 0, &class_guid), PI_OK);

        PiPluginPtr<IPiPluginBase> plugin;
        CHECK_EQ_INT(pi_plugin_factory_create_instance(factory.get(), &class_guid,
                                                host.get(), plugin.put()), PI_OK);
        CHECK(plugin.is_valid());
        CHECK_EQ_INT(pi_plugin_initialize(plugin.get(), host.get()), PI_OK);

        /* headless 宿主（无窗口）：IPiPluginHostUI 必须查不到，而且失败给的是空句柄 */
        PiPluginPtr<IPiPluginHostUI> ui = host.qi_to<IPiPluginHostUI>();
        CHECK(!ui.is_valid());
        CHECK(ui.get() == NULL);

        CHECK_EQ_INT(pi_plugin_terminate(plugin.get()), PI_OK);

        plugin.reset();     /* 实例先走 */
        factory.reset();    /* 再工厂 */
        module.reset();     /* 最后才卸模块 */
        CHECK(!module);
        /* host 在作用域结束时释放 —— 构造函数/析构函数里没有一行手写 release */
    }
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char** argv)
{
#if defined(PI_PLUGIN_CPP_CRT_LEAK_CHECK)
    /* Debug CRT：退出时把所有未释放的分配打出来，_CrtDumpMemoryLeaks() 的返回值
     * 决定本次运行是否算失败。 */
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    const char* plugin_path = (argc > 1) ? argv[1] : NULL;

    printf("== piplugin C++ RAII layer tests ==\n");
    TestPiPtr();
    TestPiUniqueModule(plugin_path);

    printf("== checks=%u failures=%u ==\n", g_checks, g_failures);

#if defined(PI_PLUGIN_CPP_CRT_LEAK_CHECK)
    /* 最后一道闸：此刻所有 PiPluginPtr / PiPluginUniqueModule 都已析构，任何漏掉的
     * AddRef/release 都会在这里现形。报告写到调试输出，返回值进退出码。 */
    if (_CrtDumpMemoryLeaks()) {
        ++g_failures;
        printf("  FAIL: the Debug CRT reported memory leaks\n");
    } else {
        printf("  CRT leak check: clean\n");
    }
#endif

    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
