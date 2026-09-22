# 接口终审记录（Interface Freeze Review）

> 对应 roadmap **BLK-08**。
> 发布（PUBLIC）之后每个 vtbl 就是"已发布"的：只能加接口，不能改接口（COM 规则）。
> 本文是发布前的逐槽终审记录 —— 审出的问题、结论、以及不能改的东西。

## 1. 范围与方法

审的是"一旦发布就冻结"的全部公共面：

| 类别 | 数量 | 出处 |
|---|---|---|
| 接口 vtbl | 7 个接口 / 26 个槽位 | `include/piplugin/*.h` |
| 公共数据导出 | 7 个 `PI_IID_*` GUID 常量 | `src/pi_plugin_unknown.c` |
| 公共帮助函数 | 16 个 `pi_*` 函数 | `pi_plugin_unknown.c`、`pi_plugin_host.c` |

方法：

1. 头文件逐槽比对实现（参数类型、返回码语义、`PI_CALL`、引用所有权）；
2. `dumpbin /exports` 查实际导出符号，确认无意外导出；
3. 用 `ctest`（单测 + 冒烟 + 负向用例）与两个 GUI 回归脚本验证行为。

---

## 2. 冻结的全局约定

写进 `docs/design/interfaces.md` 的公共约定，发布后不得变更：

### 2.1 调用约定与布局

- 所有 vtbl 槽位一律 `PI_CALL`（Windows = `__stdcall`，其它平台为空）；26 个槽位全部标注，无遗漏。
- 接口对象首成员是 `const XxxVtbl* lpVtbl`；实现对象首成员是 `PiRefCountedBase`（单继承布局）。
- 该布局与 C++ 单继承 vtable 布局一致，但**不承诺** C++ 编译器可直接以类指针调用（名字修饰与调用约定仍由编译器决定）；跨语言一律走 C ABI。

### 2.2 返回码

`PiResult` = `int32_t`，`>= 0` 为成功（`PI_SUCCEEDED`）。取值见 `interfaces.md` 1.2，
其中 `PI_E_VERSIONMISMATCH (-9)` 是 BLK-03 新增。

### 2.3 引用所有权（终审逐条确认）

| 类别 | 函数 | 规则 |
|---|---|---|
| **返回 add-ref 过的接口** | `pi_query_interface`、`pi_module_get_factory`、`pi_factory_create_instance`(out)、`pi_plugin_get_view`(out)、`pi_host_services_create_default`(out) | 调用方必须 `release` |
| **返回借用指针/句柄** | `pi_factory_get_descriptor`、`pi_view_get_native_window`、`pi_host_ui_get_parent_window` | **禁止** release；生命周期止于所属对象/模块 |
| **不涉及引用** | `pi_add_ref` / `pi_release` / `pi_host_alloc` / `pi_host_free` / `pi_host_post_message` / `pi_view_*` / `pi_service_*` | — |

`pi_factory_create_instance` 传入的 `host` **不被本调用 add-ref**：插件若要保留该指针，必须自己 add-ref。

### 2.4 out 参数约定

- 失败时一律把 `*out` 置为 **NULL**（含 `pi_host_create_plugin` 的 `*out_plugin` / `*out_module`）；
- 调用方可以不再自带预置 NULL。

### 2.5 数据导出

`PI_IID_*` 是**导出的数据符号**（不是每模块一份的副本）：所有插件与宿主共享框架 DLL 里
那一份 GUID 常量。这是刻意的 —— 能力声明与 QI 都依赖"同一份 128 位值"。

### 2.6 NULL 安全

`pi_iunknown_*` / `pi_view_*` / `pi_service_*` / `pi_factory_*` 等 inline 帮助函数对
`self == NULL`、`lpVtbl == NULL`、槽位为 NULL 三种情况都返回安全值
（`PI_E_INVALIDARG` 或 0），不崩溃。

---

## 3. vtbl 逐槽终审

`base` 指继承自 `IPiUnknownVtbl` 的三个槽位（每张表都省略）。

### 3.1 IPiUnknownVtbl

| 槽位 | 参数 | 返回 | 所有权 |
|---|---|---|---|
| `pi_query_interface` | `(void* this, const PiGuid* iid, void** out)` | `PI_OK` / `PI_E_NOINTERFACE` / `PI_E_INVALIDARG` / `PI_E_OUTOFMEMORY` | 成功时 `*out` 为 add-ref 过的接口指针；失败时 `*out = NULL` |
| `pi_add_ref` | `(void* this)` | 新的引用计数 | — |
| `pi_release` | `(void* this)` | 新的引用计数；归零时销毁对象 | — |

### 3.2 IPiHostServicesVtbl

| 槽位 | 参数 | 返回 | 所有权 |
|---|---|---|---|
| `pi_host_alloc` | `(void* this, size_t size)` | 内存指针或 NULL | 宿主分配，必须用 `pi_host_free` 归还 |
| `pi_host_free` | `(void* this, void* ptr)` | void | NULL 安全 |
| `pi_host_post_message` | `(void* this, uint32_t msg, uintptr_t wparam, intptr_t lparam)` | void | 任意线程可调；宿主负责 marshal |

### 3.3 IPiHostUIVtbl

| 槽位 | 参数 | 返回 | 所有权 |
|---|---|---|---|
| `pi_host_get_parent_window` | `(void* this)` | `PiNativeWindow`；无窗口时 `PI_INVALID_WINDOW` | 借用句柄，无引用计数 |
| `pi_host_ui_thread_id` | `(void* this)` | `uint64_t`；非 0 | 身份令牌，**只用于"是不是同一个线程"的比较**（见 4.2） |

### 3.4 IPiPluginFactoryVtbl

| 槽位 | 参数 | 返回 | 所有权 |
|---|---|---|---|
| `pi_get_descriptor` | `(void* this)` | `const PiPluginDescriptor*` 或 NULL | **借用**：属于模块，模块卸载后失效 |
| `pi_get_class_count` | `(void* this)` | `uint32_t` | — |
| `pi_get_class_guid` | `(void* this, uint32_t index, PiGuid* out)` | `PI_OK` / `PI_E_INVALIDARG` | out 由调用方提供 |
| `pi_create_instance` | `(void* this, const PiGuid*, IPiHostServices*, IPiPluginBase** out)` | `PI_OK` / `PI_E_NOINTERFACE` / `PI_E_INVALIDARG` | 成功时 `*out` 为 add-ref 过的实例；`host` 不被 add-ref |

### 3.5 IPiPluginBaseVtbl

| 槽位 | 参数 | 返回 | 所有权 |
|---|---|---|---|
| `pi_initialize` | `(void* this, IPiHostServices* host)` | `PI_OK` / 其它 | 不接管 host 所有权 |
| `pi_terminate` | `(void* this)` | `PI_OK` | 应在 `release` 之前被调用一次 |
| `pi_get_view` | `(void* this, IPiPluginView** out)` | `PI_OK` / `PI_E_NOINTERFACE` | 成功时 `*out` 为 add-ref 过的 view |

### 3.6 IPiPluginViewVtbl

| 槽位 | 参数 | 返回 | 所有权 / 线程 |
|---|---|---|---|
| `pi_attach` | `(void* this, PiNativeWindow parent)` | `PI_OK` / 其它 | 宿主递容器，不接管所有权；宿主 GUI 线程 |
| `pi_detach` | `(void* this)` | `PI_OK` | detach 后 view 仍可再次 attach（两个官方套件支持；本仓库无单测覆盖） |
| `pi_get_native_window` | `(void* this)` | `PiNativeWindow` | 借用句柄 |
| `pi_on_resize` | `(void* this, int32_t w, int32_t h)` | `PI_OK` | 宿主 GUI 线程 |
| `pi_on_idle` | `(void* this)` | `PI_OK` | 宿主 GUI 线程，每帧 |
| `pi_get_preferred_size` | `(void* this, int32_t* w, int32_t* h)` | `PI_OK` | 建议值；宿主不应用于强制布局（见 4.9） |
| `pi_set_visible` | `(void* this, int32_t visible)` | `PI_OK` | attach 后可见性 |

### 3.7 IPiServiceVtbl

| 槽位 | 参数 | 返回 | 所有权 / 线程 |
|---|---|---|---|
| `pi_service_start` | `(void* this, const PiServiceOption*, uint32_t count)` | `PI_OK` / `PI_E_MISSINGCAPABILITY` / `PI_FAIL` | 不接管 options 所有权 |
| `pi_service_stop` | `(void* this)` | `PI_OK` | **幂等**（文档承诺；卸载序列会再调一次） |
| `pi_service_poll` | `(void* this)` | `PI_OK` | 宿主主循环调用 |
| `pi_service_get_status` | `(void* this, int32_t* out)` | `PI_OK` | out 由调用方提供 |

---

## 4. 审查发现

### 4.1 已修（阻断发布的问题）

| 编号 | 问题 | 修法 |
|---|---|---|
| **F1** | `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON` 把 CRT 内部符号一并导出：实测 DLL 导出 26 个符号，多出 `__local_stdio_printf_options`、`snprintf`、`vsnprintf` | 关掉该全局开关（公开 API 一律显式 `PI_EXPORT`，插件入口显式 dllexport），导出数 **26 → 23**，全部为预期符号 |
| **F2** | `pi_host_ui_thread_id` 非 Windows 分支返回 `getpid()` —— **把进程 id 当线程 id 给插件** | Linux 改 `syscall(SYS_gettid)`（glibc 2.30 以下也有 `gettid`，用 syscall 免依赖），macOS 改 `pthread_self()` 转 64 位；契约写进头文件；Windows 侧有单测精确断言 |
| **F3** | `pi_host_create_plugin` 失败时不给 `*out_plugin` / `*out_module` 赋值，调用方会读到自己残留的旧值 | 入口处预置 NULL，并写入头文件注释与 2.4 约定 |
| **F4** | `PI_PLUGIN_ENTRY_DECL` 展开成 `PI_EXPORT`，而 `PI_EXPORT` 在插件侧是 **dllimport** —— 该宏按其字面用途（定义插件入口）**根本无法编译** | 新增 `PI_PLUGIN_EXPORT`（插件侧的 dllexport / visibility default），`PI_PLUGIN_ENTRY_DECL` 改用它；`pi_test_plugin_badversion` 现在就用该宏定义入口，兼作编译验证 |

导出符号终审结果（`dumpbin /exports bin/Debug/piplugind.dll`，共 **23** 个）：

```
PI_IID_UNKNOWN / PLUGIN_FACTORY / PLUGIN_BASE / PLUGIN_VIEW / HOST_SERVICES / HOST_UI / SERVICE
pi_guid_equal, pi_api_version_compatible, pi_descriptor_find_capability,
pi_descriptor_provides, pi_descriptor_requires, pi_refcounted_init,
pi_refcounted_init_with_destroy, pi_refcounted_add_ref, pi_refcounted_release,
pi_module_load, pi_module_unload, pi_module_get_factory, pi_module_get_load_error,
pi_host_services_create_default, pi_host_default_set_ui_window, pi_host_create_plugin
```

> **发布后追加**：`pi_host_services_create_ex`（roadmap APP-01，可组合宿主服务 /
> 通道 B）—— 新增**函数**而非改动既有 vtbl，符合"只增不改"；导出面因此为
> **24** 个。既有 23 个符号的签名与语义未变（`pi_host_services_create_default`
> 现在只是转调 `create_ex`，行为逐条断言在 `tests/unit`）。
>
> **发布后再次追加**：`pi_descriptor_find_property`（roadmap APP-04），导出面 **25** 个。
> 这一条**不是**纯新增：`PiPluginDescriptor` 末尾追加了 `properties` /
> `property_count`，是真正的**二进制布局变化**。0.x 允许（1.0 才承诺冻结），
> 代价与处理方式：
> - `PIPLUGIN_API_VERSION` minor 2 → 3（`tests/unit` 的版本 tripwire 因此失败过一次，
>   那是设计如此：它是提醒同步这里与 CHANGELOG 的机制）；
> - 版本门禁接受"更老的插件"（同 major、minor 更低），而老插件的结构体更短，
>   所以 `pi_descriptor_find_property()` 用插件声明的 `api_version` 判布局
>   （`minor < 3` → 报"没有属性"），**不**去读那截不存在的内存；
> - 第 3 节列出的 7 个 vtbl / 26 个槽位一个都没动，全局约定（第 2 节）也未变。

### 4.2 记录在案（不阻断发布，1.0 前需要结论）

| 编号 | 事项 | 现状与建议 |
|---|---|---|
| **F5** | `IPiHostUI` 的 QI 每次调用都**新建一个包装对象** | 不符合 COM 标识规则的严格解读（同一对象同一 IID 应返回同一指针）。当前无实际危害（每个包装都读宿主活值、都能独立 release）。1.0 前决定：缓存一个包装，或把"不保证指针唯一"写进契约 |
| **F6** | `pi_module_get_load_error()` 返回**进程级静态缓冲** | 非线程安全，且多个模块互相覆盖（A 模块加载失败的原因会被 B 的覆盖）。建议改为调用方提供缓冲，或返回线程局部值 |
| **F7** | `pi_get_preferred_size` 两个官方套件都**硬编码 400×300 并返回 `PI_OK`** | 宿主无法区分"插件真的想要这么大"和"套件不知道"。建议 Qt 套件返回 `sizeHint()`、imgui 套件返回 `PI_E_NOTIMPL` |
| **F8** | `pi_descriptor_provides/requires` 返回的是**掩码值**（`4` / `1`），不是 `1` | **必须按"非零"判断**，不能当布尔用。已写入 `interfaces.md` 1.4 与单测 |
| **F9** | `PI_LOCAL` 宏有定义但**全库未使用** | 内部函数一律 `static`（比 `PI_LOCAL` 更严格），已覆盖其作用。保留宏以备将来需要非 static 的内部全局 |
| **F10** | `pi_host_create_plugin` 在 `out_module == NULL` 时**故意泄漏模块** | 刻意设计（卸载会让插件代码失效），已文档化。1.0 时可考虑改为返回错误而不是静默泄漏 |

### 4.3 无法在本仓库验证的部分

- **非 Windows 的 `pi_host_ui_thread_id` 修复（F2）**：本仓库没有 Linux/macOS 构建，
  该分支只经过代码审查。Windows 分支有单测精确断言。
- **非 Windows 的框架层与适配器套件能否编译**：见第 5 节，不做承诺。

---

## 5. 平台支持声明（v0.x）

| 平台 | 框架核心 | UI 适配器套件 | UI 嵌入 | 本仓库是否验证过 |
|---|---|---|---|---|
| **Windows** | 完整支持 | 完整支持（imgui / Qt） | 完整支持（Win32 子窗口 + flip-model 交换链） | **是** —— `ctest` + conformance harness + 像素级缩放回归全绿 |
| Linux | 预期可编译 | 预期可编译 | **未实现**（X11 XEmbed 属 FUT-01） | **否**（无 CI、无人工验证） |
| macOS | 预期可编译 | 预期可编译 | **未实现**（NSView 属 FUT-02） | **否**（无 CI、无人工验证） |

**不做过度承诺**：v0.x 只有 Windows 是经过自动化验证的平台。"预期可编译"仅表示代码里
已按平台分支处理（线程身份、动态库加载、符号可见性），**没有任何构建在本仓库跑过**。
在这些平台上"插件 UI 能否嵌进宿主"的答案是否。

> README 就位时（BLK-02）本节必须原样搬过去；`docs/tutorial/adapters.md` 的
> 平台注意事项与本表保持一致。

---

## 6. 冻结结论

- 7 个接口 / 26 个槽位的参数、返回码、调用约定与引用所有权均已逐条确认并记录（第 3 节）；
- 4 个阻断发布的问题已修（F1–F4），导出符号面收敛到 23 个预期符号；
- 6 项记录事项（F5–F10）不阻断发布，但**必须在 1.0 之前给出结论**（1.0 是"ABI 冻结承诺"的时刻）；
- 第 2 节的全局约定发布后不得变更；第 3 节任何 vtbl 槽位发布后**只能新增接口，不得修改**。

**待办（1.0 前）**：F5、F6、F7、F10 需要决议；F2 的非 Windows 分支需要在有 Linux/macOS
构建后复验。这些不阻塞 M0，但阻塞 1.0。
