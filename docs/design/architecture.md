# pipluginframework 设计文档

## 1. 概述

pipluginframework 是一个 **跨平台、纯 C ABI 的插件框架**，采用 **COM 风格接口（vtbl + GUID + 引用计数）** 设计。
框架核心极度精简且与 UI 无关，GUI / 网络 / 服务等能力全部作为**可选能力（capability）**，在运行时通过
`QueryInterface` 双向发现——这一设计直接借鉴了 **LV2 音频插件标准** 的 feature 协商模型。

核心设计目标：

- **ABI 稳定**：所有接口是纯 C 结构体（函数指针表），可跨编译器、跨语言（Rust / C# / Java FFI）使用。
- **宿主无关**：宿主可以是 GUI（imgui / Qt / wxWidgets / 裸 Win32）也可以是 headless（任务服务器、CLI）。
- **插件无关**：插件可以用 Qt、imgui、GTK 或任何工具包写 UI，宿主不需要知道插件用了什么。
- **能力协商**：宿主在**实例化前**即可查看插件声明的能力（requires / optional / provides），提前拒绝不匹配的插件。

## 2. 顶层架构

```
┌─────────────────────────────────────────────────────────┐
│                       宿主 (Host)                       │
│  ┌───────────┐   ┌──────────────┐   ┌────────────────┐  │
│  │ GUI Host  │   │ Headless Host│   │ 任意宿主       │  │
│  │ (imgui/   │   │ (任务服务器/ │   │                │  │
│  │  Qt/裸Win)│   │  CLI)        │   │                │  │
│  └─────┬─────┘   └──────┬───────┘   └───────┬────────┘  │
│        │   pi_module_load / pi_host_create_plugin       │
│        └───────────────┬────────────────────┘           │
└────────────────────────┼────────────────────────────────┘
                         │ 动态加载 (LoadLibrary/dlopen)
┌────────────────────────┼────────────────────────────────┐
│                   插件 DLL (.dll/.so/.dylib)             │
│  ┌─────────────────────┴──────────────────────┐         │
│  │ pi_plugin_entry() → IPiPluginFactory        │         │
│  │   ├─ PiPluginDescriptor（名称/版本/能力声明）│         │
│  │   └─ CreateInstance → IPiPluginBase         │         │
│  │        ├─ IPiPluginView（GUI 插件）          │         │
│  │        └─ IPiService（headless/服务插件）    │         │
│  └─────────────────────────────────────────────┘         │
│  可选链接 UI Adapter Kit：                               │
│  ┌──────────────────────────────┐ ┌──────────────────┐   │
│  │ pipluginframework_qt         │ │ pipluginframework│   │
│  │ （Qt 兼容层：私有线程跑       │ │ _imgui           │   │
│  │  QApplication + 控件嵌入）    │ │ （立即模式 UI，  │   │
│  └──────────────────────────────┘ │  宿主 GUI 线程）  │   │
│                                   └──────────────────┘   │
└──────────────────────────────────────────────────────────┘
```

### 2.1 目录结构

| 路径 | 内容 |
|---|---|
| `include/pipluginframework/` | 公共头文件（完整框架 API，`pi_plugin.h` 为总入口） |
| `src/` | 框架核心 C 实现（`pi_plugin_host.c`、`pi_plugin_unknown.c`） |
| `src/pipluginframework/` | 核心库 CMake 工程 + CMake package config |
| `adapters/` | UI 适配器套件（`qt/`、`imgui/`） |
| `cmake/pi/` | 项目自用的 CMake 模块（消息、文件分类、工程初始化） |
| `tests/` | 测试宿主与测试插件 |
| `conanfile.py` | Conan 2 配方（依赖管理 + 打包） |

## 3. 核心接口族

所有接口都继承 `IPiUnknown`（根接口），因此拥有统一的
`pi_query_interface` / `pi_add_ref` / `pi_release` 三方法。

### 3.1 接口列表

| 接口 | 方向 | 职责 |
|---|---|---|
| `IPiUnknown` | 根 | 引用计数 + `QueryInterface`（COM 身份规则） |
| `IPiPluginFactory` | 插件→宿主 | 工厂：元数据、枚举类、创建实例 |
| `IPiPluginBase` | 插件 | 生命周期：`pi_initialize` / `pi_terminate` / `pi_get_view` |
| `IPiPluginView` | 插件 | GUI 视图：`pi_attach` / `pi_on_idle` / `pi_on_resize` 等 |
| `IPiService` | 插件 | headless 服务：`pi_service_start` / `stop` / `poll` / `get_status` |
| `IPiHostServices` | 宿主 | 总是提供：内存分配、消息投递 |
| `IPiHostUI` | 宿主(可选) | GUI 宿主能力：父窗口句柄、UI 线程 id |

### 3.2 已知 GUID

定义于 `src/pi_plugin_unknown.c`，以 `data1` 区分：

| GUID | 接口 |
|---|---|
| `0x00000000` | `PI_IID_UNKNOWN` |
| `0x00000001` | `PI_IID_PLUGIN_FACTORY` |
| `0x00000002` | `PI_IID_PLUGIN_BASE` |
| `0x00000003` | `PI_IID_PLUGIN_VIEW` |
| `0x00000010` | `PI_IID_HOST_SERVICES`（1.1.0 新增） |
| `0x00000011` | `PI_IID_HOST_UI`（1.1.0 新增） |
| `0x00000020` | `PI_IID_SERVICE`（1.1.0 新增） |

## 4. 能力协商模型（LV2 风格）

插件在 `PiPluginDescriptor.capabilities` 中声明一组 `PiPluginCapability`：

```c
typedef struct PiPluginCapability {
    PiGuid   iid;    /* 能力 / 接口 GUID */
    uint32_t flags;  /* PI_CAP_REQUIRED | PI_CAP_OPTIONAL | PI_CAP_PROVIDES */
} PiPluginCapability;
```

| 标志 | 含义 |
|---|---|
| `PI_CAP_REQUIRED` (1) | 宿主**必须**提供，否则实例化/初始化失败 |
| `PI_CAP_OPTIONAL` (2) | 插件"用了更好"，宿主没有则优雅降级 |
| `PI_CAP_PROVIDES` (4) | 插件实现该接口（如 `PI_IID_PLUGIN_VIEW`、`PI_IID_SERVICE`） |

**双向协商**：

- 宿主侧在 `pi_factory_create_instance` **之前**调用 `pi_descriptor_requires(desc, &PI_IID_HOST_UI)`
  检查硬性 GUI 需求——headless 宿主直接拒绝要求 GUI 的插件。
- 插件侧在 `pi_initialize` 中 `QueryInterface(PI_IID_HOST_UI)` 探测宿主是否为 GUI 宿主；
  无 GUI 宿主时插件不创建任何界面，只提供 `IPiService` 等无头能力。

示例（Qt 测试插件 `tests/test_plugin`）：

```c
m_capabilities[0].iid = PI_IID_PLUGIN_VIEW;
m_capabilities[0].flags = PI_CAP_PROVIDES;   /* 我提供 GUI 视图 */
m_capabilities[1].iid = PI_IID_HOST_UI;
m_capabilities[1].flags = PI_CAP_OPTIONAL;   /* 有 GUI 宿主就用，没有也活 */
```

## 5. 对象生命周期

### 5.1 插件模块

```
pi_module_load(path)
   ├─ LoadLibraryA / dlopen
   ├─ GetProcAddress / dlsym("pi_plugin_entry")
   └─ entry() → IPiPluginFactory (add-ref)
pi_module_unload(module)
   └─ release(factory) → FreeLibrary / dlclose
```

模块必须保持加载，直到所有由它创建的插件实例被释放（卸载 DLL 时插件代码若仍在栈上，
属未定义行为）——`pi_host_create_plugin` 通过 `out_module` 把模块所有权交给调用者规避此问题。

### 5.2 插件实例

```
pi_factory_create_instance(factory, guid, host, &plugin)
   → pi_plugin_initialize(plugin, host)   /* 插件内部 AddRef host 并探测 IPiHostUI */
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
- **C++ 对象**：`destroy` 指向模板 thunk `pi_cpp_destroy<T>`，确保析构函数运行。
- 引用计数用平台原子操作（`InterlockedIncrement` / `__sync_add_and_fetch`），线程安全。

## 6. 宿主服务（IPiHostServices / IPiHostUI）

框架在 `src/pi_plugin_host.c` 提供**默认宿主实现** `PiDefaultHost`：

- 宿主调用 `pi_host_services_create_default(post_message, user_data, ui_parent_window, &services)`
  创建对象；传入有效窗口则同时暴露 `IPiHostUI`，传 `PI_INVALID_WINDOW` 则保持 headless。
- `IPiHostUI` 用独立的轻量 wrapper 对象（COM 身份规则：不同接口需要独立 vtbl 槽位）返回，
  wrapper 内部 AddRef 持有 owner，避免悬垂。
- `pi_host_default_set_ui_window()` 允许宿主在运行时切换嵌入窗口 / 切回 headless。

## 7. UI 适配器套件（Adapter Kits）

适配器套件的核心思想：**把"UI 工具包兼容层"从插件代码中抽离**成可复用库，
让插件作者只写纯 UI 逻辑（draw 回调 / widget 工厂），其余（事件循环合并、窗口嵌入、
线程 marshal、生命周期）全部由套件处理。

### 7.1 pipluginframework_qt（Qt 套件）

- **进程级 `PiQtRuntime`**：一个后台线程跑唯一 `QApplication::exec()`，按引用计数启停。
- **嵌入**：`pi_attach()` 时在 Qt 线程内 `SetParent` 把控件 HWND 挂进宿主容器（X11/macOS 为 TODO）。
- **宿主驱动 idle**：`pi_on_idle()` 只做 dispatcher `wakeUp()`，**绝不**跨线程 `processEvents()`。
- **线程 marshal**：`pi_on_resize()` / `pi_set_visible()` 通过 queued invocation 切到 Qt 线程。
- **生命周期**：`pi_detach()` 异步；`pi_release()`（引用归零）**同步阻塞**直到 Qt 线程清理完毕
  并 join 运行时线程，保证宿主随后 `FreeLibrary` 安全。
- **限制**：面向**非 Qt 宿主**；单进程多 Qt 插件会冲突（套件需改为 SHARED，见 TODO）。

### 7.2 pipluginframework_imgui（imgui 套件）

- **无独立事件循环**：imgui 是立即模式，套件在宿主 GUI 线程的 `pi_on_idle()` 里渲染一帧。
- **自带 D3D11**：创建独立 ImGui context + Win32/DX11 backend + 子窗口 swapchain。
- **context 隔离**：渲染期间切换为自己的 ImGui context，结束后恢复宿主原有的
  context —— 因此**可以嵌进本身也用 imgui 的宿主**而互不干扰。
- 插件作者只写 `PiImGuiViewDesc.draw` 回调。

## 8. 构建系统

### 8.1 Conan 2（conanfile.py）

- `cmake_layout(self)`：conan 自动管理生成器输出与 `CMakeUserPresets.json`。
- 依赖：`imgui/1.92.8`（imgui adapter 或 imgui 测试宿主任一有效开启时自动拉取）。
- 选项：`shared` / `fPIC` + `PI_BUILD_*` 开关树（adapter kits 与 tests 各有
  总开关 + 分开关，与 CMake 缓存选项同名，整批转发）。
- Qt 为**本地安装**（`C:/Qt/5.15.2/msvc2019_64`），非 conan 依赖。

### 8.2 CMake 层级

| 层 | 职责 |
|---|---|
| 根 `CMakeLists.txt` | 引入 pi 模块、`pi_init_glob_proj`、汇总子目录 |
| `cmake/pi/` | 项目自定义模块：语言标准、路径常量、编译选项、消息、文件分类 |
| `src/pipluginframework/` | 核心 SHARED 库 + export/config 安装 |
| `adapters/` | 两个 STATIC 套件（依赖不满足时自检禁用） |
| `tests/` | 三个宿主 + 两个插件，依赖不满足时优雅 DISABLED |

### 8.3 产物布局

- 核心库 → `<root>/lib/<CONFIG>/`（`.dll` + `.lib`，Debug 带 `d` 后缀）
- 测试宿主/插件 → `<root>/bin/<CONFIG>/`（install 阶段），构建树内也能直接运行
- Qt 运行时 DLL + `platforms/qwindows.dll` 自动复制到宿主/插件目录

## 9. 平台与移植

| 平台 | 支持 | 说明 |
|---|---|---|
| Windows | ✅ 完整 | msvc + Win32 + D3D11；`__stdcall` ABI |
| Linux | ◐ 框架层 | 编译路径就绪（dlopen），UI 嵌入未实现（X11 XEmbed TODO） |
| macOS | ◐ 框架层 | 编译路径就绪（dlopen/NSView 类型已定义），嵌入未实现 |

`PI_CALL` 在 Windows 定义为 `__stdcall`（最大 FFI 兼容），其他平台为空。