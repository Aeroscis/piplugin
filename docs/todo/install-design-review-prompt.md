# 给外部模型讨论用的 prompt：install 目标与 `bin/<Config>` 部署

> **状态：已修复**（2026-09-22，提交 `72538ae` 与 `c699c63`）。本文件保留为设计讨论
> 的记录：第 1 节是最终结论，第 2 节起是当时发给外部模型的原始 prompt，可整段复制，
> 其中描述的"现状"已不再成立。

## 1. 状态

- **根因已确认**：`CMAKE_INSTALL_PREFIX` 从未设置 → 回落 Windows 默认
  `C:/Program Files/pipluginframework` → 非提权终端 `Permission denied`。
- **影响已确认**：安装脚本嵌套 include，`src/pipluginframework` 排第一且首条
  `file(INSTALL)` 即失败 → 整体中断 → `adapters/`、`tests/` 被跳过 →
  仓库 `bin/<Config>` 不会被刷新。
- **已修复**：`bin/<Config>` 改为完全由 `POST_BUILD` 维护（框架库 + 5 个测试
  target + Qt 运行时），安装前缀经 `conan-default-local` 预设固定到
  `<root>/build/install`。全仓库已无绝对安装目标，不再有被前面失败连带跳过的风险。
- 现象与排查过程见 `docs/tutorial/quickstart.md` 4.7。

---

## 2. 以下为 prompt（可直接整段复制）

````text
你是一位精通 CMake（3.23+）与 Windows 构建的工程师。请针对下面这个真实项目的
「安装 / 部署」设计问题给出评估与方案。

第 2、3 节是我本机实测确认过的事实，请不要重新猜测或否认这些事实；
但如果你认为我基于这些事实做出的某个**推论**有漏洞，请明确指出并说明理由。

### 1. 项目环境
- Windows 11 + Visual Studio 2022 多配置生成器，CMake 3.23+，Conan 2.x
- 仓库根：`D:/Flora/ProgramProjects/pipluginframework`，构建目录 `build/`
  （由 preset `conan-debug` 生成，只能构建 Debug 配置）
- 这是一个 C ABI 插件框架：核心库 `pipluginframework`（SHARED）
  + Qt / ImGui 适配器 + 若干测试宿主与测试插件（exe 与 dll）
- 开发期要求：能在 `<root>/bin/Debug/` 下直接双击/命令行跑测试宿主，
  该目录必须同时有核心库 dll、Qt 运行时 dll、`platforms/qwindows.dll`

### 2. 已实测确认的事实
F1. 全仓库没有任何一处设置过 `CMAKE_INSTALL_PREFIX`（已 grep 全仓库确认）。
    因此生成的 `build/**/cmake_install.cmake` 顶部都有：
    `if(NOT DEFINED CMAKE_INSTALL_PREFIX) set(CMAKE_INSTALL_PREFIX "C:/Program Files/pipluginframework")`
F2. 非提权终端执行 `cmake --build build --config Debug --target INSTALL` 必然失败：

    -- Install configuration: "Debug"
    -- Up-to-date: C:/Program Files/pipluginframework/lib/Debug/pipluginframeworkd.lib
    CMake Error at src/pipluginframework/cmake_install.cmake:37 (file):
      file INSTALL cannot set permissions on
      "C:/Program Files/pipluginframework/lib/Debug/pipluginframeworkd.lib": Permission denied.
    MSB3073 ... 已退出，代码为 1

F3. 安装脚本是嵌套 `include()` 执行，顺序为
    `cmake_install.cmake` → `src/` → `src/pipluginframework/` → `adapters/` → `tests/`。
    `file(INSTALL)` 报错在 `cmake -P` 脚本模式下是 fatal：第一条失败即整体中断，
    后面的 `adapters/` 与 `tests/` 一条都不执行。
F4. 项目里「把产物放进 `<root>/bin/<Config>`」的规则，全部使用**绝对安装目标**
    `GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH`（定义于 `cmake/pi/pi_project.cmake:73`，
    值为 `${GLOBAL_PROJECT_PATH}/bin/$<CONFIG>`）。绝对目标不受 `--prefix` 影响，
    但这些规则位于 `adapters/` 与 `tests/`，被 F3 的中断连带跳过。
F5. 一旦前缀可写，整条链立刻通。`cmake --install build --config Debug --prefix <可写目录>`
    实测 exit 0，日志依次出现：
      -- Installing: <prefix>/lib/Debug/pipluginframeworkd.lib
      -- Installing: <prefix>/include/pipluginframework/*.h
      -- Up-to-date: D:/.../bin/Debug/pipluginframeworkd.dll      ← 绝对目标，执行了
      -- Installing: D:/.../bin/Debug/pi_test_host_imgui.exe
      -- Installing: D:/.../bin/Debug/pi_test_plugin_imgui.dll
    即 `bin/Debug` 被正常刷新。
F6. `src/pipluginframework/CMakeLists.txt` 对一个 target 写了两条 `install(TARGETS)`：
      :141  相对目标（`${CMAKE_INSTALL_LIBDIR}/$<CONFIG>`、`${CMAKE_INSTALL_BINDIR}/$<CONFIG>`、
             `FILE_SET HEADERS`、`EXPORT`）—— 真·安装，供外部 `find_package`
      :150  绝对目标（`${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}`）—— 只装 dll，
             语义是「把运行时 dll 放到 exe 同目录」
    它们是同一 INSTALL 目标、同一份脚本里的两条独立规则，不是两个 target，按源码顺序执行。
F7. 另有两条 `add_custom_command(... POST_BUILD)` 也在往同一个 `bin/<Config>` 放东西
    （`tests/test_host_qt/CMakeLists.txt:69`、`tests/test_plugin/CMakeLists.txt:76`，拷 Qt 运行时）。
    即：同一个目录现在由「install 规则」和「POST_BUILD 规则」两套机制共同维护。

### 3. 关键代码位置
- `cmake/pi/pi_project.cmake:73`  定义 `GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH = <root>/bin/$<CONFIG>`
- `src/pipluginframework/CMakeLists.txt:141`  第一条 install（相对目标 + EXPORT）
- `src/pipluginframework/CMakeLists.txt:150`  第二条 install（绝对目标，只装 dll）
- `src/pipluginframework/CMakeLists.txt:155/170`  导出集与 CMake config 的 install
- `tests/test_host/CMakeLists.txt:71`、`tests/test_plugin/CMakeLists.txt:91`、
  `tests/test_host_qt/CMakeLists.txt:84/87/93`、`tests/test_plugin_imgui/CMakeLists.txt:65`、
  `tests/test_headless_host/CMakeLists.txt:48`  全部用同一个绝对目标

### 4. 我的诉求与约束
- 核心痛点：开发期「改一行代码 → 让 `bin/Debug` 拿到新产物」这件事现在不可靠。
  跑 INSTALL 会失败；只构建单个目标又不刷新 `bin/Debug`。
- 必须保留对外发布能力：外部项目 `find_package(pi)` 要能拿到 .lib / .dll / 头文件 / CMake config。
  不要为了本机方便把这个能力砍掉。
- 希望命令尽量简单，最好是**一条命令**；不接受每次手动 copy。
- 不接受「用管理员权限跑」「装进 C:/Program Files」这类方案。
- `DESTINATION` 里出现 `$<CONFIG>` 是现状（多配置生成器），请评估是否该保留。
- 也请评估：绝对安装目标触发的 `CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION`
  是否会演变成 `CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION` 的隐患。

### 5. 请回答这些问题
Q1. `:150` 这条绝对目标的 install，是否应该改成 `add_custom_command(TARGET ... POST_BUILD)`
    拷贝到 `bin/$<CONFIG>`？请对比它对「构建即就绪」「CI 只在 install 阶段部署」
    「dll 输出路径在多配置下的取法」三方面的影响，并给出具体写法。
Q2. 或者是否更该给它加 `COMPONENT`（如 `runtime_local`），用
    `cmake --install build --config Debug --component runtime_local` 单独触发？
    这样做能否绕开「前面失败导致后面不执行」？component 之间的失败是否会互相阻断？
Q3. 或者直接在 root CMakeLists / preset 里把 `CMAKE_INSTALL_PREFIX` 固定到仓库内
    （如 `<root>/install`）？在这种情况下 `:150` 那条绝对目标是否还有存在必要？
    会不会造成同一份 dll 被装到两个地方？
Q4. 同一个 `bin/<Config>` 目录现在被 install 规则和 POST_BUILD 规则双重维护，
    是否应该统一成一种机制？哪种更适合「开发期目录」这一定位？
Q5. 多配置生成器下，`DESTINATION` 里用 `$<CONFIG>` 是否符合惯例？
    与 GNUInstallDirs 的 `CMAKE_INSTALL_LIBDIR / BINDIR` 搭配有没有坑？
Q6. 综合以上，你认为**最小且正确**的改动是哪一步？请给出优先级排序。

### 6. 输出要求
- 给 2~3 个方案，每个方案包含：改动点（**具体的 CMake 代码片段**）、风险、
  对现有命令的影响、是否破坏 `find_package(pi)`。
- 明确指出「最小改动能解当前痛点」的那一个。
- 如果信息不足以判断，请列出需要我补充的具体项（文件路径、变量值、构建命令等），
  不要自己编造路径或变量名。
- 不要输出泛泛的 CMake 教程式内容。
````
