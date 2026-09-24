[English](docs/README.en.md) | **简体中文**

# piplugin · 纯 C 插件框架

[![CI](https://github.com/Aeroscis/piplugin/actions/workflows/ci.yml/badge.svg)](https://github.com/Aeroscis/piplugin/actions/workflows/ci.yml)

> **仓库双址（有意）**：主仓库在 [Gitee](https://gitee.com/Aeroscis/piplugin)
> （CMake `HOMEPAGE_URL` 与 Conan `url` 指向它）；[GitHub](https://github.com/Aeroscis/piplugin)
> 是镜像，CI（Actions 与上方徽章）在镜像上运行。

宿主与插件之间只用**纯 C ABI**（COM 风格 vtable + LV2 风格能力协商）通信，因此插件可以用
任何语言写、用任何工具链编译，而宿主也不必知道插件内部用的是 Qt、imgui 还是裸 Win32。

> 这是 `piplugin` 自己的文档。设计取舍见 [`docs/design/`](docs/design/)，
> 教程见 [`docs/tutorial/`](docs/tutorial/)；本文件是中文版，英文版在
> [`docs/README.en.md`](docs/README.en.md)。

---

## 为什么需要它

现有的插件方案各自绑死在一件事上：

- **`QPluginLoader` / C++ 插件**：插件与宿主必须用同一编译器、同一标准库、同一 C++ ABI 编译。
  换个编译器或升级一次 MSVC，插件就加载不了。
- **LV2 / VST**：为音频宿主设计，能力协商很强，但面向的是"音频处理"这个具体领域。
- **本库**：把 LV2 的能力协商模型搬到**通用**场景 —— 任何需要"运行时加载外部模块，
  且模块可能自带 GUI"的宿主都能用，并且把 UI 工具包的差异留给**适配器套件**去吸收。

## 特性

| 特性 | 说明 |
|---|---|
| **纯 C ABI** | 接口是函数指针结构体（`PI_CALL`，Windows 上 `__stdcall`），不含 C++ 类型；Rust / C# / Python 的 FFI 可直接声明同样的结构 |
| **COM 风格生命周期** | `QueryInterface` / `AddRef` / `Release`，`IPiUnknown` 是根接口；接口发布后只增不改 |
| **LV2 风格能力协商** | descriptor 以 GUID 声明 `REQUIRED` / `OPTIONAL` / `PROVIDES`，宿主在**实例化之前**决定接受或拒绝 |
| **双向门禁** | 宿主既能检查"插件要求的能力我有没有"，也能要求"我的生态里插件必须具备某能力"（app 特化协议，无需 fork 框架） |
| **版本门禁** | 插件声明 `api_version`，major 不同或比宿主新则在 `create_instance` 前被拒（`PI_E_VERSIONMISMATCH`） |
| **GUI 与 headless 同一套 API** | headless 宿主不暴露 `IPiPluginHostUI`，把该能力标为 OPTIONAL 的插件自动降级为无界面运行 |
| **嵌入任意 GUI 工具包** | 插件返回自己的原生窗口句柄，宿主把它嵌进自己的容器；适配器套件吸收 Qt / imgui 的差异 |
| **零依赖核心** | 核心库只依赖平台自身的库，不需要 C++ 运行时，也不带任何第三方代码 |
| **宿主侧 kit** | 可选的静态库：把加载 / 门禁 / 七步卸载序列（`piplugin_host`）与 Qt、D3D11 的嵌入胶水（`piplugin_host_qt`、`piplugin_host_dx11`）固化下来 |
| **一条命令验收** | `ctest` 跑核心回归；`--cycles` 让宿主对任意插件 DLL 跑完整生命周期与尺寸往返 |

## 平台支持

| 平台 | 框架核心 | UI 适配器套件 | UI 嵌入 | 本仓库是否验证过 |
|---|---|---|---|---|
| **Windows** | 完整支持 | 完整支持（imgui / Qt） | 完整支持（Win32 子窗口 + flip-model 交换链） | **是** —— `ctest` + 一致性验收 + 像素级缩放回归全绿 |
| Linux | **CI 证明可编译**（核心 + 宿主 kit，gcc/clang） | 预期可编译（未构建） | **未实现**（X11 XEmbed 属路线图 FUT-01） | **部分** —— CI 的 `linux` job 构建核心与宿主 kit，并让 C++ 层用例在 Linux 上跑绿；插件 DLL 与 headless 宿主尚未非 Windows 构建（见 `docs/todo/platform.md` #3） |
| macOS | 预期可编译 | 预期可编译 | **未实现**（NSView 属路线图 FUT-02） | **否**（无 CI、无人工验证） |

**不做过度承诺**：v0.x 只有 Windows 是经过**完整**自动化验证的平台。Linux 上 CI 目前只做
核心与宿主 kit 的构建加 C++ 层用例（`.github/workflows/ci.yml` 的 `linux` job）：UI 适配器套件、
插件 DLL 与 headless 宿主在 Linux 上尚未被构建过，"预期可编译"指的就是这些部分仍停留在
代码里的平台分支（线程身份、动态库加载、符号可见性）。
完整声明见 [`docs/design/interface-freeze-review.md`](docs/design/interface-freeze-review.md) 第 5 节。

## 快速开始

### 方式一：Conan（推荐，会带上 imgui 依赖）

```bash
conan install . --build=missing          # 首次会本地编译 imgui，几分钟
cmake --preset conan-default
cmake --build --preset conan-debug
ctest --test-dir build -C Debug --output-on-failure
```

`conan install` 会在仓库根生成 `CMakeUserPresets.json`（它 `include` 生成器目录里的
conan 预设，`conan-default` 等就在那里）。那是**本地文件、故意不入库**：干净检出里
没有它，`cmake --preset` 才不会被一个不存在的 include 打断（见「方式二」）。

**Qt 相关目标（可选）**：Qt 是本地安装、不是 Conan 依赖，仓库里**没有**写死任何路径。
CMake 按这个顺序找它，任选一种即可：

```bash
# 1) 标准 CMake 方式（推荐：环境里已有 Qt 时通常什么都不用做）
cmake --preset conan-default -DQt5_DIR="C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5"
# 2) 本项目的便捷变量（前缀即可）
cmake --preset conan-default -DPI_QT_PREFIX="C:/Qt/5.15.2/msvc2019_64"
# 3) 或者干脆把 Qt 的 bin 放进 PATH / 设 CMAKE_PREFIX_PATH
```

找不到 Qt 时不会失败：Qt 套件、Qt 宿主与 Qt 测试件会打印一条带指引的提示后自动禁用
（`-DPI_QT_PREFIX=<路径>` / `-DQt5_DIR=<路径>/lib/cmake/Qt5`），其余目标照常构建。

### 方式二：纯 CMake（只要本机有 MSVC / Windows SDK）

不需要 Conan：仓库内置的 `CMakePresets.json` 提供通用预设（不含任何 conan 生成的
预设），找不到 Qt / imgui 时相关目标按下面的规则自动禁用，剩下核心库、host kit、
headless 宿主与控制台测试可用。

```powershell
cmake --preset default              # 构建目录 build/generic，与 conan 的 build/ 互不干扰
cmake --build --preset default      # Debug
ctest --preset default
```

- 追加缓存变量照常：`cmake --preset default -DPI_QT_PREFIX="C:/Qt/5.15.2/msvc2019_64"`；
- 非 Windows 用 `cmake --preset default-unix`（Ninja）；
- 默认安装前缀是 `<root>/build/install`（三条流程一致，见根 `CMakeLists.txt`）。

### 跑起来看看

```powershell
cd bin\Debug
.\pi_test_host_headless.exe pi_test_plugin_qt.dll     # 无头宿主：能力协商 + 完整生命周期
.\pi_test_host_imgui.exe  pi_test_plugin_qt.dll       # imgui 宿主：把 Qt 插件嵌进自己的窗口
.\pi_test_host_qt.exe     pi_test_plugin_imgui.dll    # Qt 宿主：把 imgui 插件嵌进 Qt
```

**想自己写一个？** [`examples/`](examples/README.md) 里有可构建运行的最小示范
（宿主、imgui 插件、Qt 插件、服务插件、特化 app），每个都是"三步跑通"的独立工程：

```powershell
.\pi_example_minimal_host.exe pi_example_plugin_imgui.dll
.\pi_example_specialized_app.exe pi_example_specialized_plugin.dll pi_example_service.dll
```

宿主侧 kit 的用法见 [`docs/tutorial/write-host.md`](docs/tutorial/write-host.md)，
插件见 [`docs/tutorial/write-plugin.md`](docs/tutorial/write-plugin.md)，
适配器套件见 [`docs/tutorial/adapters.md`](docs/tutorial/adapters.md)；
**宿主本身是 Qt 程序**时见 [`docs/tutorial/qt-host-direct.md`](docs/tutorial/qt-host-direct.md)
（不要用 Qt 套件，改直连：`examples/qt_host_direct/`）。

## 架构

```
宿主 (Host)
  ├─ GUI Host        （imgui / Qt / 裸 Win32）
  ├─ Headless Host   （任务服务器 / CLI；不暴露 IPiPluginHostUI，插件自动降级）
  └─ 任意宿主
        │  pi_plugin_module_load / pi_plugin_host_create_plugin
        ▼  动态加载（LoadLibrary / dlopen）
插件 DLL（.dll / .so / .dylib）
  └─ pi_plugin_entry() → IPiPluginFactory
       ├─ PiPluginDescriptor（名称 / 版本 / 能力声明）
       └─ CreateInstance → IPiPluginBase
            ├─ IPiPluginView（GUI 插件：attach / on_idle / on_resize）
            └─ IPiPluginService（headless / 服务插件）
```

完整架构、接口族与线程模型见
[`docs/design/architecture.md`](docs/design/architecture.md) 与
[`docs/design/interfaces.md`](docs/design/interfaces.md)。

| 目录 | 内容 |
|---|---|
| `include/piplugin/` | 公共头文件（`pi_plugin.h` 是总入口） |
| `src/` | 框架核心 C 实现 |
| `adapters/` | 插件侧 UI 适配器套件（`qt/`、`imgui/`） |
| `host_kits/` | 宿主侧 kit（`core/` 会话、`events/` 事件路由、`qt/` 与 `dx11/` 嵌入胶水） |
| `examples/` | 可直接构建运行的最小示范（宿主 / imgui 插件 / Qt 插件 / 服务插件 / 特化 app / FFI / 插件发现…，见 [`examples/README.md`](examples/README.md)） |
| `tests/` | 测试宿主、测试插件与单元测试 |
| `docs/` | 设计文档与教程 |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | 文档分层、待办归档写法、语言分工与提交纪律 |

## 生态位对比

| | 本库 | `QPluginLoader` 等 C++ 插件 | LV2 / VST |
|---|---|---|---|
| 插件语言 | 任意（纯 C ABI） | 必须同一 C++ ABI | 任意（C ABI） |
| 编译器升级 | 不受影响 | 需全部重编 | 不受影响 |
| 能力协商 | GUID 声明 + 实例化前门禁 | 无 | GUID 声明（本库借鉴自它） |
| 宿主自带 GUI 工具包 | 可嵌入任意工具包的原生窗口 | 通常要求插件用同一工具包 | 面向音频插件 GUI |
| 适用领域 | 通用：任何"运行时加载外部模块"的宿主 | 桌面应用 | 音频处理 |
| 依赖 | 无（核心零依赖） | 无 | 视宿主而定 |

## 测试与验收

| 入口 | 内容 |
|---|---|
| `scripts/verify.ps1` | **一条命令跑完全部检查**（CI 调用的就是它，共 5 项）：`ctest` + 一致性验收 + FFI 示例（Python/Rust/C#）+ clang-format 漂移报告（只报告）+ **文档-仓库漂移检查**（强制） |
| `ctest -C Debug` | 21 个用例：核心单测 `unit`（210 项断言）、`unit_threads`（70）、`unit_cpp`（52，含 CRT 泄漏断言）、headless 冒烟、版本门禁负向用例 |
| `scripts/run_selftest.ps1` | **一致性验收**：对每个给定的插件 DLL 跑 `load → attach → idle → 尺寸往返 → unload`，退出码裁决 |
| `scripts/verify_resize_fix.ps1` | 缩放修复的**像素级**回归（截图量测面板与插件边缘是否恒定） |

CI（GitHub Actions，见 [`.github/workflows/ci.yml`](.github/workflows/ci.yml)）在每次 push 与 PR 上
只做"装依赖 + 构建"，然后把检查全部交给 `scripts/verify.ps1` —— 这样检查项能在本地复现。

## 生态

**约定：跑过 `--cycles`（一致性验收，退出码 0）= 可以列入下表。**

| 适配器套件 | 方向 | 状态 |
|---|---|---|
| `piplugin_qt` | 插件侧：Qt 控件插件 | 本仓库内置 |
| `piplugin_imgui` | 插件侧：立即模式 UI 插件 | 本仓库内置 |

社区适配器与插件欢迎补进这张表；交付规范见
[`docs/design/conformance.md`](docs/design/conformance.md)。

## 版本与兼容

- 当前为 **0.x**：接口仍可能变动。`y` 版本只修问题，`x` 版本可能改行为或 API，
  每次都会写进 [`CHANGELOG.md`](CHANGELOG.md)。
- `1.0.0` 留给"ABI 冻结承诺"的时刻：从那时起 vtbl 只增不改。
- 插件在自己的 descriptor 里声明 `PI_PLUGIN_API_VERSION`；宿主据此在实例化前门禁。
  规则见 [`docs/design/interfaces.md`](docs/design/interfaces.md) 1.5。

## 许可与第三方

[MIT](LICENSE) © 2026 Aeroscis。

第三方代码与依赖（vendored 的 Dear ImGui Win32/DX11 backend、Conan 的 imgui 核心、可选的 Qt）
逐条记录在 [`CREDITS.md`](CREDITS.md)，各自的许可证与版权归其作者。
