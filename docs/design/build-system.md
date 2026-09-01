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
    options = { "shared": [True, False], "fPIC": [True, False], "with_tests": [True, False] }
    generators = "CMakeToolchain", "CMakeDeps"

    def layout(self):
        cmake_layout(self)          # conan 自动管理生成器路径与 CMakeUserPresets.json

    def requirements(self):
        if self.options.with_tests:
            self.requires("imgui/1.92.8")
```

要点：

- **`cmake_layout(self)`**：conan 会据此自动生成并维护 `CMakeUserPresets.json`。
  **不要**在 `conan install` 时叠加 `--output-folder`，否则会生成重复预设
  （`Duplicate preset: conan-default` 就是这个原因）。
- `imgui` 只在 `with_tests=True` 时需要（测试插件与套件用）。
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
```

若需同时获得套件，可扩展 conan 配方将 adapters 一并打包（见 TODO）。