# 构建 / 打包待办（Build & Packaging）

## 1. 适配器套件纳入 Conan 包 [P1] —— 已完成（roadmap ECO-04）

> **状态**：已落地并经**外部消费方**验证。`conanfile.py` 的 components：
> `piplugin`（核心）/ `piplugin_host`（L0）/ `piplugin_host_qt` /
> `piplugin_events` / `piplugin_host_dx11` / `piplugin_imgui` / `piplugin_qt`，
> 各自带 libdirs / includedirs /（Windows 下）system_libs。`find_package(piplugin)`
> 与旧 `find_package(pi)` 入口都可用（`src/cmake/piForwardConfig.cmake.in` 兼容）。
> `examples/conan_consumer` + `scripts/verify_package.ps1` 对 install 树与
> conan 包**两种形态**做构建 + 运行验证（收尾 `9e833c2` / `9be2f52` 修了查找
> 路径、组件声明、目录继承、system_libs 四类问题，CHANGELOG `[Unreleased]`
> 有全记录）。遗留：conan 包不随包带 LICENSE，见第 7 条。

**现状**：`conanfile.py` 只打包核心库（`cpp_info.libs = ["piplugin"]`，
只 copy `include/` 与构建产物）。`piplugin_qt` / `piplugin_imgui`
两个套件**不在** conan 包内。

**建议**：
1. 将 `adapters/*` 也纳入 `exports_sources` 与 `package()`；
2. `package_info()` 增加组件：`self.cpp_info.components["imgui"]`、
   `self.cpp_info.components["qt"]`，各自带上 imgui / Qt5 依赖；
3. 这样消费方 `conan install` 后可直接 `find_package(pi)` + 链接
   `pi::piplugin_imgui` 等。

> 注意：Qt 目前是本地安装（非 conan 依赖），套件打包时需处理 Qt 依赖传播
> （要么要求消费方自行 find Qt，要么用 conan `qt` 包替代——见第 4 条）。

## 2. CI 流水线 [P1] —— 已完成（roadmap BLK-05；W-03、W-10 补齐另两条跑道）

> **状态**：`.github/workflows/ci.yml` 仍是薄壳（装依赖 + 构建），检查交给脚本；
> 现在跑在 GitHub 镜像仓库上的是**三个 job**（双仓分工见 README）：
> 1. `verify`（Windows，BLK-05）：`scripts/verify.ps1` —— ctest + 一致性验收 +
>    FFI 示例 + clang-format 漂移报告 + **文档漂移检查**（W-13 新增第 5 项）；
> 2. `asan`（Windows，W-03）：`scripts/verify_asan.ps1` —— 同一棵树就地重配
>    `/fsanitize=address`，只跑非 GUI 子集（非 GUI 的理由与实测见 `tests.md` #3）；
> 3. `linux`（ubuntu，W-10）：**gcc 与 clang 各一轮**，不用 conan（Qt/imgui 关掉后
>    没有任何第三方依赖），构建核心 + 宿主 kit 并直接运行 `unit_cpp`
>    （`checks=52 failures=0`）。job 里逐条写明了它**不**覆盖什么：`unit` /
>    `unit_threads` / headless 宿主因 `nanosleep` 缺 glibc 平台层宏在 Linux 上编不过、
>    所有插件目标都在 `if(NOT WIN32)` 后面、`add_test` 的 COMMAND 硬编码 `.exe`
>    使 ctest 在 Linux 上无法启动用例 —— 三处都在 `tests/` 里（见 `platform.md` #3），
>    有意**不**用额外 `-D` 把它们糊过去。
>
> 结论口径：Linux 行现在是「**核心与宿主 kit 由 CI 证明可编译**」，
> 不是「Linux 全面可编译」。原建议里的 Qt 5.15.2 那条仍不成立（Qt 是本地安装，
> runner 上没有，CI 里 Qt 目标整批自动禁用）。

无 CI 配置。建议建立（GitHub Actions / Gitee Go 均可）：
- Windows：VS2022 + conan + Qt 5.15.2，跑完整构建与测试宿主自动验证；
- Linux：gcc/clang + conan（不含 UI，或最小 UI 依赖）；
- 每次 push 跑 `conan install` + `cmake --preset` + 构建；
- headless 测试宿主作为冒烟测试（退出码断言）。

## 3. CMake presets / 工具链现代化 [P2] —— 已完成（W-09）

> **状态**：已落地。仓库内置 `CMakePresets.json`（**入库**）：`default`（VS 2022 / x64，
> 构建目录 `build/generic`，与 conan 的 `build/` 互不干扰）与 `default-unix`（Ninja），
> 各带同名 build / test 预设；文件里**不含**任何 conan 生成的预设，缺 Qt / imgui 时相关
> 目标沿用 ECO-05 的"打印指引后自动禁用"，不打断 configure。
>
> **一处超出派工板预期的发现**（"不动 `CMakeUserPresets.json`"这一条做不到）：只加内置
> 预设文件并不能让"无 conan 也能 configure"成立。`CMakeUserPresets.json` 是 conan 生成的
> 本地文件却**入了库**，它 `include` 生成器目录里的 `build/generators/CMakePresets.json`，
> 而干净检出里没有那个文件 —— CMake 对读不到的 include 是**硬失败**
> （`CMake Error: Could not read presets ... File not found`），在它检查你要用的是哪个
> 预设**之前**就整体报错（实测：把该 include 的目标藏起来，`cmake --list-presets` 直接
> exit 1）。所以该文件改为**不入库**（`.gitignore`，与 CMake 官方建议一致：它是每台机器
> 的本地文件，conan 每次 `conan install` 都会重写）。
>
> 原先只写在那个本地文件里的 `conan-default-local`（把安装前缀钉到 `<root>/build/install`，
> 见第 5 条与 `docs/todo/install-design-review-prompt.md`）随之失去载体：CMake 的可见性
> 规则不允许仓库预设继承 conan 预设（`Inherited preset ... is unreachable from preset's
> file`，实测），故这一默认值改由根 `CMakeLists.txt` 在
> `CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT` 时设定 —— conan / 纯 CMake / cpack 三条
> 流程一致，用户显式给的前缀照旧优先。
>
> **验证**：把 `CMakePresets.json` 之外的预设文件与 conan 生成的 include 都藏起来（模拟
> 干净检出）后，`cmake --preset default` + `cmake --build --preset default` 全绿；本机 Qt
> 在、imgui 不在，正好覆盖"缺依赖自动禁用"那一支（imgui 套件 / imgui 示例插件 / imgui
> 测试宿主与插件四条提示后跳过）。文档同步：README「方式二」、
> `docs/design/build-system.md` §4。派工板任务 W-09。

**原始需求**（保留）：现在 configure 必须 conan（`CMakeUserPresets.json` 由 conan 生成）；
无 conan 环境的 CI 阶段或快速试用没有一条顺手的入口。

- `CMakeUserPresets.json` 由 conan 管理，勿手工改；但可在仓库内置
  `CMakePresets.json`（不含 conan 生成的 preset）提供通用 configure/build presets，
  方便无 conan 的 CI 阶段；
- 评估 `toolchain files` 统一（conan toolchain 已承担）—— 仍未做：当前是"conan 用
  conan_toolchain.cmake、纯 CMake 流程不用 toolchain file"两条并列路径，未统一。

## 4. Qt 依赖策略 [P1] —— 已完成（roadmap ECO-05）

> **状态**：已去硬编码（根 `CMakeLists.txt` 46-56）：查找顺序
> `PI_QT_PREFIX` → `Qt5_DIR` / `CMAKE_PREFIX_PATH`（含环境与 PATH）；
> 维护者常用路径只在「本机存在」时作为提示；缺 Qt 时四个 Qt 目标带指引
> 自动禁用，不破坏 configure。CI / 他人机器经变量注入。conan `qt` 包
> 替代方案未做（维持「本地安装 Qt」策略）。

**现状**：Qt 路径硬编码在根 `CMakeLists.txt`
（`list(APPEND CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2019_64")`），
他人机器必须先改路径才能构建 Qt 相关目标。

**建议**：
1. 改为 **CMake 缓存变量 / 环境变量**（如 `PI_QT_PREFIX` 或
   `Qt5_DIR`），缺省时仅提示而不硬编码；
2. 或评估用 conan 的 `qt/5.15.x` 包替代本地 Qt（需要 conan-center 的 Qt 配方，
   注意许可与体积）；
3. CI 中通过变量注入 Qt 路径。

## 5. 安装/部署布局统一 [P2] —— 已完成（W-08）

> **状态**：已落地。cpack 产出 zip（`CPACK_GENERATOR=ZIP`，`-DPI_CPACK_NSIS=ON` 可选追加
> NSIS 安装包），归档根就是 `bin/<CONFIG>/`、`lib/<CONFIG>/`、`include/`、cmake 包配置，
> 外加随包分发的根文档 `LICENSE` / `README.md` / `CHANGELOG.md`（此前没有任何 install
> 规则带上它们）。`CPACK_PACKAGING_INSTALL_PREFIX` 置空是必需的：Windows 上 CPack 默认
> 继承 `CMAKE_INSTALL_PREFIX`，否则整棵树会嵌在 `/Program Files/piplugin/` 下面。
> 归档名带构建配置（`piplugin-0.4.0-Debug-win64.zip`），但**不是**生成器表达式算出来的
> ——`CPACK_PACKAGE_FILE_NAME` 不支持生成器表达式，字面量里的尖括号在 Windows 上是非法
> 路径字符，cpack 会以 `Problem creating temporary directory` 直接失败（实测）；配置后缀
> 改由 `cmake/CPackProjectConfig.cmake` 在打包时拼上（那里 `CPACK_BUILD_CONFIG` 已就绪）。
>
> **验证**（`scripts/verify_package.ps1`，三形态一次跑完，`RESULT: PASS`）：
> - A 段（原有）install 树：消费方 configure / build / run 全通过；
> - B 段（原有）conan 包：`conan create` + 消费方全通过；
> - C 段（本次新增）：cpack 产出 zip → 解压到干净目录 → 断言归档布局（核心 DLL /
>   导入库 / 公共头 / `pipluginConfig.cmake` / `LICENSE` 齐全）→ **不带 conan 工具链**、
>   **PATH 上没有任何本仓库路径**的情况下配置并构建出宿主程序并运行成功
>   （`RESULT: PASS`）。也就是说"解压即可用"是脚本断言出来的，不是手工看的。
>
> **C 段第一次真跑就抓到一个回归**：新加的"根文档进 install 树"这条规则让 `conan create`
> 在 `package()` 阶段失败（`file INSTALL cannot find .../LICENSE: File exists.`）——
> `conanfile.py` 的 `exports_sources` 没有导出这三份文档，conan 构建目录里自然找不到。
> 已把 `LICENSE` / `README.md` / `CHANGELOG.md` 加进导出集，于是第 7 条的遗留项
> 「conan 包不随包带 LICENSE」**顺带解决**（见下）。
>
> **仍然遗留（如实记录，未做）**：
> 1. **install 阶段并没有"统一复制 Qt 运行时"** —— 下面原始需求里那句话与实际不符：Qt
>    运行时的部署只发生在**构建树**（测试宿主/插件的 `POST_BUILD` 把 `Qt5Core/Gui/Widgets.dll`
>    与 `platforms/qwindows.dll` 复制到 `bin/<CONFIG>`），install / cpack 归档里一份都没有
>    （归档 65 个条目里 `Qt5*` 为 0）。因此 C 段的"宿主直接运行"覆盖的是**不依赖 Qt** 的
>    链接面；要让 Qt 宿主也做到"解压即跑"，得先把 Qt 运行时纳入分发 —— 那是 LGPL 再分发
>    的决策（"Qt 是本地安装、消费方自行保证可达"是当前明示约定），不在本卡范围内。
> 2. 归档里没有宿主可执行文件：产品本身没有宿主 EXE，`tests/` 的宿主与 `examples/` 的
>    示例按既有约定都不进分发（见 `conanfile.py` 的 `exports_sources` 注释与 conanfile
>    里"examples 不进包"的说明）。C 段因此用"由归档构建出的宿主程序"来做运行断言。
> 3. NSIS 形态只做了配置，未在装了 NSIS 的机器上产出过安装包（默认关）。
>
> 派工板任务 W-08。

**原始需求**（保留）：核心库产物在 `lib/<CONFIG>/`，宿主/插件在 `bin/<CONFIG>/`，
install 阶段统一复制 Qt 运行时。建议文档化+测试 `cmake --install` 到干净前缀后
宿主可直接运行（无 Qt 环境变量依赖）。可增加 `cpack` 配置产出 zip/installer。

## 6. 库名/产物名一致性 [P2]

- Debug 库名带 `d` 后缀（`piplugind.dll`），Release 不带——
  这是既有约定；建议在文档与 CI 中固定，避免误用。

## 7. 许可证/元数据完善 [P2] —— 已完成（roadmap BLK-01/07）

> **状态**：已落地：根目录 `LICENSE`（MIT © 2026 Aeroscis）；`CREDITS.md`
> 逐条记录第三方代码与依赖；conan（`license` / `url` / `author`）与 CMake
> （`HOMEPAGE_URL`）元数据一致，URL 均为真实 Gitee 地址。README 徽章指
> GitHub 是**有意分工**：Gitee 主仓库、GitHub 镜像跑 CI（README 已写明）。
> 遗留：**conan 包不随包带 LICENSE**——`package()` 只走 `cmake.install()`，
> CMake install 规则里也没有 LICENSE 文件；如需随包分发，在 `package()`
> 补一行 copy 即可（剩余项，如实记录）。
> **该遗留已由 W-08 清掉**：install 规则现在会装 `LICENSE` / `README.md` /
> `CHANGELOG.md`，`conanfile.py` 的 `exports_sources` 也把这三份纳入了导出集，
> 因此 conan 包与 cpack 归档都带 LICENSE（`verify_package.ps1` 的 B/C 两段都为
> 此跑了真实验证）。

- `conanfile.py` / `CMakeLists.txt` 的 `url` 目前是占位
  （`https://github.com/example/piplugin`）；HOMEPAGE_URL 亦为占位。
  补充真实仓库地址与 LICENSE 文件传播（backends/LICENSE.txt 是 imgui 的）。
- 打包时随包带上 LICENSE。