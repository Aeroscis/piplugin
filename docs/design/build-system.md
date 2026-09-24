# 构建系统设计

## 1. 技术栈

| 组件 | 版本 | 角色 |
|---|---|---|
| CMake | ≥ 3.23 | 构建系统 |
| Conan | 2.x | 依赖管理与打包（仅 imgui） |
| Visual Studio | 2022 (msvc v143 / 194) | Windows 工具链 |
| Qt | 5.15.2 (msvc2019_64，本地安装) | Qt 套件 / 测试宿主依赖 |

## 2. Conan 2 配方（conanfile.py）

```python
class PiPluginConan(ConanFile):
    name = "piplugin"
    version = "0.4.0"
    settings = "os", "compiler", "build_type", "arch"
    # 开关树：选项与 CMake 缓存选项同名（PI_BUILD_*），generate() 整批转发给 CMake
    options = {
        "shared": [True, False], "fPIC": [True, False],
        "PI_BUILD_ADAPTERS": [True, False],        # adapter kits 总开关
        "PI_BUILD_ADAPTER_QT": [True, False],      # qt adapter 分开关（Qt5 本地安装）
        "PI_BUILD_ADAPTER_IMGUI": [True, False],   # imgui adapter 分开关（conan imgui）
        "PI_BUILD_TESTS": [True, False],           # 测试件总开关
        "PI_BUILD_TEST_HOST": [True, False], "PI_BUILD_TEST_HOST_QT": [True, False],
        "PI_BUILD_HEADLESS_HOST": [True, False],
        "PI_BUILD_TEST_PLUGIN": [True, False], "PI_BUILD_TEST_PLUGIN_IMGUI": [True, False],
    }
    generators = "CMakeDeps"   # CMakeToolchain 由 generate() 手动实例化（注入 conf/env 路径）
```

要点：

- **`cmake_layout(self)`**：conan 会据此自动生成并维护 `CMakeUserPresets.json`。
  **不要**在 `conan install` 时叠加 `--output-folder`，否则会生成重复预设
  （`Duplicate preset: conan-default` 就是这个原因）。
- **开关树**：核心必编（无开关）；adapter kits 与 tests 各有总开关 + 分开关，
  选项与 CMake 缓存选项同名，`generate()` 整批透传（Conan bool 自动转 ON/OFF）。
  有效状态 = 总开关 AND 分开关（总开关关死即整组关闭，与 CMake 侧守卫同构）。
- **依赖自动管理**：imgui adapter 或 imgui 测试宿主任一有效开启，`requirements()`
  自动拉取 `imgui/1.92.8`；全关则依赖图中完全没有 imgui。
- **矛盾显式报错**：有效开启的测试件依赖某 adapter kit 而该 kit 有效关闭时，
  `validate()` 列出全部冲突项并报错（取代旧的"静默降级"）。
- **conf 路径注入**：`generate()` 优先读 `user.cmake_ext:cmake_prefix_path` 等
  自定义 conf（标准 profile 的 `[conf]` 段），无 conf 时降级读同名环境变量，
  注入 CMake 工具链 cache 变量。
- Qt 是本地安装（`C:/Qt/5.15.2/msvc2019_64`），在根 CMakeLists 的 `CMAKE_PREFIX_PATH` 中追加。

### 2.1 标准命令序列

```bash
conan install . --build=missing     # 生成构建文件 + 管理 CMakeUserPresets.json
cmake --preset conan-default        # configure（VS 2022, x64）
cmake --build --preset conan-debug  # 构建 Debug
cmake --build --preset conan-release
```

## 3. CMake 层级结构

### 3.1 根 CMakeLists.txt

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pi/pi.cmake)   # 引入 pi 模块
project(piplugin VERSION 0.4.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    pi_init_glob_proj(CXX 17 C 11 REQUIRED)   # 顶层工程才做全局初始化
endif()

add_subdirectory(${GLOBAL_PROJECT_SRC_PATH})  # src/
add_subdirectory(.../adapters)                # adapters/
option(PI_BUILD_TESTS "..." ON)
add_subdirectory(${GLOBAL_PROJECT_TESTS_PATH})# tests/
```

### 3.2 cmake/pi 模块（项目专用）

| 文件 | 内容 |
|---|---|
| `pi.cmake` | 入口，include 其余四个模块（`include_guard(GLOBAL)`） |
| `pi_project.cmake` | `pi_init_glob_proj`：语言标准、全局路径常量、编译器警告、Windows 最佳实践 |
| `pi_message.cmake` | `pi_proj_msg` / `pi_proj_err` / `pi_tar_msg` / `pi_glob_proj_msg` |
| `pi_debug.cmake` | `pi_exam_var`：打印变量的值（支持列表格式化） |
| `pi_file_handling.cmake` | `pi_classify_cpp_files` / `pi_extract_public_headers` / `pi_conv_abs_paths_to_rel_paths`（按 `_p.` 约定分私有头） |

`pi_init_glob_proj(CXX 17 C 11 REQUIRED)` 建立的全局常量：

| 变量 | 值 |
|---|---|
| `GLOBAL_PROJECT_PATH` | `PROJECT_SOURCE_DIR` |
| `GLOBAL_PROJECT_BIN_PATH` | `<root>/bin` |
| `GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH` | `<root>/bin/$<CONFIG>`（生成器表达式） |
| `GLOBAL_PROJECT_SRC_PATH` | `<root>/src` |
| `GLOBAL_PROJECT_TESTS_PATH` | `<root>/tests` |
| `GLOBAL_PROJECT_BUILD_TYPE_SUFFIX` | `$<$<CONFIG:Debug>:d>`（Debug 库名带 `d`） |

编译器警告策略（`pi_init_glob_proj` 内）：

| 编译器 | 选项 |
|---|---|
| MSVC | `/W4 /permissive- /Zc:__cplusplus /utf-8` |
| Clang | `-Wall -Wextra -Wpedantic` |
| GNU | `-Wall -Wextra --pedantic-errors` |

Windows 全局：`WIN32_LEAN_AND_MEAN`。

> 刻意**不**用 `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`：它会把 CRT 内部符号一并导出
> （实测漏出 `__local_stdio_printf_options`、`snprintf`、`vsnprintf`）。公开 API
> 一律显式 `PI_PLUGIN_API`，插件入口显式 `__declspec(dllexport)`，见
> `docs/design/interface-freeze-review.md`。

### 3.3 核心库（src/piplugin/CMakeLists.txt）

- `add_library(piplugin SHARED)` + 别名 `pi::piplugin`
- 源文件：`src/pi_plugin_host.c`、`src/pi_plugin_unknown.c`（C11）
- PUBLIC include：`<root>/include`（build 接口 `FILE_SET HEADERS`）
- Debug 输出名带 `d` 后缀（`GLOBAL_PROJECT_BUILD_TYPE_SUFFIX`）
- 产物输出到 `<root>/lib/<CONFIG>/`
- 安装：export `pipluginTargets.cmake`（namespace `pi::`）+ `pipluginConfig.cmake`
- 伞配置 `piConfig.cmake` → include `pipluginConfig.cmake`

### 3.4 适配器套件（adapters/）

公共模式（以 imgui 为例）：

```cmake
find_package(imgui CONFIG QUIET)          # 依赖自检
if(NOT imgui_FOUND)
    message(STATUS "imgui not found -> ${TARGET_NAME} disabled")
    return()                              # 优雅禁用
endif()
add_library(piplugin_imgui STATIC)
target_link_libraries(... PUBLIC piplugin imgui::imgui)
```

| 套件 | 依赖 | 形态 | 状态 |
|---|---|---|---|
| `piplugin_qt` | Qt5 Widgets | **SHARED**（APP-08：进程内共享一个 `QApplication`） | 找不到 Qt5 则禁用 |
| `piplugin_imgui` | imgui (conan) | STATIC | 找不到 imgui 则禁用 |

总开关 `PI_BUILD_ADAPTERS`（Conan 侧同名选项透传）：关死时所有 kit 一律不编。
每个 kit 另有独立安装规则：库 → `lib/<CONFIG>/`（SHARED 的 Windows 运行时 DLL 走
`bin/<CONFIG>/`），公共头 → `include/piplugin/adapters/<kit>/`，导出目标 → 独立
`piplugin<Kit>AdapterTargets.cmake`（由伞配置按存在性挂接）。
SHARED 的 Qt 套件 DLL 与核心库一样在 POST_BUILD 阶段自动部署到 `bin/<CONFIG>/`，
插件运行时必须能找到它。

**Qt5 怎么被找到（ECO-05）**：仓库不写死任何 Qt 路径。根 `CMakeLists.txt` 暴露缓存变量
`PI_QT_PREFIX`（默认空）；查找顺序是 `PI_QT_PREFIX` → `Qt5_DIR` → `CMAKE_PREFIX_PATH`
（含环境变量）→ Windows 上 PATH 里的 Qt。只有以上都没给线索、且本机常见的
`C:/Qt/5.15.2/msvc2019_64` 恰好存在时，才把它当**提示**用一次并打印说明。
找不到 Qt 时，四处 Qt 相关目标各自打印一条带指引的消息（`PI_QT_MISSING_HINT`）后禁用，
configure 仍然成功 —— 别人给出自己的路径即可构建 Qt 目标，不必改仓库文件。

### 3.5 例子（examples/）

roadmap ECO-03：每个例子一个目录、一个 `CMakeLists.txt`、一份 README（三步跑通），
只依赖公开 API，不引用 `tests/`。开关 `PI_BUILD_EXAMPLES`（默认 ON；Conan 侧同名选项）。

| 目标 | 依赖 | 类型 |
|---|---|---|
| `pi_example_minimal_host` | 核心 + 宿主 kit L0 | exe（console + 一个窗口） |
| `pi_example_service` | 仅核心 | dll（纯 C 服务插件） |
| `pi_example_plugin_imgui` | imgui + imgui 套件 | dll |
| `pi_example_plugin_qt` | Qt5 + Qt 套件（SHARED） | dll（仅 Windows） |
| `pi_example_kit_win32` / `pi_plugin_example_plugin_win32` | 仅核心（Windows） | STATIC 套件 + dll（ECO-01 的可执行附录：照 `docs/design/adapter-spec.md` 写的最小套件） |
| `pi_example_specialized_plugin` / `pi_example_specialized_app` | 核心 + 宿主 kit L0 | dll + exe（通道 A/B 示范） |

其中两个无 GUI 工具包依赖的例子同时注册为 ctest（`example_minimal_host_service`、
`example_specialized_app`）；GUI 例子留给 README 的人工三步。

`examples/ffi/`（ECO-06）不在 CMake 里：它是 Python / Rust / C# 三个独立工程，
由 `scripts/verify_ffi.ps1` 驱动（`scripts/verify.ps1` 的第 3 项检查，缺工具链则 SKIP）。

### 3.6 打包与安装（roadmap ECO-04）

两种分发形态，同一套目标名（`pi::piplugin` / `pi::piplugin_host` / `pi::piplugin_events` /
`pi::piplugin_imgui` / `pi::piplugin_qt` / `pi::piplugin_host_qt` / `pi::piplugin_host_dx11`）：

| 形态 | 产生方式 | 消费方入口 |
|---|---|---|
| **安装树** | `cmake --install build --prefix <前缀>` | `find_package(piplugin)`（旧入口 `find_package(pi)` 仍可用，见 `src/cmake/piForwardConfig.cmake.in`） |
| **Conan 包** | `conan create .` | `conanfile.txt` 里写 `piplugin/<版本>` |

要点（都是 ECO-04 修出来的真问题）：

1. **配置放在包名目录**：`lib/cmake/piplugin/`。`find_package(<name>)` 只搜索
   `<prefix>/lib/cmake/<name>*/`，放在 `cmake/pi/` 下的配置对 `find_package(piplugin)`
   是不可见的（旧入口因此只保留一个转发文件）；
2. **伞配置按组件按需导入**（`src/cmake/piConfig.cmake.in`）：消费方明确要求某组件时，
   它的第三方依赖是硬 `find_dependency`；只是"恰好装在同一前缀里"的组件，仅在依赖已能找到时
   顺手导入，否则打印 STATUS 跳过 —— 于是只想用核心库的消费方不会因为这台机器没装 Qt5 而配置失败；
3. **`package_info()` 必须描述包里有什么**，而不是选项说了什么：CMake 侧可以静默禁用 target
   （找不到 Qt5 时 Qt 系列整批禁用，见 ECO-05），此时若仍声明该组件，消费方会拿到
   `Library 'xxx' not found in package`。`conanfile.py::_packaged()` 逐个核对产物；
4. **组件不继承包级 `libdirs/bindirs/includedirs`**：组件必须各自设置，否则 CMakeDeps
   生成 `<pkg>/lib`（库里在 `<pkg>/lib/Debug`）、且 kit 头文件目录缺失；
5. **Qt5 是本地安装依赖**，不进 conan `requires`（否则等于强迫所有人用 conan 版 Qt）；
   包只在组件被明确要求时才硬依赖它。
6. **静态 kit 的平台库要写进 `cpp_info.system_libs`**：imgui/dx11 套件以 PRIVATE/PUBLIC 链接
   `user32 d3d11 dxgi d3dcompiler`，静态库的消费方链接期仍然需要它们 —— CMake 导出 target
   自带，CMakeDeps 只能靠包信息（漏了报 `unresolved external D3D11CreateDeviceAndSwapChain`）；
7. **组件级"外部 require"在 Conan 2.10 + CMakeDeps 下需要消费方也声明该依赖**才会被传播，
   否则被静默丢弃（Conan 侧 `get_deps_targets_names()` 取不到就 `except KeyError: pass`）：
   消费方 conanfile 里要有 `imgui/<版本>`，`pi::piplugin_imgui` 才会带上 `imgui::imgui`。
   裸 CMake 安装树没有这个限制。

验收脚本：`scripts/verify_package.ps1`（A 安装树 + B conan 包，各自 `find_package` → 构建 →
**运行** `examples/conan_consumer/`，两种形态都链接 imgui 套件）。
实测细节、证据表与 Conan 侧代码位置见
[`examples/conan_consumer/README.md`](../../examples/conan_consumer/README.md)。

### 3.7 测试（tests/）

可选目标，各自做依赖自检，不满足即 `return()` 禁用（不影响整体构建）：

| 目标 | 依赖 | 类型 |
|---|---|---|
| `pi_test_host_imgui` | imgui + backends | exe（Win32） |
| `pi_test_host_qt` | Qt5 + imgui 套件 | exe（Win32） |
| `pi_test_host_headless` | 仅核心 | exe（console，纯 C） |
| `pi_test_host_multi` | 仅核心 + 宿主 kit L0 | exe（console，APP-08 多插件同进程验收） |
| `pi_test_host_events` | 仅核心 + 宿主 kit L0/events | exe（console，APP-06 事件验收） |
| `pi_test_plugin_qt` / `pi_test_plugin_qt2` | Qt5 + Qt 套件（SHARED） | dll（仅 Windows，同一份源码两个变体） |
| `pi_test_plugin_service` | 仅核心 | dll（纯 C 服务插件，APP-07） |
| `pi_test_plugin_events` | 仅核心 | dll（纯 C 事件插件，APP-06） |
| `pi_test_plugin_imgui` | imgui + imgui 套件 | dll |

Qt 运行时部署：宿主/插件构建后自动复制 `Qt5Core/Gui/Widgets.dll` + `platforms/qwindows.dll`
到目标目录；install 时也一并安装到 `<root>/bin/<CONFIG>`。
SHARED 的 `piplugin_qt` 套件 DLL 同样由自身 POST_BUILD 部署到 `bin/<CONFIG>/`。

## 4. 预设：仓库内的 `CMakePresets.json` + conan 生成的 `CMakeUserPresets.json`

两件事分开说，因为它们解决的是不同问题（W-09）：

**仓库内置 `CMakePresets.json`**（入库）——不依赖 conan 的通用入口：

- `default`：VS 2022 / x64，构建目录 `build/generic`（与 conan 的 `build/` 互不干扰）；
- `default-unix`：Ninja + 单配置 Debug（非 Windows）；
- 配套 build / test 预设同名；缓存变量只声明 `PI_BUILD_TESTS` / `PI_BUILD_EXAMPLES`，
  其余交给默认值 + "找不到依赖即禁用"的既有逻辑（缺 Qt/imgui 时相关目标自己打印提示并跳过）；
- 可追加缓存变量，例如 `cmake --preset default -DPI_QT_PREFIX="C:/Qt/5.15.2/msvc2019_64"`。

**`CMakeUserPresets.json`（conan 生成，不入库）**：

- 该文件由 **conan 自动生成/维护**（带 `"vendor": {"conan": {}}` 标记），**不要手工编辑**；
- 它 `include` 生成器目录里的 `CMakePresets.json`（其中定义 `conan-default` 等预设）；
- 它**故意不进版本库**（`.gitignore`）：那个 include 指向的文件只有在跑过 `conan install`
  之后才存在，而 CMake 读不到 include 的文件时是**硬失败**（`Could not read presets ...
  File not found`）——把它入库会让"干净检出 + `cmake --preset`"直接不可用，这正是 W-09
  要修的场景；
- 两个文件的关系受 CMake 的可见性规则约束：user presets 可以继承仓库预设，反过来不行
  （仓库预设不能继承 conan 预设，报 `Inherited preset ... is unreachable from preset's
  file`）。因此需要 conan 预设作为基类的便捷预设只能待在 `CMakeUserPresets.json` 里 ——
  原先 `conan-default-local`（把安装前缀钉在 `<root>/build/install`）承担的默认值已改由
  根 `CMakeLists.txt` 在 `CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT` 时给出，三条流程
  （conan / 纯 CMake / cpack）一致；
- 若因历史命令（叠加 `--output-folder`）产生重复 include，修复方式是：
  删除 `build/` 与 `CMakeUserPresets.json`，然后用标准 `conan install . --build=missing` 重新生成。

## 5. 安装与消费

```bash
cmake --install build --config Debug --prefix <prefix>
```

或直接使用 CMake package（同一仓库内）：

```cmake
find_package(pi CONFIG REQUIRED)
target_link_libraries(app PRIVATE pi::piplugin)
# 套件目标随安装树自动可用（该套件开关开启时才安装/导出）：
target_link_libraries(app PRIVATE pi::piplugin_imgui)
```

Conan 打包（adapters 已随核心一并打包，测试件不进包）：

```bash
conan create . -pr MSVC2022-amd64-Cpp17-Debug -o PI_BUILD_TESTS=False
```

包内容：核心 `bin/<CONFIG>/` + `lib/<CONFIG>/` + `include/piplugin/`、
adapter 静态库与公共头（`include/piplugin/adapters/<kit>/`）、
cmake 配置（伞配置 `piConfig.cmake` 按存在性挂接各 `*AdapterTargets.cmake`，
安装了哪个套件就自动暴露哪个目标）、以及根文档（`LICENSE` / `README.md` / `CHANGELOG.md`）。
消费方经 CMakeDeps 使用 `pi::piplugin` / `pi::piplugin_imgui` 等目标。

归档打包（cpack，roadmap W-08）：

```bash
cmake --build build --config Debug            # 至少构建一次（cpack 从构建树取产物）
cpack --config build/CPackConfig.cmake -C Debug -B out
# -> out/piplugin-0.4.0-Debug-win64.zip
```

- `CPACK_PACKAGING_INSTALL_PREFIX` 置空，所以解压出来就是 `bin/`、`lib/`、`include/`
  外加 `LICENSE`/`README.md`/`CHANGELOG.md`（Windows 上不清空的话整棵树会嵌在
  `/Program Files/piplugin/` 下面）；
- 归档名里的配置由 `cmake/CPackProjectConfig.cmake` 在**打包时**拼上：
  `CPACK_PACKAGE_FILE_NAME` 在 configure 阶段拿不到多配置生成器的配置，也不支持生成器
  表达式（写成尖括号表达式会得到一个含 `<` `>` 的非法目录名而直接失败）；
- `-DPI_CPACK_NSIS=ON` 可追加 NSIS 安装包（需本机装 NSIS；默认关，免得没装 NSIS 的机器
  上 cpack 直接失败）；
- 归档的自洽性由 `scripts/verify_package.ps1` 的 **C 段**断言：解压到干净目录后，**不带**
  conan 工具链、PATH 上没有任何本仓库路径，仍要能配置并构建出宿主程序并运行成功。
  注意归档里**不含** Qt 运行时（Qt 在本项目是本地安装、不是 conan 依赖，各 kit 的使用
  约定就是"消费方自行保证 Qt 可达"），所以"宿主直接运行"这条断言覆盖的是不依赖 Qt 的那
  部分链接面；要把 Qt 宿主也做成"解压即跑"，得先把 Qt 运行时纳入分发（涉及 LGPL 再分发
  的决策，见 `docs/todo/build.md` #8）。