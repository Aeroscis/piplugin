/*
 * piplugin tests - an APP-DEFINED host service (roadmap APP-01, 通道 B)
 *
 * The framework hands a plugin an IPiHostServices object. The framework's own
 * IIDs (IPiHostServices / IPiHostUI) are only two of the things a host may
 * want to offer: the first real app needs to expose services of its own -
 * exactly the mirror image of the framework's "app defines a protocol the
 * plugin implements" channel.
 *
 * This header is that app-defined service for the test suite: the test hosts
 * install it through pi_host_services_create_ex()'s extra-QI hook, and the
 * test plugins QueryInterface the host object for PI_TEST_IID_HOST_SERVICE.
 * It lives in one place because both sides need the identical 128-bit IID and
 * vtbl layout.
 *
 * Note what a plugin does when the host does NOT provide it: the query fails
 * with PI_E_NOINTERFACE and the plugin carries on. That is the whole point of
 * capability negotiation, and it is why the capability is declared
 * PI_CAP_OPTIONAL rather than REQUIRED.
 *
 * The IID is a random 128-bit UUID, not a small number out of the framework's
 * reserved range (data1 < 0x80000000) - see docs/design/interfaces.md §5.1.
 */
#ifndef PI_TEST_HOST_SERVICE_H
#define PI_TEST_HOST_SERVICE_H

#include "piplugin/pi_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 随机 UUID：宿主/插件之间唯一的身份，与框架保留区无关。 */
#define PI_TEST_HOST_SERVICE_IID_INIT \
    PI_GUID(0x3B7E14C9, 0x2A5D, 0x4F31, \
            0x8E, 0x77, 0x51, 0xC2, 0x9A, 0x0B, 0x6D, 0x44)

static const PiGuid PI_TEST_IID_HOST_SERVICE = PI_TEST_HOST_SERVICE_IID_INIT;

/* 插件报告"我用到了宿主自定义服务"时投递的消息码。
 * 框架保留 0x80000000 以下的自定义码区之外的约定见 pi_plugin_types.h：
 * 0x80000000 及以上留给 app / 插件自定义。 */
#define PI_TEST_MSG_HOST_SERVICE ((uint32_t)0x8002u)

typedef struct IPiTestHostServiceVtbl {
    IPiUnknownVtbl base;

    /* 人类可读的宿主名（借用，生命周期归宿主），例如 "headless-test-host"。
     * 每次调用都会在宿主侧计数：这是 ctest 用来断言"插件真的 QI 到并调用了
     * 宿主自定义服务"的证据。 */
    const char* (PI_CALL *pi_test_host_service_name)(void* this_ptr);

    /* 宿主内部计数器：它一共收到过多少条插件消息。选这个槽位是刻意的 ——
     * 该值只有宿主知道，插件能报出非零值就只能是通过本接口读到的。 */
    uint32_t (PI_CALL *pi_test_host_service_messages_seen)(void* this_ptr);
} IPiTestHostServiceVtbl;

typedef struct IPiTestHostService {
    const IPiTestHostServiceVtbl* lpVtbl;
} IPiTestHostService;

/* Inline helpers（与框架其它接口一致的 NULL 安全写法） */
static inline const char* pi_test_host_service_name(IPiTestHostService* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_test_host_service_name) return NULL;
    return self->lpVtbl->pi_test_host_service_name((void*)self);
}

static inline uint32_t pi_test_host_service_messages_seen(IPiTestHostService* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_test_host_service_messages_seen) return 0;
    return self->lpVtbl->pi_test_host_service_messages_seen((void*)self);
}

#ifdef __cplusplus
}
#endif

#endif /* PI_TEST_HOST_SERVICE_H */
