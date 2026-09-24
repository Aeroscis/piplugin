# 核心框架待办（Framework）

> 本文件记**还没做**的事（标题不带完成标记的条目），以及已完成项的**结论 + 复核入口**。
> 已完成条目不再保留当时的方案原文——那是历史，在 `git log` 与 `CHANGELOG.md` 里；
> 通用机制知识在 `docs/design/`。

## 1. IPiPluginService 的真实实现与测试 [P1] —— 已完成（roadmap APP-07）

> **结论**：`tests/test_plugin_service/` 是纯 C 服务插件（`PROVIDES PI_PLUGIN_IID_SERVICE`，
> 实现 start/poll/status/stop，无任何 UI）；`tests/test_headless_host/` 加载它并把全
> 生命周期逐条断言（start 缺必填选项 → `PI_E_MISSINGCAPABILITY`、poll 的副作用计数、
> stop 幂等、卸载序列再 stop 一次），ctest 用例 `headless_host_service_lifecycle`
> 按退出码判定。场景矩阵（`docs/todo/tests.md` #4）的"headless + service"格已覆盖。
> **复核**：`ctest -C Debug -R headless_host_service_lifecycle`。

## 2. 事件/信号机制（框架级） [P2] —— 已完成（roadmap APP-06）

> **结论**：已按 mini-RFC（`docs/design/events.md`，D1~D9 全部有结论）实现，API 0.4。
> 现有 `IPiPluginEventSink`（插件可选实现，宿主按地址投递）+ `IPiPluginHostEvents`（宿主可选提供：
> publish / subscribe(owner) / unsubscribe / drop_owner），路由糖 `piplugin_events`
> 是可选静态库（`host_kits/events/`）；宿主 kit L0 负责 sink 记账与卸载时的 owner 退订。
> **非目标**（有意不做，写在 `events.md` §2）：跨进程传输（FUT-05）、可靠投递、RPC、
> 通配订阅、二进制负载 —— 想扩这条通道之前先读那一节。
> **复核**：ctest `events_two_way_loop`（`tests/test_host_events`）；设计见 `events.md`。

## 3. 插件热重载 / 动态管理 [P2]（roadmap FUT-04，保持开放）

目前模块卸载要求先释放全部实例（`pi_plugin_module_unload` 注释明确）。可设计：
- 实例==0 时的安全卸载 + 重新加载（热更新插件），
- 或带会话迁移的平滑重载方案。需要先解决"插件代码在栈上"的卸载约束。

## 4. 官方语言绑定示例 [P2] —— 已完成（roadmap ECO-06）

> **结论**：`examples/ffi/{python,rust,csharp}/` 三种语言各自完成：加载框架 DLL 与官方
> 测试插件、QI 工厂（命中 → `PI_OK`，未命中 → `PI_E_NOINTERFACE` 且 `*out == NULL`）、
> 读 descriptor、**用该语言手搓宿主对象表**（C 回调表）、完整
> initialize/terminate/卸载再加载。Rust 示例零 crate（`kernel32` 经 `extern "system"`，
> 可离线构建）。
> **复核**：`scripts/verify_ffi.ps1`（`verify.ps1` 第 3 项；工具链缺失的语言报 SKIP
> 不算失败）。

## 5. C++ RAII 包装层（可选） [P2] —— 已完成（roadmap APP-05）

> **结论**：`include/piplugin/pi_cpp.h`（header-only，无 ABI、无导出符号、无运行时依赖）：
> `PiPluginPtr<T>`（持有框架 AddRef 过的引用、move-only、`qi_to<U>()` + `PiPluginIidOf<T>` 映射）、
> `PiPluginUniqueModule`（RAII `pi_plugin_module_unload`）、`pi_plugin_cpp_destroy<T>`。刻意**不**被
> `pi_plugin.h` 总入口包含——C 总入口保持纯 C。
> **复核**：文档在 `docs/design/interfaces.md` §7；ctest `unit_cpp` 用
> `_CrtDumpMemoryLeaks()` 按退出码断言无泄漏。

## 6. 加载错误诊断增强 [P2] —— 已完成（W-01）

> **结论**：错误串改为**线程局部**（Windows `__declspec(thread)`、其余 `_Thread_local`），
> 每个线程读回**自己**那次 `pi_plugin_module_load` 的结果；旧函数签名与"下次同线程 load 前
> 有效"的语义不变。另加 `pi_plugin_module_get_load_error_r(char* buf, size_t size)`
> （调用方提供缓冲的拷贝变体：拷贝可留存、不受后续 load 影响；`PI_E_INVALIDARG` /
> 截断语义已文档化）。导出面 27 → 28（纯新增）。
> **复核**：`docs/design/interfaces.md`（函数表 + §6 的线程模型总结）、
> `interface-freeze-review.md` 的 F6；回归是 `tests/unit` 里 4 线程各加载自己独有的
> 不存在路径的并发用例（改回进程级 buffer 时该用例稳定失败）。

## 7. 宿主服务的线程模型文档化 [P1] —— 已完成（roadmap BLK-08）

> **结论**：线程契约已成文并作为宿主实现 checklist 项：`pi_plugin_host_post_message` /
> 事件 `publish` 可从任意线程调用，宿主负责 marshal 到自己的主线程回调；APP-06 的
> 事件机制沿用并强化了同一模型（`pi_plugin_event_deliver`、订阅回调全在宿主主线程）。
> **复核**：`docs/design/interfaces.md` §6「线程模型总结」、
> `docs/design/interface-freeze-review.md`；`docs/tutorial/write-host.md` 的 checklist。

## 8. Windows .rc 版本资源 [P2] —— 已完成（W-07）

> **结论**：`cmake/version_dll.rc.in` 提供模板，`cmake/version_resource.cmake` 的
> `piplugin_add_version_resource(<target> "<说明>")` 负责注入 + 按配置生成 `.rc` +
> 挂到目标上；核心库、两个适配器套件、四个宿主 kit 全部启用，`tests/` 与 `examples/`
> 按硬规则排除。`project(VERSION)` 是唯一事实来源，版本号不可能与 `conanfile.py` /
> 头文件漂移；`OriginalFilename` 走生成器表达式，所以 Debug 下写 `piplugind.dll`。
> **一条别误读的实测结论**（细节与适用范围写在 helper 的注释里）：STATIC 库里的
> `.res` **不会**进入消费方二进制——MSVC 链接器只按符号需求拉取静态库成员，所以四个
> STATIC 宿主 kit 与 imgui 套件的 `.rc` 目前是"随 .lib 备着"，真正可见的是核心库与
> Qt 套件两个 DLL；目标改成 SHARED 时资源自动生效，不必回头补。
> **复核**：`cmake/version_resource.cmake` 的注释；`docs/todo/build.md` #6。

## 9. ABI 2.0（多视图 API）的时间窗 [P2]（开放）

`pi_plugin_get_view` 是**单视图**接口，"一个插件多面板"属 ABI 2.0 事项（见
`host_kits/core/pi_host_session.h:34`）。什么时候开 2.0 是**排期决策**，与 1.0 的
"ABI 冻结承诺"绑在一起，需要维护者显式拍板；不做也不阻断 0.x。
（相关：`docs/design/interface-freeze-review.md` §6 的冻结结论。）
