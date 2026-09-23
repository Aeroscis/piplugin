# 核心框架待办（Framework）

## 1. IPiService 的真实实现与测试 [P1] —— 已完成（roadmap APP-07）

> **状态**：已落地。`tests/test_plugin_service/` 是一个纯 C 服务插件
> （`PROVIDES PI_IID_SERVICE`，实现 start/poll/status/stop，无任何 UI），
> `tests/test_headless_host/` 加载它并把全生命周期逐条断言
> （start 缺必填选项 → `PI_E_MISSINGCAPABILITY`、poll 的副作用计数、
> stop 幂等、卸载序列再 stop 一次），ctest 用例 `headless_host_service_lifecycle`
> 按退出码判定。场景矩阵（`tests.md` #4）已补上"headless + service"格。

## 2. 事件/信号机制（框架级） [P2] —— 已完成（roadmap APP-06）

> **状态**：已按 mini-RFC（`docs/design/events.md`，D1~D9 全部有结论）实现，API 0.4。
> 现有：`IPiEventSink`（插件可选实现，宿主按地址投递）+ `IPiHostEvents`
> （宿主可选提供：publish / subscribe(owner) / unsubscribe / drop_owner）；
> 可选路由糖 `piplugin_events`（`host_kits/events/`）；宿主 kit L0 负责 sink 记账与
> 卸载时的 owner 退订。验收：`tests/test_host_events`（ctest `events_two_way_loop`）。
> **未做**（有意，写在 RFC 非目标里）：跨进程传输（FUT-05）、可靠投递、RPC、通配订阅、
> 二进制负载。

插件目前通过 `pi_host_post_message` 单向发消息给宿主。已扩展：
- 宿主→插件的事件通道（原来只有 `pi_on_idle`/`pi_on_resize` 这类轮询/视图事件）；
- 命名事件/信号订阅机制（类似 glib signals），让 GUI 宿主能监听插件的结构化事件。

## 3. 插件热重载 / 动态管理 [P2]（roadmap FUT-04，保持开放）

目前模块卸载要求先释放全部实例（`pi_module_unload` 注释明确）。可设计：
- 实例==0 时的安全卸载 + 重新加载（热更新插件），
- 或带会话迁移的平滑重载方案。需要先解决"插件代码在栈上"的卸载约束。

## 4. 官方语言绑定示例 [P2] —— 已完成（roadmap ECO-06）

> **状态**：已落地。`examples/ffi/{python,rust,csharp}/`：每种语言各自加载框架
> DLL 与官方测试插件、QI 工厂（命中 → `PI_OK`，未命中 → `PI_E_NOINTERFACE`
> 且 `*out == NULL`）、读 descriptor、**用该语言手搓宿主对象表**（C 回调表），
> 完整 initialize/terminate/卸载再加载。Rust 示例零 crate（`kernel32` 经
> `extern "system"`，可离线构建）。`scripts/verify_ffi.ps1` 跑三个语言并已是
> `verify.ps1` 第 3 项，工具链缺失的语言报 SKIP 不算失败。

头文件注释声明 ABI 面向"Rust、C#、Java FFI"。建议补充：
- **Rust** 侧最小 FFI 示例（`#[repr(C)]` + vtbl）加载插件；
- **C#** P/Invoke 示例；
- **Python** ctypes 示例。
作为 `examples/ffi/` 演示目录（不影响核心）。

## 5. C++ RAII 包装层（可选） [P2] —— 已完成（roadmap APP-05）

> **状态**：已落地。`include/piplugin/pi_cpp.h`（header-only，无 ABI、无导出
> 符号、无运行时依赖）：`PiPtr<T>`（持有框架 AddRef 过的引用、move-only、
> `qi_to<U>()` + `PiIidOf<T>` 映射）、`PiUniqueModule`（RAII `pi_module_unload`）、
> `pi_cpp_destroy<T>`。刻意**不**被 `pi_plugin.h` 总入口包含——C 总入口保持
> 纯 C。文档在 `interfaces.md` §7；两个测试插件与 Qt 测试宿主已改用；
> `tests/unit_cpp`（ctest `unit_cpp`）用 `_CrtDumpMemoryLeaks()` 按退出码
> 断言无泄漏。

对 C++ 宿主/插件作者提供 `PiPtr<T>` / 接口包装（引用计数 RAII、
`QueryInterface` 安全转换），减少手写 AddRef/Release 负担。保持 C ABI 不变，
仅作为头文件内联层提供。

## 6. 加载错误诊断增强 [P2] —— 已上收（W-01）

> **状态**：仍开放。`g_load_error[256]` 仍是 `src/pi_plugin_host.c` 的进程级
> static buffer（L34 定义，L59~L117 各失败路径写入），非线程安全且只保留
> 最后一条。roadmap §8 曾把本条映射到「BLK-06/08」，**不成立**——BLK-06 只
> 加了加载失败路径的用例，没动这个机制本身。已上收为下一波工作 **W-01**
> （`docs/todo/parallel-improvements.md` 派工板，测试线第 1 位）：per-thread
> 缓冲或新增带尺寸的安全变体，旧 API 保持兼容。

`pi_module_get_load_error()` 使用一个进程级 static buffer（`g_load_error[256]`），
非线程安全且只保留最后一条。可改为 per-thread 或返回代码 + 描述的结构，
便于并发宿主诊断。

## 7. 宿主服务的线程模型文档化 [P1] —— 已完成（roadmap BLK-08）

> **状态**：已落地。接口终审（`78a0099`，`docs/design/interface-freeze-review.md`）
> 把线程契约写成显式约定，总结在 `interfaces.md` §6「线程模型总结」：
> `pi_host_post_message` / 事件 `publish` 可从任意线程调用、宿主负责 marshal
> 到自己的主线程回调——并作为宿主实现 checklist 项。APP-06 的事件机制
> 沿用并强化了同一模型（`pi_event_deliver`、订阅回调全部宿主主线程）。

- `pi_host_alloc/free` 标称"线程安全"（内部即 `malloc/free`），但实现是每插件
  直接 `malloc`——若未来切换 allocator 需保持线程安全契约；
- `pi_host_post_message` 的宿主 marshal 语义（消息在**哪个线程**被回调）目前隐含
  "宿主自行决定"，应在文档中明确约定，并作为宿主实现的 checklist 项。

## 8. Windows .rc 版本资源 [P2] —— 已上收（W-07）

> **状态**：仍未做：`version_dll.rc.in 暂未提供` 的注释还在，核心库/套件/宿主
> kit 均无版本资源。roadmap §8 原判「顺带项」**不成立**——已上收为下一波工作
> **W-07**（派工板，构建打包线）：提供 `.rc.in` 模板并给库/套件/宿主 kit 目标
> 启用（排除 `tests/`），验收 = DLL 属性页显示 0.4.0。

核心库/套件/测试目标的 `.rc` 版本资源模板被注释（`version_dll.rc.in 暂未提供`）。
提供模板后，DLL/EXE 将带正确版本信息（`FILE_DESCRIPTION`、`PRODUCT_VERSION` 等
已就绪）。