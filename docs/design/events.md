# 事件机制 mini-RFC（roadmap APP-06，通道 C 补全）

> 状态：**已评审通过，并已按 §7 的决定实现（API 0.4）**。
> 实现记录与草案的差异见 §8；本文保留草案与取舍过程，作为"为什么是现在这个样子"的依据。
> 相关代码：`include/piplugin/pi_plugin_events.h`（接口）、
> `host_kits/events/pi_event_router.{h,c}`（可选路由糖）、
> `host_kits/core/pi_host_session.{h,c}`（sink 记账 + 卸载退订）、
> `tests/test_plugin_events/` 与 `tests/test_host_events/`（验收）。
> 相关文档：`docs/design/interfaces.md` §2.7/§2.8、`docs/design/architecture.md`、
> `docs/tutorial/write-{host,plugin}.md`、`docs/todo/framework.md` #2、`docs/release-roadmap.md` APP-06 / FUT-05

## 1. 问题

框架今天只有一条单向、无结构、轮询式的通道：

| 方向 | 现状 | 限制 |
|---|---|---|
| 插件 → 宿主 | `pi_host_post_message(msg, wparam, lparam)` | 只有 3 个整数，没有主题、没有结构化负载、无法路由到"关心这件事的人" |
| 宿主 → 插件 | 只有 `pi_on_idle()` / `pi_on_resize()` 这类**轮询式视图回调** | 宿主想说一句话，只能等插件下一帧来问；插件没有 view（headless service 插件）时连这点都没有 |

后果（都是"第一个应用会立刻撞上"的那类）：

1. 宿主无法**主动通知**插件（配置变更、许可证到期、网络断开、另一个插件改了共享状态）；
2. 插件之间的**间接协作**没有通道（A 插件发的事件没法被 B 插件订阅，除非宿主为每对插件手写胶水）；
3. `msg`/`wparam`/`lparam` 表达力不足，app 只能自己定义编码约定，无法被框架看见、无法跨插件复用（`docs/todo/framework.md` #2 要求 glib signals 式的命名订阅）；
4. `FUT-05`（跨进程插件代理）要求"事件可序列化"，而 `wparam` 里的指针天生不可序列化。

## 2. 目标与非目标

**目标**

- G1 双向：宿主 → 插件（投递）、插件 → 宿主（发布）；
- G2 结构化：事件有**类型 + 命名主题 + 键值负载**，宿主可路由、可日志、可序列化；
- G3 能力协商：宿主/插件任一侧不实现都能优雅降级（LV2 风格：QI 探测失败即跳过），
  与 `IPiHostUI` / `IPiService` 完全同构；
- G4 **不改任何现有 vtbl**（COM 规则：已发布接口不可变），只新增 IID 与新接口；
- G5 线程语义**显式**，不留"看情况"；
- G6 不为 1.0 之后留坑：数据结构可序列化（FUT-05）、topic 命名有保留区（BLK-04 规则的延续）。

**非目标（本期不做，明确写下来）**

- N1 不做跨进程/网络传输（FUT-05）——但结构必须是可序列化的；
- N2 不做持久化、不做可靠投递/重传/确认（事件是**尽力而为**的瞬时通知）；
- N3 不做请求/响应（RPC）语义——若将来要，属于新接口（`IPiEventQuery`），不塞进本期；
- N4 不做沙箱/权限（FUT-06）；
- N5 不给 `pi_host_post_message` 加语义，也不废弃它：它继续是"最原始的拉杆"，事件是结构化通道。

## 3. 方案空间与取舍

### 3.1 主题（topic）用什么标识

| 选项 | 优点 | 缺点 |
|---|---|---|
| **A. UTF-8 字符串**（建议） | 可读、可日志、可 grep、可序列化；与 APP-04 的 properties key 同构（`pi.` 保留区 + app 自有前缀） | 拼写错误要到运行时才发现；没有编译期标识 |
| B. 128 位 GUID | 与 IID 同构、机器稳定、无拼写问题 | 不可读、日志里全是十六进制、app 侧要自己维护 GUID 表 |
| C. `uint32_t` 数字 | 最省 | 与 `msg` 一样回到"自己定编码约定"，正是要被替代的东西 |

**建议 A**，并沿用一条与 BLK-04 同精神的规则：

- `pi.` 前缀**框架保留**（框架将来定义的事件，如 `pi.plugin.unloaded`）；
- app / 插件使用自有前缀（`com.example.thing.changed`）；
- 比较按字节、大小写敏感（与 `pi_descriptor_find_property` 一致）。

> 若评审更看重"机器稳定标识"，可保留字符串为主、**另加**一个可选的
> `const PiGuid* topic_id`（两者都有时以 topic_id 为准）。本期建议不引入，避免两套真相。

### 3.2 负载（payload）怎么表达

| 选项 | 优点 | 缺点 |
|---|---|---|
| **A. 复用 `PiPluginProperty`（键值字符串）**（建议，roadmap 原案） | 零新类型；天然可序列化（FUT-05）；人能读；APP-04 刚落地 | 数字/二进制要编码成文本；大 blob 不合适 |
| B. 键值 + `const void* data; size_t size` 二进制块 | 支持大块数据/图像 | 生命周期与线程语义立刻复杂化；跨进程要额外定格式 |
| C. 强类型 union | 类型安全 | 每加一个事件类型都要动框架（与"app 自由扩展"冲突） |

**建议 A**（二进制留给"将来真需要时新增一个字段/接口"）。约定：
字符串借用，**仅在本次投递/发布调用期间有效**；宿主若排队，必须自己拷贝。

### 3.3 `type` 字段的用途

`type` 是**框架分类的"事件种类"**，与 `topic`（app 语义）正交：

```c
#define PI_EVENT_NOTIFY     0x00000001u  /* 通知：发生了什么（默认） */
#define PI_EVENT_REQUEST    0x00000002u  /* 请求：希望对方做点什么（仍不承诺回应） */
/* < 0x80000000 保留给框架；>= 0x80000000 由 app / 插件自定义（与 msg 码同规则） */
```

本期只定义 `NOTIFY`/`REQUEST` 两个取值 + 自定义区间。评审若认为"`type` 现在是多余的"，
也可以退化为只保留 `topic`（更小），但那会失去"框架级语义 vs app 语义"的分层。

### 3.4 投递线程（G5，必须显式）

| 选项 | 优点 | 缺点 |
|---|---|---|
| **A. 宿主 marshal 到自己的主线程**（建议，roadmap 建议） | 插件侧 sink 不需要任何锁；与 GUI/句柄线程模型一致（Win32 父子窗口同线程的教训）；与 `pi_host_post_message` 的"宿主决定 marshal"完全对称 | 宿主必须实现队列；投递延迟到下一次泵 |
| B. 在发布者线程上同步回调 | 实现最简单、时序直观 | 插件必须线程安全；宿主线程可能被插件线程间接阻塞（本项目已经因为跨线程 Qt 调用踩过死锁，见 `adapters/qt/README.md`） |
| C. 由插件在 sink 上申报线程要求 | 最灵活 | 接口复杂；每个宿主都要实现 marshal |

**建议 A**，并写死三条契约：

1. 宿主保证 `pi_event_deliver()` **在宿主自己的主/UI 线程**上被调用（host kit 的
   `pi_host_session_drive_idle()` 是天然泵点，但泵的时机仍归宿主）；
2. 因此插件 sink 里可以直接操作 UI/句柄，**不需要锁**；
3. 宿主可以丢弃、合并、批量投递（尽力而为），但**不得**在插件 `pi_terminate()` 之后再投递。

### 3.5 订阅模型

- **插件 → 宿主发布**：`pi_host_events_publish()`，由宿主路由；
- **宿主/插件 → 插件投递**：宿主在泵点调用该插件 sink 的 `pi_event_deliver()`；
- **谁是订阅者**：三个可能 —— 宿主自己的代码、其它插件、以及"框架"（日志/调试）。
  为了不把路由策略写死，`IPiHostEvents` 只提供 **publish/subscribe/unsubscribe** 三件事，
  由宿主决定路由策略（广播 / 按 topic 定向 / 只喂自己）。

**可选糖**：提供一个很小的 `PiEventRouter`（静态库 `piplugin_events`，非必须链接），
实现"topic 前缀匹配的订阅表 + 有界队列 + 泵点 flush"，让不想自己写路由的宿主一行接入。
不放进核心库，保持核心零依赖与最小。

### 3.6 交付语义（明确写下来，避免误解）

- **尽力而为**：无重传、无确认、无顺序跨发布者保证；同一发布者 FIFO 是推荐的实现，
  但不写进契约；
- **有界队列**：队列满时宿主**丢弃最旧**或**丢弃最新**由宿主决定（日志里必须体现），
  框架不定义；插件不能依赖"一定能收到"；
- **重入**：在 `pi_event_deliver()` 里再 `publish()` 是**允许**的（入队，不在回调里递归投递），
  但要写进契约（否则插件作者会以为可以同步拿到结果）；
- **生命周期**：`PiEvent` 与其 payload 只在调用期间有效（借用）。

## 4. 建议的 API 草案

新头 `include/piplugin/pi_plugin_events.h`（新增，**不改任何现有头与 vtbl**）：

```c
/* 新 IID（框架保留区 data1 < 0x80000000，紧跟 SERVICE 的 0x20 之后）：
 *   PI_IID_EVENT_SINK   data1 = 0x00000030   （插件可选实现）
 *   PI_IID_HOST_EVENTS  data1 = 0x00000031   （宿主可选提供）
 */

typedef struct PiEvent {
    uint32_t                 type;          /* PI_EVENT_* 或自定义（>= 0x80000000） */
    const char*              topic;         /* UTF-8；`pi.` 保留给框架 */
    const PiPluginProperty*  payload;       /* 可 NULL；键值对（借用，仅本次调用有效） */
    uint32_t                 payload_count;
    const PiGuid*            origin;        /* 发布者 class GUID；宿主自身发布时可为 NULL */
} PiEvent;

/* ---- 插件侧：宿主投递事件给它 ---- */
typedef struct IPiEventSinkVtbl {
    IPiUnknownVtbl base;
    /* 宿主主线程调用；返回 PI_OK / PI_E_NOTIMPL（不关心该 topic）/ PI_FAIL。
     * 宿主不得因返回值而重试或报错（事件是尽力而为）。 */
    PiResult (PI_CALL *pi_event_deliver)(void* this_ptr, const PiEvent* event);
} IPiEventSinkVtbl;
/* + inline 帮助 pi_event_deliver()，NULL 安全（无 sink -> PI_E_NOINTERFACE） */

/* ---- 宿主侧：插件发布 / 订阅 ---- */
typedef void (*PiEventCallback)(void* user_data, const PiEvent* event);

typedef struct IPiHostEventsVtbl {
    IPiUnknownVtbl base;
    /* 插件发布；host 负责路由/入队/丢弃。origin 由宿主填入（发布者不便自称）。 */
    PiResult (PI_CALL *pi_host_events_publish)(void* this_ptr, const PiEvent* event);
    /* 订阅一个 topic（精确匹配本期；前缀/通配留给将来）。owner 是生命周期令牌
     * （插件传自己的实例指针；NULL = 宿主自己）。回调在宿主主线程执行。 */
    PiResult (PI_CALL *pi_host_events_subscribe)(void* this_ptr, const char* topic,
                                                 void* owner, PiEventCallback cb,
                                                 void* user_data,
                                                 uint32_t* out_subscription);
    PiResult (PI_CALL *pi_host_events_unsubscribe)(void* this_ptr, uint32_t subscription);
    /* "这个 owner 要走了"：摘掉它的全部订阅。host kit 在每个槽位卸载时调用。 */
    PiResult (PI_CALL *pi_host_events_drop_owner)(void* this_ptr, void* owner);
} IPiHostEventsVtbl;
```

可选糖（`host_kits/events/`，静态库 `piplugin_events`，宿主可不用它）：`PiEventRouter`
实现上面这套接口 —— 订阅表 + 有界队列（默认 256，满则丢新并计数）+ `pi_event_router_pump()`
+ `pi_event_router_stats()`，并直接提供 `pi_event_router_extra_qi()` 作为
`pi_host_services_create_ex()` 的钩子（于是插件 QI 宿主对象就能拿到它，见 §8.2）。

宿主 kit L0 侧只加三件事（机制，不含策略）：`pi_host_session_has_event_sink()`、
`pi_host_session_deliver_event()`（投给某槽位 —— 投什么、投给谁是宿主的策略）、
`pi_host_session_get_host_events()`（借用），外加卸载序列里的"退订 + 释放 sink"两步。

既有机制如何"自动"覆盖需求（无需新 API）：

- **能力声明**：插件 `PROVIDES PI_IID_EVENT_SINK`、`OPTIONAL PI_IID_HOST_EVENTS`；
  宿主用既有的 `pi_host_session_require(&PI_IID_EVENT_SINK)` 就能实现
  "我生态内所有插件必须能被通知"；
- **优雅降级**：宿主 QI 插件要 sink，失败就跳过（不报错）；
  插件 QI 宿主 `PI_IID_HOST_EVENTS` 失败就用 `post_message`/轮询兜底；
- **无 host kit 的宿主**：直接用 `pi_iunknown_query_interface` 做同样的事。

## 5. 最小闭环（roadmap 的验收）

```
宿主                                  插件
────                                  ────
QI(plugin, PI_IID_EVENT_SINK) ───────► 提供 sink（或返回 NOINTERFACE）
  ◄─────────────────────────────────── PI_OK / 无
pi_event_deliver({type=REQUEST,
                  topic="com.example.refresh"})   ──►  插件在 sink 里处理
                                                        插件 publish({"com.example.progress",
                                                                      {"percent","42"}})
  ◄── pi_host_events_publish(...)  宿主路由：自己处理 / 转投给别的插件 sink
```

测试计划（实现阶段）：

1. `tests/test_plugin_events/`：插件 `PROVIDES PI_IID_EVENT_SINK`（记录收到的事件并回发），
   并在 `initialize` 后 QI 宿主 `IPiHostEvents` 发布一条事件；
2. headless 测试宿主实现 `IPiHostEvents`（用一个最小路由 + 队列），
   在泵点把事件投给 sink，断言：双向都到、topic/payload 正确、顺序与丢弃策略符合契约；
3. 负向：**不实现 sink 的插件**（现有的 imgui/qt 测试插件）被投递时，宿主 QI 失败即跳过，
   宿主与插件都不出错（`PI_E_NOINTERFACE` 路径）；
4. ctest 只用退出码 + 一行可 grep 的证据行（沿用 APP-01/APP-07 的写法）。

## 6. 兼容性与工作量

- **不改任何现有 vtbl**（G4）：`IPiEventSink` / `IPiHostEvents` 是两个全新接口；
- **数据面**：`PiEvent` 是新类型，不动 `PiPluginDescriptor`；
- **版本**：按 pre-1.0 政策，新增接口 = minor 前进一位 → `PI_PLUGIN_API_VERSION` 0.3 → **0.4**
  （0.3 已被 APP-04 的 descriptor 追加占用）；单测的版本 tripwire 会失败一次（设计如此）；
- **涉及**（预估）：`include/piplugin/pi_plugin_events.h`（新）、
  `include/piplugin/pi_plugin.h`（加一行 include）、`src/pi_plugin_unknown.c`（两个新 IID）、
  `pi_event_router`（可选静态库，`src/` 或 `host_kits/`）、测试宿主/插件各一侧、
  `docs/design/interfaces.md`、`CHANGELOG.md`；
- **不做**：跨进程、RPC、持久化、通配订阅、二进制负载。

## 7. 评审结论（2026，已实现）

| # | 决策点 | 结论 | 落地位置 |
|---|---|---|---|
| D1 | 本期是否两个方向都做 | **两个方向都做**：插件 sink（寻址投递）+ 宿主事件（发布/订阅） | `IPiEventSink`、`IPiHostEvents` |
| D2 | topic 标识 | **UTF-8 字符串**；`pi.` 框架保留、app 用自有前缀；字节比较、大小写敏感、无通配语法 | `pi_plugin_events.h` 顶部契约；`PI_EVENT_TOPIC_MAX`=128 |
| D3 | payload | **复用 `PiPluginProperty` 键值**，不加二进制块 | `PiEvent.payload` |
| D4 | 投递线程 | **宿主 marshal 到自己的主/owner 线程**；`publish()` 任意线程；sink 与订阅回调都在主线程，插件侧无需锁 | `pi_event_router_pump()` + 契约 3 条 |
| D5 | `type` 字段 | **保留**：`PI_EVENT_NOTIFY` / `PI_EVENT_REQUEST` + `>= 0x80000000` 自定义区 | `PI_EVENT_*` |
| D6 | 路由实现放哪 | **接口进核心头；路由糖做成可选静态库** `piplugin_events`（`host_kits/events/`）；宿主可不用它 | `pi_event_router.*`；开关 `PI_BUILD_HOST_KIT_EVENTS` |
| D7 | 订阅匹配 | **本期只做精确匹配**，且明确"框架不定义任何通配语法" | `pi_host_events_subscribe` 契约；路由器 `strcmp` |
| D8 | session 加事件泵 | **加，但只加机制**：按槽位记账 sink + 投递 + 卸载序列里释放；不持队列、不决定泵点与路由策略 | `pi_host_session_deliver_event()` / `has_event_sink()` / `get_host_events()` |
| D9 | 订阅的生命周期归属（评审时补入） | **订阅带 owner，卸载自动退订**：`subscribe(topic, owner, cb, user, &handle)` + `drop_owner(owner)`；kit 在每个槽位卸载时调用 | 接口第 4 槽 + `SessionTearDownSlot` 步 2 |

**不采纳的备选**（连同理由，避免将来重开）：GUID topic（可读性与"没有全局注册表"两点都不划算）、
payload 加二进制块（生命周期/跨进程/定帧三件事本期都不想回答）、发布者线程同步回调
（Qt 跨线程死锁的既有教训）、去掉 `type`（宿主将失去"不懂 app topic 也能路由"的能力）、
路由进核心库（核心变胖并引入队列与锁策略）、通配订阅（框架不定义语法，
宿主可以自己在回调里做前缀分流）。

## 8. 实现记录（与草案的差异）

1. **`subscribe` 多了 `owner`，`IPiHostEvents` 多了第 4 个槽位 `drop_owner`**（D9）；
   草案里只有 publish/subscribe/unsubscribe。理由：插件卸载后路由表里残留的函数指针
   会变成"调用已卸载内存"，而"谁卸载了"的权威来源正是 session 的 teardown。
2. **`pi_event_router_extra_qi()` 直接作为 `pi_host_services_create_ex()` 的钩子**：
   APP-01（通道 B）与 APP-06 在这里合成一件事 —— 宿主把路由器挂上，插件 QI 就拿到它，
   session 也从同一个宿主对象 QI 到它（走的就是插件将来会走的那条路径）。
3. **`PiEvent` 的字段集视为冻结**，不追加：与 `PiPluginDescriptor` 不同，
   事件里没有 `api_version` 可判别布局，而插件在运行期无法知道宿主编译时的 API 版本，
   所以 host→plugin 方向追加字段无法安全判定。将来要携带更多数据 → **新增接口**
   （`IPiEventSink2` + `PiEvent2`），沿用"只增不改"的 COM 规则。
4. **路由器的丢弃策略 = 丢最新**（队列满时丢新来的事件），并把 `dropped_full` 计数暴露出来；
   契约只要求"宿主可丢，但必须可观测"。`pi_event_router_set_capacity()` 缩容时丢最旧。
5. **pump 重入规则**被明确并写进头文件：在回调里 `publish()` 是允许的，
   但它由**下一次 pump** 投递，绝不递归 —— 测试把这条规则断言了两遍
   （宿主回调里发的事件、插件订阅回调里的回声）。
6. **`PI_EVENT_TOPIC_MAX` = 128**（含终止符）：超长 topic 被判 `PI_E_INVALIDARG`，
   便于宿主用定长缓冲拷贝。
7. **取消 `origin` 的自报**：`publish` 传入的 `origin` 由宿主覆盖（防冒名），
   宿主自己发布时为 NULL —— 与草案一致，这里只是明确它是**宿主填的**。
8. 验收跑的是 `tests/test_host_events`（ctest `events_two_way_loop`，36 条断言、退出码判定），
   覆盖：双向闭环、**按地址投递**与**按 topic 扇出**两条投递路径、
   pump 重入规则、unsubscribe 真停止与非法 handle、D9 的卸载退订、
   以及"无 sink 插件照常加载 + 投递返回 `PI_E_NOINTERFACE`"的优雅降级。
