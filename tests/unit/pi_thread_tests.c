/*
 * piplugin - 线程安全专项（W-04）：三个跨线程场景里"不需要 UI 工具包"的两个
 *
 * 由 ctest 注册为 `unit_threads`（tests/unit/CMakeLists.txt）。与 pi_unit_tests.c
 * 用同一套断言宏风格与同一份线程封装（tests/common/pi_test_thread.h），但刻意分成
 * 独立可执行文件：并发用例真的起线程、真的互相踩，比其它单测慢，单独一个用例
 * 便于 `ctest -R unit_threads` 反复跑（压测要的不是"跑过"，是"跑很多遍不抖"）。
 *
 * 覆盖：
 *   1. 插件子线程调用 pi_host_post_message —— 宿主按 marshal 约定收到
 *      （宿主回调必须真的在**子线程**上被调到，然后由宿主自己的队列搬到主线程；
 *       断言不丢、不重、payload 正确、主线程上恰好投递一次）
 *   2. 并发 AddRef/Release 压力 —— 计数归零、destroy 恰好一次
 *      （帮助函数与 vtbl 槽位两条路径都压）
 *
 * 第三个场景（Qt 套件 pi_qt_view_post 跨线程）需要真 Qt 插件与宿主，在
 * tests/test_host_multi（ctest `qt_view_post_from_worker_thread`）。
 */
#include "piplugin/pi_plugin.h"
#include "pi_test_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>   /* GetCurrentThreadId：断言"回调发生在哪条线程"用 */
#endif

/* --------------------------------------------------------------------------
 * 断言框架（与 pi_unit_tests.c 同形：跑完全部再按失败数决定退出码）
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

static unsigned long CurrentThreadId(void)
{
#if defined(_WIN32) || defined(_WIN64)
    return (unsigned long)GetCurrentThreadId();
#else
    /* 非 Windows 的并发跑道还没有（见 W-10），这里只要一个"同线程稳定、
     * 跨线程不同"的值用于比较，不承诺它是内核 tid。 */
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

/* ==========================================================================
 * 场景 1：插件子线程调 pi_host_post_message，宿主按 marshal 约定收到
 *
 * 契约（interfaces.md §6 / pi_plugin_host_services.h）：`pi_host_post_message`
 * **任意线程可调**，宿主自己决定怎么 marshal 到自己的事件循环。所以这里的
 * "宿主"照契约做一遍：回调可能在任意线程上被调到 —— 把消息连同调用线程 id
 * 记进一把锁保护的队列，再由主线程（模拟事件循环）取出来投递。
 *
 * 断言三件事：
 *   a) 子线程发起的调用**真的在子线程上**到达宿主回调（不是被框架悄悄串行化）；
 *   b) 一条不丢、一条不重、payload 原样；
 *   c) 每一条都在"主线程投递"这一步恰好出现一次（宿主 marshal 的结果）。
 * ========================================================================== */
#define POST_THREADS    3
#define POST_PER_THREAD 200
#define POST_QUEUE_MAX  (POST_THREADS * POST_PER_THREAD)

typedef struct PostedMessage {
    uint32_t      msg;
    uintptr_t     wparam;
    unsigned long caller_thread;
} PostedMessage;

static PiTestMutex   g_post_mutex;
static PostedMessage g_post_queue[POST_QUEUE_MAX];
static int           g_post_queued    = 0;   /* 宿主回调收到的条数（任意线程） */
static int           g_post_overflow  = 0;   /* 队列不够用（计数/容量有 bug） */
static int           g_post_delivered = 0;   /* 主线程真正投递出去的条数 */
static int           g_post_bad_delivery = 0;/* 主线程投递时发现的不一致 */
static unsigned long g_post_main_thread = 0;
static int           g_post_seen[POST_THREADS][POST_PER_THREAD];

/* 线程体要拿到宿主服务对象：模块级指针，线程启动前写好 */
static IPiHostServices* g_post_services = NULL;

/* 宿主侧消息回调：**可能在任意线程上**被调到（这就是契约的全部意思） */
static void HostPostProc(void* user_data, uint32_t msg,
                         uintptr_t wparam, intptr_t lparam)
{
    unsigned long caller = CurrentThreadId();
    (void)user_data; (void)lparam;

    PiTestMutexLock(&g_post_mutex);
    if (g_post_queued < POST_QUEUE_MAX) {
        PostedMessage* slot = &g_post_queue[g_post_queued++];
        slot->msg           = msg;
        slot->wparam        = wparam;
        slot->caller_thread = caller;
    } else {
        ++g_post_overflow;
    }
    PiTestMutexUnlock(&g_post_mutex);
}

/* "主线程事件循环"：把队列里的消息搬出去投递（宿主真正要写的那一步） */
static void DrainPostedMessagesOnMainThread(void)
{
    int i;
    PiTestMutexLock(&g_post_mutex);
    for (i = 0; i < g_post_queued; ++i) {
        PostedMessage* m = &g_post_queue[i];
        unsigned thread_index = (unsigned)(m->wparam / POST_PER_THREAD);
        unsigned seq          = (unsigned)(m->wparam % POST_PER_THREAD);

        if (CurrentThreadId() != g_post_main_thread) ++g_post_bad_delivery;
        if (m->msg != 0x4000u + thread_index)       ++g_post_bad_delivery;
        if (thread_index >= POST_THREADS || seq >= POST_PER_THREAD)
            ++g_post_bad_delivery;
        else
            g_post_seen[thread_index][seq] += 1;
        ++g_post_delivered;
    }
    PiTestMutexUnlock(&g_post_mutex);
}

typedef struct PostWorker {
    unsigned      index;
    unsigned long thread_id;
} PostWorker;

static void PostWorkerThread(void* user_data)
{
    PostWorker* w = (PostWorker*)user_data;
    unsigned i;
    w->thread_id = CurrentThreadId();
    for (i = 0; i < POST_PER_THREAD; ++i) {
        /* wparam 同时编码"哪个线程的第几条"，便于断言不丢不重 */
        pi_host_post_message(g_post_services,
                             0x4000u + w->index,
                             (uintptr_t)(w->index * POST_PER_THREAD + i),
                             0);
        if ((i % 16) == 0) PiTestThreadYield();   /* 让线程真的交错 */
    }
}

static void TestCrossThreadPostMessage(void)
{
    IPiHostServices* services = NULL;
    PiTestThread     threads[POST_THREADS];
    PostWorker       workers[POST_THREADS];
    unsigned         i, t;
    int              distinct_callers = 0;
    int              worker_calls     = 0;
    int              duplicates       = 0;
    int              total_seen       = 0;

    Section("W-04/1 插件子线程 pi_host_post_message：宿主 marshal 后主线程收到");

    g_post_main_thread = CurrentThreadId();
    PiTestMutexInit(&g_post_mutex);
    memset(g_post_seen, 0, sizeof(g_post_seen));

    CHECK_EQ_INT(pi_host_services_create_default(&HostPostProc, NULL,
                                                 PI_INVALID_WINDOW, &services), PI_OK);
    CHECK(services != NULL);
    if (!services) return;

    g_post_services = services;

    for (i = 0; i < POST_THREADS; ++i) {
        memset(&workers[i], 0, sizeof(workers[i]));
        workers[i].index = i;
        CHECK_EQ_INT(PiTestThreadStart(&threads[i], &PostWorkerThread, &workers[i]), 0);
    }
    for (i = 0; i < POST_THREADS; ++i) PiTestThreadJoin(&threads[i]);

    /* a) 回调必须真的在**子线程**上被调到：否则"跨线程 post"根本没被测到 */
    for (i = 0; i < POST_THREADS; ++i) {
        if (workers[i].thread_id != 0 && workers[i].thread_id != g_post_main_thread)
            ++distinct_callers;
    }
    CHECK_EQ_INT(distinct_callers, POST_THREADS);

    /* b) 数量与来源：一条不丢，且每一条都来自某条子线程 */
    CHECK_EQ_INT(g_post_overflow, 0);
    CHECK_EQ_INT(g_post_queued, POST_THREADS * POST_PER_THREAD);

    PiTestMutexLock(&g_post_mutex);
    for (i = 0; i < (unsigned)g_post_queued; ++i) {
        if (g_post_queue[i].caller_thread != g_post_main_thread) ++worker_calls;
    }
    PiTestMutexUnlock(&g_post_mutex);
    CHECK_EQ_INT(worker_calls, POST_THREADS * POST_PER_THREAD);

    /* c) 主线程投递一遍：payload/消息码/投递线程逐条核对，且不重 */
    DrainPostedMessagesOnMainThread();
    CHECK_EQ_INT(g_post_delivered, POST_THREADS * POST_PER_THREAD);
    CHECK_EQ_INT(g_post_bad_delivery, 0);
    for (t = 0; t < POST_THREADS; ++t) {
        for (i = 0; i < POST_PER_THREAD; ++i) {
            if (g_post_seen[t][i] != 1) ++duplicates;
            total_seen += g_post_seen[t][i];
        }
    }
    CHECK_EQ_INT(duplicates, 0);
    CHECK_EQ_INT(total_seen, POST_THREADS * POST_PER_THREAD);

    PiTestMutexDestroy(&g_post_mutex);
    CHECK_EQ_INT(pi_iunknown_release((IPiUnknown*)services), 0);
}

/* ==========================================================================
 * 场景 2：并发 AddRef/Release 压力 —— 计数归零、destroy 恰好一次
 *
 * 两段：
 *   A) 4 线程 × 400 轮"成对"add/release（净效果 0）→ 基数必须还是 1、不得销毁；
 *   B) 预置 N 份引用后，N 条线程**同时**各放一份 —— 归零那一下发生在并发里，
 *      destroy 必须恰好一次；
 *   C) 丢更新探测器：4 线程各在紧循环里 add_ref 2000 次（只加不减）→ 计数必须
 *      **分毫不差**地等于 1 + 4*2000。成对操作会互相抵消，看不出丢更新；只加
 *      不减会把任何一次丢失累加起来，所以这一段是"计数是否真的原子"的判据。
 * 帮助函数与 vtbl 槽位两条路径各跑一遍。
 *
 * 计数是原子的（src/pi_plugin_unknown.c 用 Interlocked/__sync）。关于本用例的
 * 灵敏度，实测结论写在这里，免得后来者高估它：
 *   - 把实现换成普通 `++`（三条指令的读改写、窗口只有一两个周期）时，本机
 *     （16 逻辑核）跑 12 次都没抓到丢更新 —— 指令级窗口太窄，调度不一定重叠；
 *   - 给递增注入一个明确窗口（读与写之间 Sleep(0)）后，本用例立刻失败
 *     （并发释放把对象提前销毁，直接 AV）—— 说明断言本身是有效的，
 *     只是"窄窗口 + 抢占式调度"不保证每次都能撞上。
 * 所以它的价值是"计数必须精确"的回归 + 真并发压力下的不崩不腐，而不是
 * "任何非原子实现都必然被抓"。
 * ========================================================================== */
#define REF_THREADS     4
#define REF_ROUNDS      400
#define REF_INC_THREADS 4
#define REF_INC_ROUNDS  2000

static int          g_ref_destroy_calls = 0;
static PiTestMutex  g_ref_mutex;
static volatile int g_ref_go = 0;

static void RefCountedDestroy(void* self_ptr)
{
    /* destroy 可能发生在任意一条线程上（这里就是并发归零那一次），故计数加锁 */
    PiTestMutexLock(&g_ref_mutex);
    ++g_ref_destroy_calls;
    PiTestMutexUnlock(&g_ref_mutex);
    free(self_ptr);
}

static uint32_t PI_CALL RefTestAddRef(void* self_ptr) { return pi_refcounted_add_ref(self_ptr); }
static uint32_t PI_CALL RefTestRelease(void* self_ptr) { return pi_refcounted_release(self_ptr); }

static const IPiUnknownVtbl s_ref_test_vtbl = {
    NULL,
    &RefTestAddRef,
    &RefTestRelease
};

typedef struct RefWorker {
    PiRefCountedBase* obj;
    int               use_vtbl;   /* 0 = 帮助函数，1 = vtbl 槽位 */
    int               released;   /* 只释放一次的那一段用来确认线程真的跑了 */
} RefWorker;

/* A 段：成对的 add/release，净效果为零 */
static void RefPairWorkerThread(void* user_data)
{
    RefWorker* w = (RefWorker*)user_data;
    int i;
    for (i = 0; i < REF_ROUNDS; ++i) {
        if (w->use_vtbl) {
            w->obj->unk.lpVtbl->pi_add_ref(w->obj);
            w->obj->unk.lpVtbl->pi_release(w->obj);
        } else {
            pi_refcounted_add_ref(w->obj);
            pi_refcounted_release(w->obj);
        }
    }
}

/* B 段：一起出发、各放掉一份引用 —— 归零与 destroy 都发生在并发里 */
static void RefReleaseWorkerThread(void* user_data)
{
    RefWorker* w = (RefWorker*)user_data;
    while (!g_ref_go) PiTestThreadYield();   /* 一起出发，最大化并发归零的窗口 */
    if (w->use_vtbl) w->obj->unk.lpVtbl->pi_release(w->obj);
    else             pi_refcounted_release(w->obj);
    w->released = 1;
}

/* C 段：紧循环里只加不减 —— 任何一次丢失的增量都会累积下来 */
static void RefIncWorkerThread(void* user_data)
{
    RefWorker* w = (RefWorker*)user_data;
    int i;
    while (!g_ref_go) PiTestThreadYield();
    for (i = 0; i < REF_INC_ROUNDS; ++i) {
        if (w->use_vtbl) w->obj->unk.lpVtbl->pi_add_ref(w->obj);
        else             pi_refcounted_add_ref(w->obj);
    }
}

static void TestConcurrentRefCount(int use_vtbl)
{
    PiTestThread threads[REF_THREADS];
    RefWorker    workers[REF_THREADS];
    unsigned     i;

    Section(use_vtbl ? "W-04/2b 并发 AddRef/Release（vtbl 槽位）"
                     : "W-04/2a 并发 AddRef/Release（帮助函数）");

    /* --- A 段：成对操作，计数必须回到基数、对象必须还活着 --- */
    {
        PiRefCountedBase* obj = (PiRefCountedBase*)calloc(1, sizeof(PiRefCountedBase));
        CHECK(obj != NULL);
        if (!obj) return;

        g_ref_destroy_calls = 0;
        pi_refcounted_init_with_destroy(obj, &s_ref_test_vtbl, &RefCountedDestroy);

        for (i = 0; i < REF_THREADS; ++i) {
            workers[i].obj      = obj;
            workers[i].use_vtbl = use_vtbl;
            workers[i].released = 0;
            CHECK_EQ_INT(PiTestThreadStart(&threads[i], &RefPairWorkerThread, &workers[i]), 0);
        }
        for (i = 0; i < REF_THREADS; ++i) PiTestThreadJoin(&threads[i]);

        CHECK_EQ_INT(obj->ref_count, 1);          /* 成对操作后回到基数 */
        CHECK_EQ_INT(g_ref_destroy_calls, 0);     /* 还没释放，绝不能销毁 */

        /* 主线程收尾：释放基数 -> 恰好销毁一次 */
        CHECK_EQ_INT(pi_refcounted_release(obj), 0);
        CHECK_EQ_INT(g_ref_destroy_calls, 1);
    }

    /* --- B 段：预置 REF_THREADS 份引用，N 条线程同时各放一份 --- */
    {
        PiRefCountedBase* obj = (PiRefCountedBase*)calloc(1, sizeof(PiRefCountedBase));
        CHECK(obj != NULL);
        if (!obj) return;

        g_ref_destroy_calls = 0;
        g_ref_go            = 0;
        pi_refcounted_init_with_destroy(obj, &s_ref_test_vtbl, &RefCountedDestroy);

        /* init 之后是 1 份；再加 REF_THREADS-1 份 -> 恰好 REF_THREADS 份，
         * 这样每条线程各放一份就会在并发里归零（不留"主线程最后放"的余地）。 */
        for (i = 1; i < REF_THREADS; ++i) pi_refcounted_add_ref(obj);
        CHECK_EQ_INT(obj->ref_count, REF_THREADS);

        for (i = 0; i < REF_THREADS; ++i) {
            workers[i].obj      = obj;
            workers[i].use_vtbl = use_vtbl;
            workers[i].released = 0;
            CHECK_EQ_INT(PiTestThreadStart(&threads[i], &RefReleaseWorkerThread, &workers[i]), 0);
        }
        PiTestThreadYield();
        g_ref_go = 1;
        for (i = 0; i < REF_THREADS; ++i) PiTestThreadJoin(&threads[i]);

        for (i = 0; i < REF_THREADS; ++i)
            CHECK_EQ_INT(workers[i].released, 1);      /* 四条线程都真的跑了 */
        CHECK_EQ_INT(g_ref_destroy_calls, 1);          /* 恰好销毁一次 */
        /* 注意：此时 obj 已被 destroy 释放，后面不许再碰它 */
    }

    /* --- C 段：只加不减的丢更新探测（非原子实现必然在这里露馅） --- */
    {
        PiRefCountedBase* obj = (PiRefCountedBase*)calloc(1, sizeof(PiRefCountedBase));
        uint32_t expected = 1u + (uint32_t)REF_INC_THREADS * (uint32_t)REF_INC_ROUNDS;
        uint32_t remaining;
        CHECK(obj != NULL);
        if (!obj) return;

        g_ref_destroy_calls = 0;
        g_ref_go            = 0;
        pi_refcounted_init_with_destroy(obj, &s_ref_test_vtbl, &RefCountedDestroy);

        for (i = 0; i < REF_INC_THREADS; ++i) {
            workers[i].obj      = obj;
            workers[i].use_vtbl = use_vtbl;
            workers[i].released = 0;
            CHECK_EQ_INT(PiTestThreadStart(&threads[i], &RefIncWorkerThread, &workers[i]), 0);
        }
        PiTestThreadYield();
        g_ref_go = 1;
        for (i = 0; i < REF_INC_THREADS; ++i) PiTestThreadJoin(&threads[i]);

        CHECK_EQ_INT(obj->ref_count, expected);   /* 分毫不差 —— 丢一次就少一份 */
        CHECK_EQ_INT(g_ref_destroy_calls, 0);

        /* 按**实际**计数收尾（断言已失败时也不能制造下溢），最后一个必须恰好
         * 触发一次 destroy */
        remaining = obj->ref_count;
        while (remaining > 0) remaining = pi_refcounted_release(obj);
        CHECK_EQ_INT(g_ref_destroy_calls, 1);
    }
}

int main(void)
{
    printf("== piplugin thread-safety tests (W-04) ==\n");

    PiTestMutexInit(&g_ref_mutex);

    TestCrossThreadPostMessage();
    TestConcurrentRefCount(0);
    TestConcurrentRefCount(1);

    PiTestMutexDestroy(&g_ref_mutex);

    printf("== checks=%u failures=%u ==\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
