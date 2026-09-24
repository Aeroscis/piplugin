/*
 * piplugin - Host kit L0: PiPluginHostSession
 *
 * 职责（本目录 README「三层结构」里的 L0 层）：消灭宿主侧重复的"机制"代码 ——
 * 加载 / 双向能力门禁 / 实例化 / 多插件槽位 / 七步卸载序列。
 *
 * 本层不含任何 UI 决策，也不创建任何窗口：
 *   - 容器窗口由宿主自己创建并持有，通过 pi_plugin_host_session_attach_view() 告知本层；
 *     本层只记录"已 attach"，以便卸载时按正确顺序 detach；
 *   - 何时 pump（主循环 / QTimer / WM_TIMER）由宿主决定，本层只提供
 *     pi_plugin_host_session_drive_idle() 这一个"把每个活着的 view 过一遍"的机制；
 *   - 布局、样式、可见性策略、窗口数量全部归宿主。
 *
 * 依赖：只依赖框架核心（piplugin），零 GUI 依赖 —— 见同目录上级
 * host_kits/README.md 的三层纪律。
 *
 * 线程：本层不加锁，所有函数必须在宿主 GUI 线程上调用（与框架核心的
 * IPiPluginView 契约一致）。
 *
 * 版本门禁（roadmap BLK-03，已落地）：pi_plugin_host_session_load() 在 descriptor 到手后、
 * 实例化之前先过 pi_plugin_api_version_compatible()（同 major 且插件不比宿主新），
 * 不兼容直接拒绝加载；负向回归是 ctest version_gate_rejects_incompatible_plugin。
 */
#ifndef PI_PLUGIN_HOST_SESSION_H
#define PI_PLUGIN_HOST_SESSION_H

#include "piplugin/pi_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一个 session 能同时持有的插件实例数。注意这是多"插件"而非多"视图"：
 * 一个插件实例只能有一个 view（pi_plugin_get_view 为单视图接口，"一个插件多面板"
 * 是 ABI 2.0 事项 FUT-03）。 */
#define PI_PLUGIN_HOST_SESSION_MAX_SLOTS    16u
#define PI_PLUGIN_HOST_SESSION_INVALID_SLOT 0xFFFFFFFFu
#define PI_PLUGIN_HOST_SESSION_ERROR_MAX    256u

typedef struct PiPluginHostSession PiPluginHostSession;

/* 日志回调：本层把自身步骤（加载 / attach / 卸载各步）经此上报，
 * 由宿主决定写文件、写 stdout 还是丢弃 —— 机制留下，去向归宿主。
 * 在调用线程（宿主 GUI 线程）上同步执行。 */
typedef void (*PiPluginHostSessionLogProc)(void* user_data, const char* message);

/* --------------------------------------------------------------------------
 * 生命周期
 * -------------------------------------------------------------------------- */

/* 创建 session。services 会被 add-ref（调用方保留自己的引用，通常由调方
 * 在 session 销毁后释放）。失败返回 PI_E_INVALIDARG / PI_E_OUTOFMEMORY。 */
PiResult pi_plugin_host_session_create(IPiPluginHostServices* services,
                                PiPluginHostSession** out_session);

/* 销毁 session：先按七步序列卸掉所有仍活着的槽位，再释放宿主服务引用。
 * NULL 安全。 */
void pi_plugin_host_session_destroy(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 宿主生态要求（特化协议门禁 —— roadmap §1.3 通道 A）
 * -------------------------------------------------------------------------- */

/* 声明"本宿主生态要求插件必须 PROVIDES 的能力"。幂等；可在 load 之前任意次调用。
 * load/inspect 时对插件 descriptor 做门禁，不满足即拒绝（PI_E_MISSINGCAPABILITY）。
 * 这是 app 作者实现"我生态内所有插件必须符合 XXX"而无需 fork 框架的入口。 */
PiResult pi_plugin_host_session_require(PiPluginHostSession* session, const PiGuid* iid);

/* --------------------------------------------------------------------------
 * 加载
 * -------------------------------------------------------------------------- */

/* 推荐路径：加载模块 + 双向能力门禁 + 实例化 + 初始化，一次到位。
 * 成功后 out_slot 收到槽位下标；失败返回错误码，且不留下任何半成品状态
 * （已加载的模块会被就地回卷）。 */
PiResult pi_plugin_host_session_load(PiPluginHostSession* session,
                              const char* dll_path,
                              uint32_t* out_slot);

/* 分解形式（可选）：只加载模块并跑门禁，不实例化。
 * 供"实例化前按 descriptor 过滤"的宿主使用（LV2 式场景，见 headless 测试宿主）：
 * 过滤掉不合适的插件时无需付出实例化代价。成功后必须对该槽位调用
 * instantiate 或 unload。load() 就是这两个调用合起来的快捷形式。 */
PiResult pi_plugin_host_session_inspect(PiPluginHostSession* session,
                                 const char* dll_path,
                                 uint32_t* out_slot);
PiResult pi_plugin_host_session_instantiate(PiPluginHostSession* session, uint32_t slot);

/* --------------------------------------------------------------------------
 * 槽位查询
 *
 * 返回的接口指针与 descriptor 均为"借用"：所有权在 session 内部，
 * 调用方禁止 release，其生命周期止于该槽位的 unload（descriptor 止于模块卸载）。
 * -------------------------------------------------------------------------- */
uint32_t pi_plugin_host_session_count(const PiPluginHostSession* session);
int      pi_plugin_host_session_is_loaded(const PiPluginHostSession* session, uint32_t slot);

IPiPluginBase*            pi_plugin_host_session_get_plugin(PiPluginHostSession* session, uint32_t slot);
IPiPluginView*            pi_plugin_host_session_get_view(PiPluginHostSession* session, uint32_t slot);
IPiPluginService*               pi_plugin_host_session_get_service(PiPluginHostSession* session, uint32_t slot);
const PiPluginDescriptor* pi_plugin_host_session_get_descriptor(const PiPluginHostSession* session, uint32_t slot);

/* --------------------------------------------------------------------------
 * 事件（roadmap APP-06，通道 C）
 *
 * 会话替宿主把"事件 sink 的记账与生命周期"做掉，理由和七步卸载序列一样：
 * 顺序错了就是调用已卸载内存。
 *
 *   - 实例化后 QI 一次 PI_PLUGIN_IID_EVENT_SINK，命中就持有到该槽位卸载为止；
 *   - 卸载序列里**先**按 owner 退订（host_events 说的 owner 就是插件实例指针）、
 *     **再**释放 sink，两者都在 terminate / 模块卸载之前；
 *   - 宿主投递事件用 pi_plugin_host_session_deliver_event()：**投给谁、投什么**是宿主的策略，
 *     本层只保证"路由到正确的槽位、且在正确的时机存在/销毁"。
 *
 * 本层不自带队列、不决定泵点、不定义路由策略（那是宿主的，或者用可选的
 * piplugin_events 路由器）。宿主对象没有 PI_PLUGIN_IID_HOST_EVENTS 时 host_events 为 NULL：
 * 投递返回 PI_E_NOINTERFACE，订阅相关的一切都不发生，其余功能不受影响。
 * -------------------------------------------------------------------------- */

/* 该槽位的插件是否实现了 IPiPluginEventSink（1/0）。未实例化或未实现都为 0。 */
int pi_plugin_host_session_has_event_sink(const PiPluginHostSession* session, uint32_t slot);

/* 把一个事件投给该槽位插件的 sink（宿主主线程）。
 * 返回 sink 自己的返回码；槽位不存在 / 插件没有 sink / 宿主没有事件接口时返回
 * PI_E_NOINTERFACE —— 调用方据此静默跳过（这正是"未实现 sink 的插件优雅降级"）。
 * event 为 NULL 返回 PI_E_INVALIDARG。 */
PiResult pi_plugin_host_session_deliver_event(PiPluginHostSession* session, uint32_t slot,
                                       const PiPluginEvent* event);

/* 宿主提供的事件接口（借用；所有权在 session，禁止 release）。宿主没有提供时返回
 * NULL。宿主可以用它订阅/发布 —— 或者直接用自己那份路由器指针。 */
IPiPluginHostEvents* pi_plugin_host_session_get_host_events(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 嵌入（机制：attach + "已 attach"记账。容器是谁 / 在哪 / 多大 / 几个归宿主）
 * -------------------------------------------------------------------------- */
/* 把槽位的 view 嵌进 parent_window（宿主自己创建的容器）。
 * set_visible 非 0 时顺带 pi_plugin_view_set_visible(view, 1)。
 * 槽位没有 view（headless 插件）返回 PI_E_NOINTERFACE。 */
PiResult pi_plugin_host_session_attach_view(PiPluginHostSession* session, uint32_t slot,
                                     PiNativeWindow parent_window, int set_visible);

/* 每帧 pump：把 session 里每个活着的 view 过一遍 pi_plugin_on_idle()。
 * 何时调用（每帧 / 定时器）是宿主的决策，本层不持有计时器。 */
void pi_plugin_host_session_drive_idle(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 卸载（七步卸载序列内化；顺序错了就是崩溃，故不交给宿主手写）
 *
 *   1) service stop + release
 *   2) view detach + release
 *   3) plugin terminate + release
 *   4) factory release
 *   5) module unload
 *   6) 槽位清空
 *   （宿主服务对象由 pi_plugin_host_session_destroy 释放）
 * -------------------------------------------------------------------------- */
PiResult pi_plugin_host_session_unload(PiPluginHostSession* session, uint32_t slot);
void     pi_plugin_host_session_unload_all(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 诊断
 * -------------------------------------------------------------------------- */

void pi_plugin_host_session_set_logger(PiPluginHostSession* session,
                                PiPluginHostSessionLogProc log, void* user_data);

/* 诊断开关：卸载时跳过 pi_plugin_view_detach()，让插件自己的 terminate 收尾 ——
 * 复现"宿主直接丢模块"那条历史崩溃路径用。正常宿主不要打开。 */
void pi_plugin_host_session_set_skip_detach(PiPluginHostSession* session, int skip);

/* 上一次失败的可读原因（配合 pi_plugin_module_get_load_error 使用）。永不为 NULL。 */
const char* pi_plugin_host_session_last_error(const PiPluginHostSession* session);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_HOST_SESSION_H */
