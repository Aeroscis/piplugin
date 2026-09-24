# piplugin 设计文档

## 1. 概述

piplugin 是一个 **跨平台、纯 C ABI 的插件框架**，采用 **COM 风格接口（vtbl + GUID + 引用计数）** 设计。
框架核心极度精简且与 UI 无关，GUI / 网络 / 服务等能力全部作为**可选能力（capability）**，在运行时通过
`QueryInterface` 双向发现——这一设计直接借鉴了 **LV2 音频插件标准** 的 feature 协商模型。

核心设计目标：

- **ABI 稳定**：所有接口是纯 C 结构体（函数指针表），可跨编译器、跨语言（Rust / C# / Java FFI）使用。
- **宿主无关**：宿主可以是 GUI（imgui / Qt / wxWidgets / 裸 Win32）也可以是 headless（任务服务器、CLI）。
- **插件无关**：插件可以用 Qt、imgui、GTK 或任何工具包写 UI，宿主不需要知道插件用了什么。
- **能力协商**：宿主在**实例化前**即可查看插件声明的能力（requires / optional / provides），提前拒绝不匹配的插件。

## 2. 顶层架构

```
宿主 (Host)
  ├─ GUI Host        （imgui / Qt / 裸 Win32）
  ├─ Headless Host   （任务服务器 / CLI；不暴露 IPiPluginHostUI，插件自动降级）
  └─ 任意宿主
        │  pi_plugin_module_load / pi_plugin_host_create_plugin
        │  （宿主侧 kit：piplugin_host = 加载 + 双向门禁 + 七步卸载序列）
        ▼  动态加载（LoadLibrary / dlopen）
插件 DLL（.dll / .so / .dylib）
  └─ pi_plugin_entry() → IPiPluginFactory
       ├─ PiPluginDescriptor（名称 / 版本 / 能力声明 / 自由元数据 properties）
       └─ CreateInstance → IPiPluginBase
            ├─ IPiPluginView（GUI 插件：attach / on_idle / on_resize）
            └─ IPiPluginService（headless / 服务插件）

插件侧可选链接 UI 适配器套件：piplugin_qt、piplugin_imgui
宿主侧 kit（宿主链接，不属于插件 ABI）：piplugin_host、piplugin_host_qt、piplugin_host_dx11
```

### 2.1 目录结构

| 路径 | 内容 |
|---|---|
| `include/piplugin/` | 公共头文件（完整框架 API，`pi_plugin.h` 为总入口；C++ 糖在可选的 `pi_cpp.h`，**不**包含在总入口里） |
| `src/` | 框架核心 C 实现（`pi_plugin_host.c`、`pi_plugin_unknown.c`） |
| `src/piplugin/` | 核心库 CMake 工程 + CMake package config |
| `adapters/` | 插件侧 UI 适配器套件（`qt/` SHARED、`imgui/` STATIC） |
| `host_kits/` | 宿主侧 kit（`core/` 会话、`events/` 事件路由、`qt/` 与 `dx11/` 嵌入胶水） |
| `examples/` | 可构建运行的最小示范（宿主 / 插件 / FFI / 特化 app / 插件发现…） |
| `cmake/` | 项目自用的 CMake 模块（工程初始化、文件分类、消息、版本资源、cpack） |
| `cmake/pi/` | 同上，语言标准 / 路径常量 / 编译选项一类 |
| `tests/` | 测试宿主、测试插件与单元测试（`unit` / `unit_cpp` / `unit_threads`） |
| `scripts/` | 一条命令的验收入口（`verify.ps1` 等） |
| `conanfile.py` | Conan 2 配方（依赖管理 + 打包） |

## 3. 核心接口族

所有接口都继承 `IPiUnknown`（根接口），因此拥有统一的
`pi_query_interface` / `pi_add_ref` / `pi_release` 三方法。

### 3.1 接口列表

| 接口 | 方向 | 职责 |
|---|---|---|
| `IPiUnknown` | 根 | 引用计数 + `QueryInterface`（COM 身份规则） |
| `IPiPluginFactory` | 插件→宿主 | 工厂：元数据、枚举类、创建实例 |
| `IPiPluginBase` | 插件 | 生命周期：`pi_plugin_initialize` / `pi_plugin_terminate` / `pi_plugin_get_view` |
| `IPiPluginView` | 插件 | GUI 视图：`pi_plugin_attach` / `pi_plugin_on_idle` / `pi_plugin_on_resize` 等 |
| `IPiPluginService` | 插件 | headless 服务：`pi_plugin_service_start` / `stop` / `poll` / `get_status` |
| `IPiPluginHostServices` | 宿主 | 总是提供：内存分配、消息投递 |
| `IPiPluginHostUI` | 宿主(可选) | GUI 宿主能力：父窗口句柄、UI 线程 id |
| `IPiPluginEventSink` | 插件(可选) | 事件接收：宿主按地址投递 `PiPluginEvent`（0.4，APP-06） |
| `IPiPluginHostEvents` | 宿主(可选) | 事件路由：发布 / 订阅 / 退订 / owner 退订（0.4，APP-06） |

### 3.2 已知 GUID

定义于 `src/pi_plugin_unknown.c`，以 `data1` 区分：

| GUID | 接口 |
|---|---|
| `0x00000000` | `PI_IID_UNKNOWN` |
| `0x00000001` | `PI_PLUGIN_IID_PLUGIN_FACTORY` |
| `0x00000002` | `PI_PLUGIN_IID_PLUGIN_BASE` |
| `0x00000003` | `PI_PLUGIN_IID_PLUGIN_VIEW` |
| `0x00000010` | `PI_PLUGIN_IID_HOST_SERVICES`（0.2 新增） |
| `0x00000011` | `PI_PLUGIN_IID_HOST_UI`（0.2 新增） |
| `0x00000020` | `PI_PLUGIN_IID_SERVICE`（0.2 新增） |
| `0x00000030` | `PI_PLUGIN_IID_EVENT_SINK`（0.4 新增，APP-06） |
| `0x00000031` | `PI_PLUGIN_IID_HOST_EVENTS`（0.4 新增，APP-06） |

## 4. 能力协商模型（LV2 风格）

插件在 `PiPluginDescriptor.capabilities` 中声明一组 `PiPluginCapability`：

```c
typedef struct PiPluginCapability {
    PiGuid   iid;    /* 能力 / 接口 GUID */
    uint32_t flags;  /* PI_PLUGIN_CAP_REQUIRED | PI_PLUGIN_CAP_OPTIONAL | PI_PLUGIN_CAP_PROVIDES */
} PiPluginCapability;
```

| 标志 | 含义 |
|---|---|
| `PI_PLUGIN_CAP_REQUIRED` (1) | 宿主**必须**提供，否则实例化/初始化失败 |
| `PI_PLUGIN_CAP_OPTIONAL` (2) | 插件"用了更好"，宿主没有则优雅降级 |
| `PI_PLUGIN_CAP_PROVIDES` (4) | 插件实现该接口（如 `PI_PLUGIN_IID_PLUGIN_VIEW`、`PI_PLUGIN_IID_SERVICE`） |

**双向协商**：

- 宿主侧在 `pi_plugin_factory_create_instance` **之前**调用 `pi_plugin_descriptor_requires(desc, &PI_PLUGIN_IID_HOST_UI)`
  检查硬性 GUI 需求——headless 宿主直接拒绝要求 GUI 的插件。
- 插件侧在 `pi_plugin_initialize` 中 `QueryInterface(PI_PLUGIN_IID_HOST_UI)` 探测宿主是否为 GUI 宿主；
  无 GUI 宿主时插件不创建任何界面，只提供 `IPiPluginService` 等无头能力。

示例（Qt 测试插件 `tests/test_plugin`）：

```c
m_capabilities[0].iid = PI_PLUGIN_IID_PLUGIN_VIEW;
m_capabilities[0].flags = PI_PLUGIN_CAP_PROVIDES;   /* 我提供 GUI 视图 */
m_capabilities[1].iid = PI_PLUGIN_IID_HOST_UI;
m_capabilities[1].flags = PI_PLUGIN_CAP_OPTIONAL;   /* 有 GUI 宿主就用，没有也活 */
```

## 5. 对象生命周期

### 5.1 插件模块

```
pi_plugin_module_load(path)
   ├─ LoadLibraryA / dlopen
   ├─ GetProcAddress / dlsym("pi_plugin_entry")
   └─ entry() → IPiPluginFactory (add-ref)
pi_plugin_module_unload(module)
   └─ release(factory) → FreeLibrary / dlclose
```

模块必须保持加载，直到所有由它创建的插件实例被释放（卸载 DLL 时插件代码若仍在栈上，
属未定义行为）——`pi_plugin_host_create_plugin` 通过 `out_module` 把模块所有权交给调用者规避此问题。

### 5.2 插件实例

```
pi_plugin_factory_create_instance(factory, guid, host, &plugin)
   → pi_plugin_initialize(plugin, host)   /* 插件内部 AddRef host 并探测 IPiPluginHostUI */
   → ... 使用阶段 ...
   → pi_plugin_terminate(plugin)          /* 释放资源 */
   → pi_plugin_release(plugin)            /* 引用归零 → destroy 回调 */
```

### 5.3 引用计数基类 `PiRefCountedBase`

框架提供可复用的引用计数基类，插件对象把它作为**第一个成员**嵌入：

```c
typedef struct PiRefCountedBase {
    IPiUnknown          unk;
    volatile uint32_t   ref_count;
    PiDestroyProc       destroy;   /* 归零时调用：C 对象 free()，C++ 对象 delete */
} PiRefCountedBase;
```

- **C 对象**：`destroy` 指向 `free`-like 函数。
- **C++ 对象**：`destroy` 指向模板 thunk `pi_plugin_cpp_destroy<T>`，确保析构函数运行。
- 引用计数用平台原子操作（`InterlockedIncrement` / `__sync_add_and_fetch`），线程安全。

## 6. 宿主服务（IPiPluginHostServices / IPiPluginHostUI）

框架在 `src/pi_plugin_host.c` 提供**默认宿主实现** `PiPluginDefaultHost`：

- 宿主调用 `pi_plugin_host_services_create_default(post_message, user_data, ui_parent_window, &services)`
  创建对象；传入有效窗口则同时暴露 `IPiPluginHostUI`，传 `PI_INVALID_WINDOW` 则保持 headless。
- `pi_plugin_host_services_create_ex()` 在此基础上多一个 **extra-QI 钩子**（APP-01）：
  框架 IID 之外的 `QueryInterface` 全部转交宿主，于是 app 可以把自己的服务
  递给插件（通道 B），而插件侧仍然是普通的一次 `QueryInterface`。传 `NULL`
  钩子时两条入口共用同一条实现路径，行为完全一致。
- `IPiPluginHostUI` 用独立的轻量 wrapper 对象（COM 身份规则：不同接口需要独立 vtbl 槽位）返回，
  wrapper 内部 AddRef 持有 owner，避免悬垂。
- `pi_plugin_host_default_set_ui_window()` 允许宿主在运行时切换嵌入窗口 / 切回 headless。
  它改的是宿主报告的**活值**（已发出的 `IPiPluginHostUI` 指针立刻读到新容器），搬动
  插件控件仍是宿主的动作：`pi_plugin_view_detach()` -> 改窗口 -> `pi_plugin_view_attach(新容器)`
  —— 回归用例 `container_switch_runtime`（W-02，tests/test_host_multi）。

### 6.1 事件通道（APP-06，API 0.4）

宿主可选提供 `IPiPluginHostEvents`（插件 QI 它发布/订阅），插件可选实现 `IPiPluginEventSink`
（宿主按地址投递）。**宿主是 broker**：插件之间从不互相认识，谁收到什么由宿主决定。

- 分层：两个接口在**核心头** `pi_plugin_events.h`（零实现）；
  可选的 `PiPluginEventRouter`（`host_kits/events/`，STATIC）提供现成的订阅表 + 有界队列 + pump；
  宿主 kit L0 负责**按槽位记账 sink** 并在七步卸载序列里"先按 owner 退订、再释放 sink"。
- 典型接法：`pi_plugin_event_router_create()` → 用 `pi_plugin_event_router_extra_qi()` 当
  `pi_plugin_host_services_create_ex()` 的钩子（通道 B）→ 宿主在主循环里 `pi_plugin_event_router_pump()`。
- 线程：`publish` 任意线程；投递与回调都在宿主主线程，插件侧 sink 无需锁。
- 生命周期：订阅带 `owner`（= 插件实例指针），插件**忘记退订也不会**在卸载后回调 ——
  kit 兜底（详见 `docs/design/events.md` 的 D9）。

## 7. UI 适配器套件（Adapter Kits）

适配器套件的核心思想：**把"UI 工具包兼容层"从插件代码中抽离**成可复用库，
让插件作者只写纯 UI 逻辑（draw 回调 / widget 工厂），其余（事件循环合并、窗口嵌入、
线程 marshal、生命周期）全部由套件处理。

### 7.1 piplugin_qt（Qt 套件）

- **进程唯一的 `QApplication`**：套件是 SHARED 库，进程里只有一份；`QApplication`
  **创建在宿主的 GUI 线程上**（Qt 要求它就是 GUI 线程；Win32 也要求父子窗口同线程），
  由宿主每帧调用的 `pi_plugin_on_idle()` 驱动（内部是有上限的一段 `processEvents()`）。
- **嵌入**：`pi_plugin_attach()` 在控件原生窗口创建**之前**把宿主容器 HWND 写进
  `_q_embedded_native_parent_handle`，让 **Qt 自己**把控件窗口建成容器的 `WS_CHILD`
  （无边框余量、按父客户区坐标；`SetParent` 只是兜底路径）。X11/macOS 为 TODO。
- **全同步、单线程**：attach / detach / resize / set_visible 都在宿主 GUI 线程上同步完成；
  `pi_release()` 返回时控件**与** `QApplication` 都已经析构（最后一次视图销毁时），
  宿主随后 `FreeLibrary` 安全。**不存在**后台 Qt 线程 —— 早期"私有线程跑
  `QApplication::exec()` + queued invocation"的写法会造成卸载崩溃与 detach 死锁，
  不要改回去（详见 `adapters/qt/README.md`）。
- **多 Qt 插件同进程**：共用那一个 `QApplication`；拆控件用带 owner 的
  `pi_plugin_qt_view_shutdown_owner()`，进程级的 `pi_plugin_qt_view_shutdown()` 只用于"整个进程的
  Qt 用量都归我"的场景。回归用例 `tests/test_host_multi`（APP-08）。
- **限制**：面向**非 Qt 宿主**（宿主自己就是 Qt 程序时不要把插件 Qt 再套一层）；
  Linux/macOS 的嵌入未实现。

### 7.2 piplugin_imgui（imgui 套件）

- **无独立事件循环**：imgui 是立即模式，套件在宿主 GUI 线程的 `pi_plugin_on_idle()` 里渲染一帧。
- **自带 D3D11**：创建独立 ImGui context + Win32/DX11 backend + 子窗口 swapchain。
- **context 隔离**：渲染期间切换为自己的 ImGui context，结束后恢复宿主原有的
  context —— 因此**可以嵌进本身也用 imgui 的宿主**而互不干扰。
- 插件作者只写 `PiPluginImGuiViewDesc.draw` 回调。

## 8. 构建系统

### 8.1 Conan 2（conanfile.py）

- `cmake_layout(self)`：conan 自动管理生成器输出与 `CMakeUserPresets.json`。
- 依赖：`imgui/1.92.8`（imgui adapter 或 imgui 测试宿主任一有效开启时自动拉取）。
- 选项：`shared` / `fPIC` + `PI_PLUGIN_BUILD_*` 开关树（adapter kits 与 tests 各有
  总开关 + 分开关，与 CMake 缓存选项同名，整批转发）。
- Qt 为**本地安装**，非 conan 依赖；仓库里**不写死任何路径**，查找顺序
  `PI_PLUGIN_QT_PREFIX` → `Qt5_DIR` / `CMAKE_PREFIX_PATH`（ECO-05，见 `docs/todo/build.md` #4）。

### 8.2 CMake 层级

| 层 | 职责 |
|---|---|
| 根 `CMakeLists.txt` | 引入 pi 模块、`pi_init_glob_proj`、汇总子目录 |
| `cmake/pi/` | 项目自定义模块：语言标准、路径常量、编译选项、消息、文件分类 |
| `src/piplugin/` | 核心 SHARED 库 + export/config 安装 |
| `adapters/` | 两个 UI 套件（`imgui` STATIC / `qt` SHARED；依赖不满足时自检禁用） |
| `tests/` | 5 个测试宿主 + 6 个插件目录（8 个插件 DLL）+ `unit` / `unit_cpp` / `unit_threads`，依赖不满足时优雅 DISABLED |

### 8.3 产物布局

- 核心库 → `<root>/lib/<CONFIG>/`（`.dll` + `.lib`，Debug 带 `d` 后缀）
- 测试宿主/插件 → `<root>/bin/<CONFIG>/`：由各目标的 **POST_BUILD** 维护（构建完即可跑，
  不再依赖 install）；install 只负责"干净前缀下的产品树"，默认前缀 `<build>/install`
- Qt 运行时 DLL + `platforms/qwindows.dll` 自动复制到 `bin/<CONFIG>/`（**仅构建树**；
  install / cpack 归档不含 Qt 运行时，见 `docs/todo/build.md` #8）
- 默认安装前缀在 `CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT` 时被设为 `<build>/install`，
  于是 conan / 纯 CMake / cpack 三条流程一致

## 9. 平台与移植

| 平台 | 支持 | 说明 |
|---|---|---|
| Windows | ✅ 完整 | msvc + Win32 + D3D11；`__stdcall` ABI |
| Linux | ◐ 框架层 | **CI 证明可编译**（核心 + 宿主 kit，gcc/clang）；UI 嵌入未实现（X11 XEmbed，FUT-01） |
| macOS | ◐ 框架层 | 编译路径就绪（dlopen/NSView 类型已定义），嵌入未实现（FUT-02） |

`PI_CALL` 在 Windows 定义为 `__stdcall`（最大 FFI 兼容），其他平台为空。