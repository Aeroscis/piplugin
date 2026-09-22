/*
 * piplugin tests - the service-plugin test protocol (roadmap APP-07)
 *
 * A service plugin and the headless host that drives it have to agree on the
 * messages the plugin posts while it works, and on the option keys its
 * `pi_service_start()` expects. Both sides include this header so the strings
 * and codes exist exactly once.
 *
 * Message codes: the framework reserves codes below 0x80000000 and leaves the
 * rest to apps and plugins (pi_plugin_types.h), so these are >= 0x80000000.
 */
#ifndef PI_TEST_SERVICE_PROTOCOL_H
#define PI_TEST_SERVICE_PROTOCOL_H

#include "piplugin/pi_plugin.h"

/* poll() 报数：wparam = 第几次 tick。宿主据此断言"poll 真的在干活"。 */
#define PI_TEST_MSG_SERVICE_TICK ((uint32_t)0x8003u)

/* stop() 报数：wparam = 第几次 stop 调用。宿主据此断言 stop 的幂等性，
 * 以及卸载序列确实又调了一次 stop。 */
#define PI_TEST_MSG_SERVICE_STOP ((uint32_t)0x8004u)

/* start() 的选项键：`slot` 必填（缺了就按文档返回 PI_E_MISSINGCAPABILITY），
 * `interval` 可选（每 N 次 poll 报一次 tick，缺省 1）。 */
#define PI_TEST_SERVICE_OPTION_SLOT     "slot"
#define PI_TEST_SERVICE_OPTION_INTERVAL "interval"

#endif /* PI_TEST_SERVICE_PROTOCOL_H */
