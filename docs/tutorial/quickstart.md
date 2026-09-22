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
| `piplugind.dll`（核心库） | `lib/Debug/` |
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

## 4. 自动化验证

手工点击之外，仓库自带几种无人值守的验证方式，改完代码后建议都跑一遍。

### 4.1 核心回归（`ctest`）

一条命令跑完核心单元测试与 headless 冒烟测试：

```bash
ctest --test-dir build -C Debug --output-on-failure
```

| 用例 | 内容 |
|---|---|
| `unit` | `pi_guid_equal`、descriptor 帮助函数、`PiRefCountedBase` 引用计数与 destroy 回调、`pi_module_load` 失败路径、`pi_api_version_compatible` 边界、默认宿主服务的 headless / GUI 两形态 |
| `headless_host_smoke` | headless 宿主加载真实插件跑完整个生命周期 |
| `version_gate_rejects_incompatible_plugin` | 声明不兼容 `api_version` 的插件必须在实例化之前被拒（负向用例） |

前两个用例**只按退出码判定**（0 = 通过）；第三个刻意反过来 —— "被拒绝"就是期望结果，
所以断言的是宿主输出里出现拒绝理由。失败细节靠 `--output-on-failure` 打印。

### 4.2 插件生命周期自测（`--cycles`）

```bash
cd bin/Debug
pi_test_host_imgui.exe --cycles 3 --plugin pi_test_plugin_qt.dll --idle-frames 12
echo %errorlevel%        # 0 = PASS
```

跑真实的 load → attach → idle 帧 → 尺寸往返 → detach → release → unload 循环，
用真实渲染循环驱动（能暴露 Qt 控件析构与模块卸载竞态这类只在事件循环转起来后才出现的崩溃）。
`--plugin` 接受逗号分隔的列表，逐个插件都跑完整轮回。日志末尾会出现
`selftest: PASS (N plugin(s) x M cycles)`。也可封装为（默认就跑两个官方插件）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run_selftest.ps1 -Cycles 3
```

这就是 roadmap 里的 conformance harness，详见 `docs/design/conformance.md`。

### 4.3 截图证明（`--screenshot`）

```bash
pi_test_host_imgui.exe pi_test_plugin_qt.dll --screenshot shot.bmp
```

加载插件、渲染若干帧后把**合成后的窗口**截成 bmp 并退出，
用于自动证明"嵌入的插件确实盖在 D3D 帧之上可见"。

### 4.4 缩放回归（`scripts/verify_resize_fix.ps1`）

程序化 `SetWindowPos` 分步放大/缩小 + `PrintWindow` 截图 + 像素扫描，
断言面板与插件区的几何在多轮缩放后保持不变：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify_resize_fix.ps1
```

它同时断言日志中出现 `scale:none`、缓冲按需增长、无 `ResizeBuffers` 失败。
详见 `docs/design/d3d-window-resizing.md`。

> **注意**：以上只能覆盖缩放路径的**稳态**正确性。
> "拖动过程中是否还有可感知闪烁"属于**瞬态**问题，需人工快速拖动确认。

### 4.5 持续集成（GitHub Actions）

`.github/workflows/ci.yml` 在 push 到 `main` 与所有 PR 上运行（`windows-latest` + MSVC + conan）：

1. `conan install` —— **Qt 三件套显式关闭**（Qt5 是本工程的本地安装依赖，runner 上没有；去硬编码属 ECO-05）；
2. 构建核心库 + headless 宿主 + 单元测试 + imgui 宿主；
3. `ctest` —— 核心单测 + 版本门禁负向用例（Qt 插件关掉后，headless 冒烟用例按 CMake 守卫自动不注册）；
4. conformance harness —— imgui 插件跑完整生命周期 + 尺寸往返（`--cycles` 不依赖 GPU：拿不到硬件设备时宿主回退 WARP）；
5. clang-format 漂移**以信息方式报告**：当前全仓 C/C++ 文件都不符合已提交的 `.clang-format`
   （含 include 排序与缩进/wrap 漂移），一次性重排会产生淹没评审的巨型 diff，
   故本轮"暴露但不阻塞"；收紧方式见工作流内该步骤的注释。

失败时会把 `bin/Debug/*.log` 与 `build/selftest.txt` 作为 artifact 上传，便于定位。
README 就位（BLK-02）后在 README 挂 badge。

## 5. 常见问题

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

### 4.5 MSBuild 报 `MSB6001 ... 字典中的关键字:"HTTPS_PROXY"所添加的关键字:"https_proxy"`

环境里同时存在大小写两种代理变量（如 `HTTPS_PROXY` 与 `https_proxy`），
MSBuild 构造子进程环境时把它们当成重复键而失败。构建前清掉即可：

```bash
unset https_proxy http_proxy all_proxy HTTPS_PROXY HTTP_PROXY ALL_PROXY
```

### 4.6 只构建了某个目标时，exe 目录缺少运行时依赖

`cmake --build build --config Debug --target <单个目标>` 只会产出该目标，
`bin/<Config>` 里可能缺少核心库/Qt 运行时。两种做法：

- 把产物手动复制到已部署好的 `bin/<Config>/` 再运行；
- 或构建 `INSTALL` 目标整体部署（但 `INSTALL` 默认会失败，见 4.7）。

### 4.7 构建 `INSTALL` 目标报 `Permission denied`

```text
-- Install configuration: "Debug"
CMake Error at src/piplugin/cmake_install.cmake:37 (file):
  file INSTALL cannot set permissions on
  "C:/Program Files/piplugin/lib/Debug/piplugind.lib": Permission denied.
```

**原因**：项目里从来没有设置过 `CMAKE_INSTALL_PREFIX`，CMake 在 Windows 上
回落到默认值 `C:/Program Files/<PROJECT_NAME>`，本仓库即
`C:/Program Files/piplugin`。往那里写文件（哪怕只是给已存在的文件
重设权限）都需要管理员权限，非提权终端必然失败。

**影响范围**：安装脚本是嵌套 `include()` 执行的，顺序为

```text
cmake_install.cmake
  ├─ src/cmake_install.cmake
  │    └─ src/piplugin/cmake_install.cmake   ← 在这里失败
  ├─ adapters/cmake_install.cmake                     ← 不会执行
  └─ tests/cmake_install.cmake                        ← 不会执行
```

`src/piplugin` 排在最前面且第一条 `file(INSTALL)` 就报错，所以后面的
子目录全被跳过。**编译产物是好的**（`lib/Debug/`、`build/**/Debug/` 都在），
但仓库里的 `bin/<Config>` **不会被刷新**——它那些 `install()` 用的是绝对目标
`GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH = <root>/bin/$<CONFIG>`，正好排在后面
被跳过的 `adapters/` 和 `tests/` 里。所以"报 INSTALL 失败"之后 `bin/<Config>`
里的东西是旧的，别误以为已经更新过。

**两条 `install(TARGETS)` 的关系**（`src/piplugin/CMakeLists.txt:141` 与 `:150`）：

| | 第 141 行 | 第 150 行 |
|---|---|---|
| 目标路径 | `${CMAKE_INSTALL_LIBDIR}/$<CONFIG>` 等**相对**路径 | `${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}` **绝对路径** |
| 实际落点 | `${CMAKE_INSTALL_PREFIX}` 下的 `lib/<Config>`、`bin/<Config>` | `<root>/bin/<Config>`（不受 prefix 影响） |
| 内容 | .lib / .dll / 头文件 / CMake config | 只有 .dll |
| 语义 | 真正意义的"安装"（给外部项目 find_package 用） | 把运行时 dll 放到 exe 同目录，本质是"本地部署" |

它们是**同一个 INSTALL 目标、同一份脚本里的两条独立规则**，不是两个 target；
按源码顺序执行，第二条不会因为你改了 `--prefix` 而改变落点。
但只要第一条失败，脚本就整体中断，第二条和后面的 `adapters/`、`tests/` 都跑不到。
（实测：把 prefix 指到可写目录后，两条都执行，`bin/Debug` 被正常刷新，退出码 0。）

**解决办法**（任选其一）：

1. 配置时显式指定仓库内的前缀：
   `cmake --preset conan-debug -DCMAKE_INSTALL_PREFIX=<root>/install`
2. 或安装时临时指定：`cmake --install build --config Debug --prefix <root>/install`
3. 或在 root `CMakeLists.txt` / preset 里固定 `CMAKE_INSTALL_PREFIX`，别再依赖默认值。
4. 只是想把本机跑起来：不装，直接把 `lib/<Config>/` 与 `build/**/Debug/` 的产物
   拷到 `bin/<Config>/` 即可。

### 4.8 构建配置必须与 Conan 变体一致

`conan install` 用的是哪个 build_type，构建时就只能用对应 config。
当前仓库的 `build/` 是 **Debug 变体**（preset `conan-debug`），
所以必须用：

```bash
cmake --build build --config Debug --target pi_test_host_imgui
```

用 `--config Release` 会因 Conan 生成的 `imgui` 包数据不匹配而报
`无法打开包括文件: "imgui.h"`。

## 6. 下一步

- 了解架构：阅读 `docs/design/architecture.md`
- 写第一个插件：阅读 `docs/tutorial/write-plugin.md`
- 使用 UI 适配器：阅读 `docs/tutorial/adapters.md`
- 排查 Windows 窗口缩放/嵌入的渲染问题：阅读 `docs/design/d3d-window-resizing.md`
- 规划路线：阅读 `docs/todo/`