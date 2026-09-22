/*
 * pipluginframework - Host kit L0: PiPluginHostSession
 *
 * 职责（release-roadmap.md §1.1 的 L0 层）：消灭宿主侧重复的"机制"代码 ——
 * 加载 / 双向能力门禁 / 实例化 / 多插件槽位 / 七步卸载序列。
 *
 * 本层不含任何 UI 决策，也不创建任何窗口：
 *   - 容器窗口由宿主自己创建并持有，通过 pi_host_session_attach_view() 告知本层；
 *     本层只记录"已 attach"，以便卸载时按正确顺序 detach；
 *   - 何时 pump（主循环 / QTimer / WM_TIMER）由宿主决定，本层只提供
 *     pi_host_session_drive_idle() 这一个"把每个活着的 view 过一遍"的机制；
 *   - 布局、样式、可见性策略、窗口数量全部归宿主。
 *
 * 依赖：只依赖框架核心（pipluginframework），零 GUI 依赖 —— 见同目录上级
 * host_kits/README.md 的三层纪律。
 *
 * 线程：本层不加锁，所有函数必须在宿主 GUI 线程上调用（与框架核心的
 * IPiPluginView 契约一致）。
 *
 * 变更提醒（roadmap §2 BLK-03）：descriptor 的 api_version 运行时协商尚未落地
 * （pi_api_version_compatible 还不存在），load 门禁里已留好插入点。
 */
#ifndef PI_HOST_SESSION_H
#define PI_HOST_SESSION_H

#include "pipluginframework/pi_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一个 session 能同时持有的插件实例数。注意这是多"插件"而非多"视图"：
 * 一个插件实例只能有一个 view（pi_get_view 为单视图接口，"一个插件多面板"
 * 是 ABI 2.0 事项 FUT-03）。 */
#define PI_HOST_SESSION_MAX_SLOTS    16u
#define PI_HOST_SESSION_INVALID_SLOT 0xFFFFFFFFu
#define PI_HOST_SESSION_ERROR_MAX    256u

typedef struct PiPluginHostSession PiPluginHostSession;

/* 日志回调：本层把自身步骤（加载 / attach / 卸载各步）经此上报，
 * 由宿主决定写文件、写 stdout 还是丢弃 —— 机制留下，去向归宿主。
 * 在调用线程（宿主 GUI 线程）上同步执行。 */
typedef void (*PiHostSessionLogProc)(void* user_data, const char* message);

/* --------------------------------------------------------------------------
 * 生命周期
 * -------------------------------------------------------------------------- */

/* 创建 session。services 会被 add-ref（调用方保留自己的引用，通常由调方
 * 在 session 销毁后释放）。失败返回 PI_E_INVALIDARG / PI_E_OUTOFMEMORY。 */
PiResult pi_host_session_create(IPiHostServices* services,
                                PiPluginHostSession** out_session);

/* 销毁 session：先按七步序列卸掉所有仍活着的槽位，再释放宿主服务引用。
 * NULL 安全。 */
void pi_host_session_destroy(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 宿主生态要求（特化协议门禁 —— roadmap §1.3 通道 A）
 * -------------------------------------------------------------------------- */

/* 声明"本宿主生态要求插件必须 PROVIDES 的能力"。幂等；可在 load 之前任意次调用。
 * load/inspect 时对插件 descriptor 做门禁，不满足即拒绝（PI_E_MISSINGCAPABILITY）。
 * 这是 app 作者实现"我生态内所有插件必须符合 XXX"而无需 fork 框架的入口。 */
PiResult pi_host_session_require(PiPluginHostSession* session, const PiGuid* iid);

/* --------------------------------------------------------------------------
 * 加载
 * -------------------------------------------------------------------------- */

/* 推荐路径：加载模块 + 双向能力门禁 + 实例化 + 初始化，一次到位。
 * 成功后 out_slot 收到槽位下标；失败返回错误码，且不留下任何半成品状态
 * （已加载的模块会被就地回卷）。 */
PiResult pi_host_session_load(PiPluginHostSession* session,
                              const char* dll_path,
                              uint32_t* out_slot);

/* 分解形式（可选）：只加载模块并跑门禁，不实例化。
 * 供"实例化前按 descriptor 过滤"的宿主使用（LV2 式场景，见 headless 测试宿主）：
 * 过滤掉不合适的插件时无需付出实例化代价。成功后必须对该槽位调用
 * instantiate 或 unload。load() 就是这两个调用合起来的快捷形式。 */
PiResult pi_host_session_inspect(PiPluginHostSession* session,
                                 const char* dll_path,
                                 uint32_t* out_slot);
PiResult pi_host_session_instantiate(PiPluginHostSession* session, uint32_t slot);

/* --------------------------------------------------------------------------
 * 槽位查询
 *
 * 返回的接口指针与 descriptor 均为"借用"：所有权在 session 内部，
 * 调用方禁止 release，其生命周期止于该槽位的 unload（descriptor 止于模块卸载）。
 * -------------------------------------------------------------------------- */
uint32_t pi_host_session_count(const PiPluginHostSession* session);
int      pi_host_session_is_loaded(const PiPluginHostSession* session, uint32_t slot);

IPiPluginBase*            pi_host_session_get_plugin(PiPluginHostSession* session, uint32_t slot);
IPiPluginView*            pi_host_session_get_view(PiPluginHostSession* session, uint32_t slot);
IPiService*               pi_host_session_get_service(PiPluginHostSession* session, uint32_t slot);
const PiPluginDescriptor* pi_host_session_get_descriptor(const PiPluginHostSession* session, uint32_t slot);

/* --------------------------------------------------------------------------
 * 嵌入（机制：attach + "已 attach"记账。容器是谁 / 在哪 / 多大 / 几个归宿主）
 * -------------------------------------------------------------------------- */

/* 把槽位的 view 嵌进 parent_window（宿主自己创建的容器）。
 * set_visible 非 0 时顺带 pi_view_set_visible(view, 1)。
 * 槽位没有 view（headless 插件）返回 PI_E_NOINTERFACE。 */
PiResult pi_host_session_attach_view(PiPluginHostSession* session, uint32_t slot,
                                     PiNativeWindow parent_window, int set_visible);

/* 每帧 pump：把 session 里每个活着的 view 过一遍 pi_on_idle()。
 * 何时调用（每帧 / 定时器）是宿主的决策，本层不持有计时器。 */
void pi_host_session_drive_idle(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 卸载（七步卸载序列内化；顺序错了就是崩溃，故不交给宿主手写）
 *
 *   1) service stop + release
 *   2) view detach + release
 *   3) plugin terminate + release
 *   4) factory release
 *   5) module unload
 *   6) 槽位清空
 *   （宿主服务对象由 pi_host_session_destroy 释放）
 * -------------------------------------------------------------------------- */
PiResult pi_host_session_unload(PiPluginHostSession* session, uint32_t slot);
void     pi_host_session_unload_all(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 诊断
 * -------------------------------------------------------------------------- */

void pi_host_session_set_logger(PiPluginHostSession* session,
                                PiHostSessionLogProc log, void* user_data);

/* 诊断开关：卸载时跳过 pi_view_detach()，让插件自己的 terminate 收尾 ——
 * 复现"宿主直接丢模块"那条历史崩溃路径用。正常宿主不要打开。 */
void pi_host_session_set_skip_detach(PiPluginHostSession* session, int skip);

/* 上一次失败的可读原因（配合 pi_module_get_load_error 使用）。永不为 NULL。 */
const char* pi_host_session_last_error(const PiPluginHostSession* session);

#ifdef __cplusplus
}
#endif

#endif /* PI_HOST_SESSION_H */
