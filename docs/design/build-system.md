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
class PiPluginFrameworkConan(ConanFile):
    name = "pipluginframework"
    version = "1.0.0"
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
project(pipluginframework VERSION 1.0.0 LANGUAGES C CXX)

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

Windows 全局：`WIN32_LEAN_AND_MEAN`、`CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON`。

### 3.3 核心库（src/pipluginframework/CMakeLists.txt）

- `add_library(pipluginframework SHARED)` + 别名 `pi::pipluginframework`
- 源文件：`src/pi_plugin_host.c`、`src/pi_plugin_unknown.c`（C11）
- PUBLIC include：`<root>/include`（build 接口 `FILE_SET HEADERS`）
- Debug 输出名带 `d` 后缀（`GLOBAL_PROJECT_BUILD_TYPE_SUFFIX`）
- 产物输出到 `<root>/lib/<CONFIG>/`
- 安装：export `pipluginframeworkTargets.cmake`（namespace `pi::`）+ `pipluginframeworkConfig.cmake`
- 伞配置 `piConfig.cmake` → include `pipluginframeworkConfig.cmake`

### 3.4 适配器套件（adapters/）

公共模式（以 imgui 为例）：

```cmake
find_package(imgui CONFIG QUIET)          # 依赖自检
if(NOT imgui_FOUND)
    message(STATUS "imgui not found -> ${TARGET_NAME} disabled")
    return()                              # 优雅禁用
endif()
add_library(pipluginframework_imgui STATIC)
target_link_libraries(... PUBLIC pipluginframework imgui::imgui)
```

| 套件 | 依赖 | 状态 |
|---|---|---|
| `pipluginframework_qt` | Qt5 Widgets | 找不到 Qt5 则禁用 |
| `pipluginframework_imgui` | imgui (conan) | 找不到 imgui 则禁用 |

总开关 `PI_BUILD_ADAPTERS`（Conan 侧同名选项透传）：关死时所有 kit 一律不编。
每个 kit 另有独立安装规则：静态库 → `lib/<CONFIG>/`，公共头 →
`include/pipluginframework/adapters/<kit>/`，导出目标 → 独立
`pipluginframework<Kit>AdapterTargets.cmake`（由伞配置按存在性挂接）。

### 3.5 测试（tests/）

五个可选目标，各自做依赖自检，不满足即 `return()` 禁用（不影响整体构建）：

| 目标 | 依赖 | 类型 |
|---|---|---|
| `pi_test_host_imgui` | imgui + backends | exe（Win32） |
| `pi_test_host_qt` | Qt5 + imgui 套件 | exe（Win32） |
| `pi_test_host_headless` | 仅核心 | exe（console，纯 C） |
| `pi_test_plugin_qt` | Qt5 + Qt 套件 | dll（仅 Windows） |
| `pi_test_plugin_imgui` | imgui + imgui 套件 | dll |

Qt 运行时部署：宿主/插件构建后自动复制 `Qt5Core/Gui/Widgets.dll` + `platforms/qwindows.dll`
到目标目录；install 时也一并安装到 `<root>/bin/<CONFIG>`。

## 4. `CMakeUserPresets.json` 说明

- 该文件由 **conan 自动生成/维护**（带 `"vendor": {"conan": {}}` 标记），**不要手工编辑**。
- 它 `include` 生成器目录里的 `CMakePresets.json`（其中定义 `conan-default` 等预设）。
- 若因历史命令（叠加 `--output-folder`）产生重复 include，修复方式是：
  删除 `build/` 与 `CMakeUserPresets.json`，然后用标准 `conan install . --build=missing` 重新生成。

## 5. 安装与消费

```bash
cmake --install build --config Debug --prefix <prefix>
```

或直接使用 CMake package（同一仓库内）：

```cmake
find_package(pi CONFIG REQUIRED)
target_link_libraries(app PRIVATE pi::pipluginframework)
# 套件目标随安装树自动可用（该套件开关开启时才安装/导出）：
target_link_libraries(app PRIVATE pi::pipluginframework_imgui)
```

Conan 打包（adapters 已随核心一并打包，测试件不进包）：

```bash
conan create . -pr MSVC2022-amd64-Cpp17-Debug -o PI_BUILD_TESTS=False
```

包内容：核心 `bin/<CONFIG>/` + `lib/<CONFIG>/` + `include/pipluginframework/`、
adapter 静态库与公共头（`include/pipluginframework/adapters/<kit>/`）、
cmake 配置（伞配置 `piConfig.cmake` 按存在性挂接各 `*AdapterTargets.cmake`，
安装了哪个套件就自动暴露哪个目标）。消费方经 CMakeDeps 使用
`pi::pipluginframework` / `pi::pipluginframework_imgui` 等目标。