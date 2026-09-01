# 核心框架待办（Framework）

## 1. IPiService 的真实实现与测试 [P1]

**现状**：`IPiService` 接口已定义（`pi_plugin_service.h`），headless 宿主
（`pi_test_host_headless`）会探测 `PI_IID_SERVICE` 并具备 start/stop/poll 驱动逻辑；
但**现有测试插件都没有实现 SERVICE 能力**，该路径只有接口没有落地示例。

**建议**：
1. 新增一个 headless 服务型测试插件（如模拟"任务/计算"插件），声明
   `PI_IID_SERVICE (PROVIDES)`，实现 start（读 `PiServiceOption` 配置）、poll（做工作）、
   status、stop；
2. 让 `pi_test_host_headless` 加载它，验证完整服务生命周期。

## 2. 事件/信号机制（框架级） [P2]

插件目前通过 `pi_host_post_message` 单向发消息给宿主。可扩展：
- 宿主→插件的事件通道（目前只有 `pi_on_idle`/`pi_on_resize` 这类轮询/视图事件）；
- 命名事件/信号订阅机制（类似 glib signals），让 GUI 宿主能监听插件的结构化事件。

## 3. 插件热重载 / 动态管理 [P2]

目前模块卸载要求先释放全部实例（`pi_module_unload` 注释明确）。可设计：
- 实例==0 时的安全卸载 + 重新加载（热更新插件），
- 或带会话迁移的平滑重载方案。需要先解决"插件代码在栈上"的卸载约束。

## 4. 官方语言绑定示例 [P2]

头文件注释声明 ABI 面向"Rust、C#、Java FFI"。建议补充：
- **Rust** 侧最小 FFI 示例（`#[repr(C)]` + vtbl）加载插件；
- **C#** P/Invoke 示例；
- **Python** ctypes 示例。
作为 `examples/ffi/` 演示目录（不影响核心）。

## 5. C++ RAII 包装层（可选） [P2]

对 C++ 宿主/插件作者提供 `PiPtr<T>` / 接口包装（引用计数 RAII、
`QueryInterface` 安全转换），减少手写 AddRef/Release 负担。保持 C ABI 不变，
仅作为头文件内联层提供。

## 6. 加载错误诊断增强 [P2]

`pi_module_get_load_error()` 使用一个进程级 static buffer（`g_load_error[256]`），
非线程安全且只保留最后一条。可改为 per-thread 或返回代码 + 描述的结构，
便于并发宿主诊断。

## 7. 宿主服务的线程模型文档化 [P1]

- `pi_host_alloc/free` 标称"线程安全"（内部即 `malloc/free`），但实现是每插件
  直接 `malloc`——若未来切换 allocator 需保持线程安全契约；
- `pi_host_post_message` 的宿主 marshal 语义（消息在**哪个线程**被回调）目前隐含
  "宿主自行决定"，应在文档中明确约定，并作为宿主实现的 checklist 项。

## 8. Windows .rc 版本资源 [P2]

核心库/套件/测试目标的 `.rc` 版本资源模板被注释（`version_dll.rc.in 暂未提供`）。
提供模板后，DLL/EXE 将带正确版本信息（`FILE_DESCRIPTION`、`PRODUCT_VERSION` 等
已就绪）。