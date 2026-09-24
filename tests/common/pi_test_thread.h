/*
 * piplugin - tests/common：极简可移植线程封装（W-01 / W-04 的并发用例共用）
 *
 * 只提供并发测试真正需要的四件事：起线程、等线程、让出时间片、一把互斥锁。
 * Windows 走 Win32（CreateThread / CRITICAL_SECTION），其余走 pthread。
 * 全部是 `static inline`：头文件即全部，不需要额外的 .c，也不会产生
 * "定义了但没用到" 的 /W4 警告（只有用到的那几个函数才会实例化）。
 *
 * 刻意不做的事：不封装条件变量、不做线程池、不隐藏平台差异。测试要的是
 * "真并发 + 零意外"，不是一套迷你线程库。
 */
#ifndef PI_PLUGIN_TEST_THREAD_H
#define PI_PLUGIN_TEST_THREAD_H

#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
    #define PI_PLUGIN_TEST_THREADS_WIN32 1
    #include <windows.h>
#else
    #define PI_PLUGIN_TEST_THREADS_WIN32 0
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
#endif

/* 线程体：void(*)(void*) —— 两个平台的入口签名都由下面的 trampoline 适配 */
typedef void (*PiPluginTestThreadFn)(void* user_data);

typedef struct PiPluginTestThreadStartData {
    PiPluginTestThreadFn fn;
    void*                user_data;
} PiPluginTestThreadStartData;

typedef struct PiPluginTestThread {
#if PI_PLUGIN_TEST_THREADS_WIN32
    HANDLE handle;
#else
    pthread_t handle;
    int       started;
#endif
} PiPluginTestThread;

typedef struct PiPluginTestMutex {
#if PI_PLUGIN_TEST_THREADS_WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t m;
#endif
} PiPluginTestMutex;

static inline void* PiPluginTestThreadTrampoline(void* param)
{
    PiPluginTestThreadStartData* start     = (PiPluginTestThreadStartData*)param;
    PiPluginTestThreadFn         fn        = start->fn;
    void*                        user_data = start->user_data;
    free(start);
    fn(user_data);
    return NULL;
}

#if PI_PLUGIN_TEST_THREADS_WIN32
/* Win32 的线程入口是 DWORD WINAPI(LPVOID)：单独包一层，避免把函数指针硬转成
 * 另一种原型（/W4 下的 C4191，语义上也是未定义行为）。 */
static inline DWORD WINAPI PiPluginTestThreadTrampolineWin32(LPVOID param)
{
    PiPluginTestThreadTrampoline(param);
    return 0;
}
#endif

/* 起线程：0 = 成功（与 pthread_create 同约定）。起始数据堆分配、由线程自己
 * 释放，所以调用方不必等"线程是否已经读到参数"。 */
static inline int PiPluginTestThreadStart(PiPluginTestThread* thread, PiPluginTestThreadFn fn, void* user_data)
{
    PiPluginTestThreadStartData* start =
        (PiPluginTestThreadStartData*)malloc(sizeof(PiPluginTestThreadStartData));
    if (!start)
    {
        return -1;
    }
    start->fn        = fn;
    start->user_data = user_data;

#if PI_PLUGIN_TEST_THREADS_WIN32
    thread->handle = CreateThread(NULL, 0, &PiPluginTestThreadTrampolineWin32, start, 0, NULL);
    if (!thread->handle)
    {
        free(start);
        return -1;
    }
    return 0;
#else
    thread->started = (pthread_create(&thread->handle, NULL,
                                      &PiPluginTestThreadTrampoline, start) == 0);
    if (!thread->started)
    {
        free(start);
        return -1;
    }
    return 0;
#endif
}

static inline void PiPluginTestThreadJoin(PiPluginTestThread* thread)
{
#if PI_PLUGIN_TEST_THREADS_WIN32
    if (thread->handle)
    {
        WaitForSingleObject(thread->handle, INFINITE);
        CloseHandle(thread->handle);
        thread->handle = NULL;
    }
#else
    if (thread->started)
    {
        pthread_join(thread->handle, NULL);
        thread->started = 0;
    }
#endif
}

/* 让出时间片（不睡）：给调度器一个切换到别的线程的机会 */
static inline void PiPluginTestThreadYield(void)
{
#if PI_PLUGIN_TEST_THREADS_WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

/* 真睡：把"写入 → 读取"这类窗口拉开，避免测试靠调度运气 */
static inline void PiPluginTestThreadSleepMs(int ms)
{
#if PI_PLUGIN_TEST_THREADS_WIN32
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;
        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
#endif
}

static inline void PiPluginTestMutexInit(PiPluginTestMutex* mutex)
{
#if PI_PLUGIN_TEST_THREADS_WIN32
    InitializeCriticalSection(&mutex->cs);
#else
    pthread_mutex_init(&mutex->m, NULL);
#endif
}

static inline void PiPluginTestMutexDestroy(PiPluginTestMutex* mutex)
{
#if PI_PLUGIN_TEST_THREADS_WIN32
    DeleteCriticalSection(&mutex->cs);
#else
    pthread_mutex_destroy(&mutex->m);
#endif
}

static inline void PiPluginTestMutexLock(PiPluginTestMutex* mutex)
{
#if PI_PLUGIN_TEST_THREADS_WIN32
    EnterCriticalSection(&mutex->cs);
#else
    pthread_mutex_lock(&mutex->m);
#endif
}

static inline void PiPluginTestMutexUnlock(PiPluginTestMutex* mutex)
{
#if PI_PLUGIN_TEST_THREADS_WIN32
    LeaveCriticalSection(&mutex->cs);
#else
    pthread_mutex_unlock(&mutex->m);
#endif
}

#endif /* PI_PLUGIN_TEST_THREAD_H */
