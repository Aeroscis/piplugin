/*
 * piplugin - Host kit L0: PiPluginHostSession
 *
 * 职责（本目录 README「三层结构」里的 L0 层）：消灭宿主侧重复的"机制"代码 ——
 * 加载 / 双向能力门禁 / 实例化 / 多插件槽位 / 七步卸载序列。
 *
 * 本层不含任何 UI 决策，也不创建任何窗口：
 *   - 容器窗口由宿主自己创建并持有，通过 pi_plugin_host_session_attach_view() 告知本层；
 *     本层只记录"哪个视图 attach 到了哪个窗口"，以便卸载时按正确顺序 detach；
 *   - 何时 pump（主循环 / QTimer / WM_TIMER）由宿主决定，本层只提供
 *     pi_plugin_host_session_drive_idle() 这一个"把每个已创建的 view 过一遍"的机制；
 *   - 布局、样式、可见性策略、**窗口数量与其上限**全部归宿主。
 *
 * 视图（0.6 起一个槽位可以有 N 个）：
 *   - 插件实现 IPiPluginViewProvider，本层在实例化后 QI 一次并持有到卸载；
 *   - 视图身份是插件给的**字符串 id**，不是下标。下标只是"当前这次枚举"的游标，
 *     插件关掉中间一个视图时下标会整体位移，宿主就会把 A 的内容画进 B 的窗口；
 *   - 枚举（views）直接转发给 provider，**不缓存**：集合是动态的，宿主每帧重读；
 *   - 本层只记住"宿主已经取过引用"的那些视图（get_view / attach_view 问到的），
 *     并在卸载时按顺序 detach + release。窗口成组、标签页、只取第一个等等都是
 *     宿主策略；框架与本层都不提供 max_views。
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
 * 每个槽位可以有任意多个视图（受下面的表容量限制），视图数量由插件决定。 */
#define PI_PLUGIN_HOST_SESSION_MAX_SLOTS    16u
#define PI_PLUGIN_HOST_SESSION_INVALID_SLOT 0xFFFFFFFFu
#define PI_PLUGIN_HOST_SESSION_ERROR_MAX    256u

/* 每个槽位最多记住多少个"宿主已经取过引用"的视图。
 *
 * 这不是框架策略：框架的 provider vtbl 里没有 max_views，插件想给几个就给几个。
 * 本层需要一个表的上界而已 —— 宿主想要别的上界就自己维护一张表（四行），
 * 或者按自己的需求改这个上限重编 kit。超过上限时新视图取不到，last_error 会说明。 */
#define PI_PLUGIN_HOST_SESSION_MAX_VIEWS_PER_SLOT 16u

typedef struct PiPluginHostSession PiPluginHostSession;

/* 日志回调：本层把自身步骤（加载 / attach / 卸载各步）经此上报，
 * 由宿主决定写文件、写 stdout 还是丢弃 —— 机制留下，去向归宿主。
 * 在调用线程（宿主 GUI 线程）上同步执行。 */
typedef void (*PiPluginHostSessionLogProc)(void* user_data, char const* message);

/* --------------------------------------------------------------------------
 * 生命周期
 * -------------------------------------------------------------------------- */

/* 创建 session。services 会被 add-ref（调用方保留自己的引用，通常由调方
 * 在 session 销毁后释放）。失败返回 PI_E_INVALIDARG / PI_E_OUTOFMEMORY。 */
PiResult pi_plugin_host_session_create(IPiPluginHostServices* services,
                                       PiPluginHostSession**  out_session);

/* 销毁 session：先按七步序列卸掉所有仍活着的槽位，再释放宿主服务引用。
 * NULL 安全。 */
void pi_plugin_host_session_destroy(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 宿主生态要求（特化协议门禁 —— roadmap §1.3 通道 A）
 * -------------------------------------------------------------------------- */

/* 声明"本宿主生态要求插件必须 PROVIDES 的能力"。幂等；可在 load 之前任意次调用。
 * load/inspect 时对插件 descriptor 做门禁，不满足即拒绝（PI_E_MISSINGCAPABILITY）。
 * 这是 app 作者实现"我生态内所有插件必须符合 XXX"而无需 fork 框架的入口。 */
PiResult pi_plugin_host_session_require(PiPluginHostSession* session, PiGuid const* iid);

/* --------------------------------------------------------------------------
 * 加载
 * -------------------------------------------------------------------------- */

/* 推荐路径：加载模块 + 双向能力门禁 + 实例化 + 初始化，一次到位。
 * 成功后 out_slot 收到槽位下标；失败返回错误码，且不留下任何半成品状态
 * （已加载的模块会被就地回卷）。 */
PiResult pi_plugin_host_session_load(PiPluginHostSession* session,
                                     char const*          dll_path,
                                     uint32_t*            out_slot);

/* 分解形式（可选）：只加载模块并跑门禁，不实例化。
 * 供"实例化前按 descriptor 过滤"的宿主使用（LV2 式场景，见 headless 测试宿主）：
 * 过滤掉不合适的插件时无需付出实例化代价。成功后必须对该槽位调用
 * instantiate 或 unload。load() 就是这两个调用合起来的快捷形式。 */
PiResult pi_plugin_host_session_inspect(PiPluginHostSession* session,
                                        char const*          dll_path,
                                        uint32_t*            out_slot);
PiResult pi_plugin_host_session_instantiate(PiPluginHostSession* session, uint32_t slot);

/* --------------------------------------------------------------------------
 * 槽位查询
 *
 * 返回的接口指针与 descriptor 均为"借用"：所有权在 session 内部，
 * 调用方禁止 release，其生命周期止于该槽位的 unload（descriptor 止于模块卸载）。
 * -------------------------------------------------------------------------- */
uint32_t pi_plugin_host_session_count(PiPluginHostSession const* session);
int      pi_plugin_host_session_is_loaded(PiPluginHostSession const* session, uint32_t slot);

IPiPluginBase*            pi_plugin_host_session_get_plugin(PiPluginHostSession* session, uint32_t slot);
IPiPluginService*         pi_plugin_host_session_get_service(PiPluginHostSession* session, uint32_t slot);
PiPluginDescriptor const* pi_plugin_host_session_get_descriptor(PiPluginHostSession const* session, uint32_t slot);

/* --------------------------------------------------------------------------
 * 视图：枚举 / 取用 / 嵌入 / pump
 *
 * 一切"几个窗口、怎么摆、要不要限制成一个"都是宿主的决策；本层只提供机制。
 * -------------------------------------------------------------------------- */

/* 枚举该槽位插件**此刻**存在的视图：直接转发给 IPiPluginViewProvider，不缓存。
 * 返回的数组与 id 都是插件的借用数据（零分配），有效期到下一次 views() 调用为止；
 * 宿主不得 free，也不得跨帧持有。集合会变（新建/关闭文档），宿主每帧重读即可。
 * 插件没有界面（未暴露 provider）→ PI_E_NOINTERFACE，两个出参都被清空。 */
PiResult pi_plugin_host_session_views(PiPluginHostSession* session, uint32_t slot,
                                      PiPluginViewInfo const** out_views, uint32_t* out_count);

/* 该槽位的 provider（借用；禁止 release）。headless 插件为 NULL。
 * 宿主想自己实现窗口策略（例如"只接受单视图"：读 count，超限就拒载）时用它；
 * 本层其余视图函数已经替常用场景转发好了。 */
IPiPluginViewProvider* pi_plugin_host_session_get_view_provider(PiPluginHostSession* session,
                                                                uint32_t             slot);

/* 按 id 取视图（借用指针；禁止 release，生命周期止于该槽位 unload）。
 *
 * 第一次问到某个 id 时向 provider 取一份引用并记住，之后再问同一个 id 返回同一个
 * 指针 —— 直到该槽位卸载。id 不存在、插件没有界面、表满都返回 NULL，
 * 具体原因见 pi_plugin_host_session_last_error()。
 *
 * 注意：provider 的枚举说某个 id 没了，本层**不会**立刻释放它（宿主可能还开着
 * 那个窗口，释放时机得宿主自己决定）；本层持有的引用统一在卸载序列里放掉。 */
IPiPluginView* pi_plugin_host_session_get_view(PiPluginHostSession* session, uint32_t slot,
                                               char const* view_id);

/* 把视图嵌进 parent_window（宿主自己创建的容器）。
 * set_visible 非 0 时顺带 pi_plugin_view_set_visible(view, 1)。
 * 视图不存在 → PI_E_NOTFOUND；插件没有界面 → PI_E_NOINTERFACE；
 * 该视图已经 attach 过 → PI_E_UNEXPECTED（一个视图只在一个容器里）。 */
PiResult pi_plugin_host_session_attach_view(PiPluginHostSession* session, uint32_t slot,
                                            char const* view_id, PiNativeWindow parent_window,
                                            int set_visible);

/* 从容器里摘下来，但**保留**引用 —— 宿主可以把它 attach 到别处（容器切换）。
 * 未 attach 的视图是无操作；id 不存在返回 PI_E_NOTFOUND。 */
PiResult pi_plugin_host_session_detach_view(PiPluginHostSession* session, uint32_t slot,
                                            char const* view_id);

/* 每帧 pump：把 session 里每个已创建的视图过一遍 pi_plugin_on_idle()。
 * 何时调用（每帧 / 定时器）是宿主的决策，本层不持有计时器。 */
void pi_plugin_host_session_drive_idle(PiPluginHostSession* session);

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
int pi_plugin_host_session_has_event_sink(PiPluginHostSession const* session, uint32_t slot);

/* 把一个事件投给该槽位插件的 sink（宿主主线程）。
 * 返回 sink 自己的返回码；槽位不存在 / 插件没有 sink / 宿主没有事件接口时返回
 * PI_E_NOINTERFACE —— 调用方据此静默跳过（这正是"未实现 sink 的插件优雅降级"）。
 * event 为 NULL 返回 PI_E_INVALIDARG。 */
PiResult pi_plugin_host_session_deliver_event(PiPluginHostSession* session, uint32_t slot,
                                              PiPluginEvent const* event);

/* 宿主提供的事件接口（借用；所有权在 session，禁止 release）。宿主没有提供时返回
 * NULL。宿主可以用它订阅/发布 —— 或者直接用自己那份路由器指针。 */
IPiPluginHostEvents* pi_plugin_host_session_get_host_events(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 卸载（七步卸载序列内化；顺序错了就是崩溃，故不交给宿主手写）
 *
 *   1) 每个已创建的视图：detach（若 attached）+ release，id 副本一并释放
 *   2) provider release
 *   3) 事件 sink：按 owner 退订 + release（APP-06）
 *   4) service stop + release
 *   5) plugin terminate + release
 *   6) factory release
 *   7) module unload
 *   8) 槽位清空
 *
 * 视图那一步按 N 个展开，且**必须**先于模块卸载：控件的析构函数在插件模块里。
 * 第 3 步不是"七步"原文里的内容，是 APP-06 加进来的事件收尾（原顺序里插在
 * 视图之后、服务之前，两件事都必须在插件代码还映射着的时候做完）。
 * （宿主服务对象由 pi_plugin_host_session_destroy 释放）
 * -------------------------------------------------------------------------- */
PiResult pi_plugin_host_session_unload(PiPluginHostSession* session, uint32_t slot);
void     pi_plugin_host_session_unload_all(PiPluginHostSession* session);

/* --------------------------------------------------------------------------
 * 诊断
 * -------------------------------------------------------------------------- */

void pi_plugin_host_session_set_logger(PiPluginHostSession*       session,
                                       PiPluginHostSessionLogProc log, void* user_data);

/* 诊断开关：卸载时跳过 pi_plugin_view_detach()，让插件自己的 terminate 收尾 ——
 * 复现"宿主直接丢模块"那条历史崩溃路径用。正常宿主不要打开。 */
void pi_plugin_host_session_set_skip_detach(PiPluginHostSession* session, int skip);

/* 上一次失败的可读原因（配合 pi_plugin_module_get_load_error 使用）。永不为 NULL。 */
char const* pi_plugin_host_session_last_error(PiPluginHostSession const* session);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_HOST_SESSION_H */
