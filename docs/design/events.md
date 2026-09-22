# 事件机制 mini-RFC（roadmap APP-06，通道 C 补全）

> 状态：**待评审**（评审通过后才实现；roadmap 明确要求"先出 RFC 再实现"）
> 作者：维护者 + 实现 agent
> 目标读者：本仓库维护者（决策人）
> 相关代码：`include/piplugin/pi_plugin_host_services.h`（`pi_host_post_message`）、
> `include/piplugin/pi_plugin_view.h`（`pi_on_idle` 轮询）、
> `docs/todo/framework.md` #2、`docs/release-roadmap.md` APP-06 / FUT-05

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
    /* 订阅一个 topic（精确匹配本期；前缀/通配留给将来）。回调在宿主主线程执行。 */
    PiResult (PI_CALL *pi_host_events_subscribe)(void* this_ptr, const char* topic,
                                                 PiEventCallback cb, void* user_data,
                                                 uint32_t* out_subscription);
    PiResult (PI_CALL *pi_host_events_unsubscribe)(void* this_ptr, uint32_t subscription);
} IPiHostEventsVtbl;
```

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
- **版本**：按 pre-1.0 政策，新增接口 = minor 前进一位 → `PIPLUGIN_API_VERSION` 0.3 → **0.4**
  （0.3 已被 APP-04 的 descriptor 追加占用）；单测的版本 tripwire 会失败一次（设计如此）；
- **涉及**（预估）：`include/piplugin/pi_plugin_events.h`（新）、
  `include/piplugin/pi_plugin.h`（加一行 include）、`src/pi_plugin_unknown.c`（两个新 IID）、
  `pi_event_router`（可选静态库，`src/` 或 `host_kits/`）、测试宿主/插件各一侧、
  `docs/design/interfaces.md`、`CHANGELOG.md`；
- **不做**：跨进程、RPC、持久化、通配订阅、二进制负载。

## 7. 请评审决定的事项

| # | 决策点 | 建议 | 备选 |
|---|---|---|---|
| D1 | 本期是否两个方向都做 | 都做（roadmap 原案） | 先只做宿主→插件 sink，插件→宿主继续用 `post_message` |
| D2 | topic 标识 | UTF-8 字符串 + `pi.` 保留区 | 128 位 GUID；或字符串 + 可选 GUID |
| D3 | payload | 复用 `PiPluginProperty` 键值 | 追加二进制块（`data`/`size`） |
| D4 | 投递线程 | 宿主 marshal 到自己的主线程（插件 sink 无需锁） | 发布者线程同步回调（插件必须线程安全） |
| D5 | `type` 字段 | 保留 `NOTIFY`/`REQUEST` + 自定义区 | 去掉 `type`，只用 `topic` |
| D6 | 路由实现放哪 | 接口进核心头；路由糖做成可选静态库 `piplugin_events` | 路由也进核心库（核心变大） |
| D7 | 订阅匹配 | 本期只做精确匹配 | 前缀/通配（`com.example.*`） |
| D8 | 是否给 `pi_on_idle` 加"事件泵"角色 | 不加（泵点仍归宿主，host kit 的 `drive_idle` 天然可复用） | 在 `session` 里加一个 `pi_host_session_pump_events()` |

> 决定后我会按此 RFC 实现，并把"评审结论"追加到本节下方，作为实现依据。
