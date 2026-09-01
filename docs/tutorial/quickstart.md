# 快速上手（Quick Start）

本教程带你从一个全新的 shell 环境出发，把这套框架构建起来并跑通所有演示程序。
本文以 **Windows + Visual Studio 2022** 为例（这是当前唯一完整支持的平台）。

## 1. 前置条件

| 软件 | 版本要求 |
|---|---|
| Visual Studio | 2022（含 C++ 桌面开发工作负载，msvc v143） |
| CMake | ≥ 3.23 |
| Conan | 2.x（`pip install conan` 或官方安装器） |
| Qt | 5.15.2 msvc2019_64（本地安装，路径 `C:/Qt/5.15.2/msvc2019_64`） |
| Python | 3.x（Conan 运行需要） |

> Qt 路径在根 `CMakeLists.txt` 中硬编码为 `C:/Qt/5.15.2/msvc2019_64`。
> 若你的 Qt 装在其他位置，请修改该 `CMAKE_PREFIX_PATH` 项。

## 2. 构建步骤

### 2.1 准备编译环境

在 **Developer PowerShell / cmd**（或先执行 vcvars）中操作：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
```

### 2.2 安装依赖并生成构建文件

```bash
conan install . --build=missing
```

> - 首次运行会从 Conan Center 下载并**本地编译** `imgui/1.92.8`（Debug 配置没有现成预编译包），耗时几分钟。
> - 不需要 `--output-folder`：`conanfile.py` 的 `cmake_layout` 会自动管理输出位置与 `CMakeUserPresets.json`。

### 2.3 配置与构建

```bash
cmake --preset conan-default
cmake --build --preset conan-debug      # Debug 版
# 或 cmake --build --preset conan-release
```

### 2.4 输出

| 产物 | 位置 |
|---|---|
| `pipluginframeworkd.dll`（核心库） | `lib/Debug/` |
| 测试宿主 exe / 插件 dll | `build/tests/.../Debug/` 及 `bin/Debug/`（install 后） |
| `pi_test_host_headless.exe` | console 宿主（可独立运行验证） |

## 3. 运行演示

### 3.1 无头测试宿主（自动验证脚本友好）

```bash
cd bin/Debug
pi_test_host_headless.exe pi_test_plugin_qt.dll
```

输出应显示：宿主无 UI 能力探测、插件能力清单、能力门（requirement 检查）通过、
插件无头初始化成功、无视图创建、无 SERVICE 能力、300ms 循环后干净退出。

### 3.2 imgui 测试宿主 + 插件

```bash
cd bin/Debug
pi_test_host_imgui.exe pi_test_plugin_imgui.dll
```

启动一个 imgui + D3D11 窗口，左侧控制面板，右侧嵌入插件的 imgui 界面（滑动条、心跳动画）。

### 3.3 Qt 测试宿主 + 插件

```bash
cd bin/Debug
pi_test_host_qt.exe pi_test_plugin_imgui.dll
```

Qt 窗口内嵌入**imgui 插件**——演示"宿主与插件 UI 工具包不同"的交叉嵌入。
（反之，imgui 宿主加载 Qt 插件：`pi_test_host_imgui.exe pi_test_plugin_qt.dll`。）

## 4. 常见问题

### 4.1 `ERROR: Missing prebuilt package for 'imgui/1.92.8'`

你当前 profile（Debug / 特定 cppstd）在 Conan Center 没有现成二进制。解决：
`conan install . --build=missing`（推荐，只编译缺失的包；或 `--build=imgui/1.92.8` 指定单个）。

### 4.2 `Duplicate preset: "conan-default"`

根因：`CMakeUserPresets.json` 被 conan 注入了两个 include（通常是历史命令叠加
`--output-folder` 造成）。修复：

```bash
rm -rf build CMakeUserPresets.json   # Windows: Remove-Item -Recurse -Force build
conan install . --build=missing       # 重新生成单 include
```

### 4.3 `pi_test_host_imgui` 被 DISABLED

它的 Win32/DX11 backend 来自 `adapters/imgui/backends`，若找不到会以 WARNING 跳过。
运行 `conan install . --build=missing` 提供 imgui 后重新 configure 即可。

### 4.4 Qt 相关目标被 DISABLED

检查 `find_package(Qt5)` 是否能找到你的 Qt 安装（`CMAKE_PREFIX_PATH` 中的路径是否正确）。

## 5. 下一步

- 了解架构：阅读 `docs/design/architecture.md`
- 写第一个插件：阅读 `docs/tutorial/write-plugin.md`
- 使用 UI 适配器：阅读 `docs/tutorial/adapters.md`
- 规划路线：阅读 `docs/todo/`