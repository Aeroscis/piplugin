# 构建 / 打包待办（Build & Packaging）

> 本文件记**还没做**的事（标题不带完成标记的条目），以及已完成项的**结论 + 复核入口**。
> 已完成条目不再保留当时的方案原文——那是历史，在 `git log` 与 `CHANGELOG.md` 里；
> 构建/打包的机制与取舍在 `docs/design/build-system.md`。

## 1. 适配器套件纳入 Conan 包 [P1] —— 已完成（roadmap ECO-04）

> **结论**：`conanfile.py` 的 components 为 `piplugin`（核心）/ `piplugin_host`（L0）/
> `piplugin_host_qt` / `piplugin_events` / `piplugin_host_dx11` / `piplugin_imgui` /
> `piplugin_qt`，各自带 libdirs / includedirs /（Windows 下）system_libs；
> `find_package(piplugin)` 与旧 `find_package(pi)` 入口都可用。install 树与 conan 包
> **两种形态**都有构建 + 运行的自动验证。
> **复核**：`scripts/verify_package.ps1`（A/B 两段）；
> `docs/design/build-system.md`；消费方示例 `examples/conan_consumer/`。

## 2. CI 流水线 [P1] —— 已完成（roadmap BLK-05；W-03、W-10 补齐另两条跑道）

> **结论**：`.github/workflows/ci.yml` 是薄壳（装依赖 + 构建），检查全交给脚本；跑在
> GitHub 镜像仓库上的是**三个 job**（双仓分工见 `README.md`）：
> 1. `verify`（Windows）：`scripts/verify.ps1` —— ctest + 一致性验收 + FFI 示例 +
>    clang-format 漂移报告 + 文档漂移检查；
> 2. `asan`（Windows）：`scripts/verify_asan.ps1`（非 GUI 子集；理由与读日志须知写在
>    脚本注释里）；
> 3. `linux`（ubuntu）：gcc 与 clang 各一轮，不用 conan（Qt/imgui 关掉后没有第三方
>    依赖），构建核心 + 宿主 kit 并直接运行 `unit_cpp`——它证明的是"核心与宿主 kit
>    在 Linux 上可编译"，不覆盖什么逐条列在 `platform.md` #3。
> **长期策略（未发生，仅预案）**：Gitee 主仓库 + GitHub 镜像跑 Actions 是**有意分工**；
> 若镜像失维护，则把徽章改成文字链接并收缩为单仓。
> **复核**：`.github/workflows/ci.yml`；`docs/tutorial/quickstart.md` §4.6。

## 3. CMake presets / 工具链现代化 [P2] —— 已完成（W-09）

> **结论**：仓库内置 `CMakePresets.json`（`default`：VS 2022 / x64，构建目录
> `build/generic`；`default-unix`：Ninja），各带同名 build / test 预设，文件里**不含**
> 任何 conan 生成的预设；缺 Qt / imgui 时相关目标打印指引后自动禁用，不打断 configure。
> `CMakeUserPresets.json`（conan 生成）**不入库**——它是每台机器的本地文件，且干净检出里
> 它 `include` 的目标不存在时 CMake 会**硬失败**（在检查你用哪个预设之前）。
> 安装前缀的默认值也随之下沉到根 `CMakeLists.txt`（`CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT`
> 时设为 `<build>/install`），conan / 纯 CMake / cpack 三条流程一致，用户显式给的前缀优先。
> **复核**：`docs/design/build-system.md` §4；`README.md` 的 preset 用法。

## 4. Qt 依赖策略 [P1] —— 已完成（roadmap ECO-05）

> **结论**：已去硬编码。查找顺序 `PI_PLUGIN_QT_PREFIX` → `Qt5_DIR` / `CMAKE_PREFIX_PATH`
> （含环境与 PATH）；维护者常用路径只在「本机存在」时作为提示；缺 Qt 时四个 Qt 目标带
> 指引自动禁用，不破坏 configure。conan `qt` 包替代方案**未采纳**（维持「本地安装
> Qt」策略）。
> **复核**：`docs/design/build-system.md`；`docs/tutorial/quickstart.md` 5.4。

## 5. 安装/部署布局统一 [P2] —— 已完成（W-08）

> **结论**：cpack 产出 zip（`CPACK_GENERATOR=ZIP`，`-DPI_CPACK_NSIS=ON` 可选追加 NSIS），
> 归档根就是 `bin/<CONFIG>/`、`lib/<CONFIG>/`、`include/`、cmake 包配置，外加随包分发的
> `LICENSE` / `README.md` / `CHANGELOG.md`；归档名带构建配置。机制细节
> （`CPACK_PACKAGING_INSTALL_PREFIX` 为什么必须置空、`CPACK_PACKAGE_FILE_NAME` 为什么
> 不能用生成器表达式）写在 `docs/design/build-system.md`。
> **复核**：`scripts/verify_package.ps1` 三段一次跑完 —— A 段 install 树、B 段 conan 包、
> C 段 cpack zip 解压后在**不带 conan 工具链、PATH 上没有本仓库路径**的环境里配置并构建
> 出宿主程序运行成功（"解压即可用"是断言出来的）。
> **遗留**：见第 8 条。

## 6. 库名/产物名一致性 [P2]（开放）

- **约定本身已成文**：Debug 输出名带 `d` 后缀（`GLOBAL_PROJECT_BUILD_TYPE_SUFFIX`，
  `docs/design/build-system.md:118`）与产物布局（`docs/design/architecture.md` §8.3）；
  `OriginalFilename` 走生成器表达式取目标真实产物名（W-07 顺带），所以 Debug 的版本资源
  写的是 `piplugind.dll` 而不是 `piplugin.dll`；版本号取自 `project(VERSION)`，不可能与
  `conanfile.py` / 头文件漂移。
- **剩下开放的是**：在 CI 里断言一次（例如核对 `bin/<CONFIG>` 里的核心库名按配置带 / 不带
  `d`），免得消费方误用；也可以把这句约定补进 `docs/design/build-system.md` 的产物一节。

## 7. 许可证/元数据完善 [P2] —— 已完成（roadmap BLK-01/07）

> **结论**：根目录 `LICENSE`（MIT © 2026 Aeroscis）；`CREDITS.md` 逐条记录第三方代码与
> 依赖；conan（`license` / `url` / `author`）与 CMake（`HOMEPAGE_URL`）元数据一致，URL
> 均为真实 Gitee 地址；install 规则与 conan `exports_sources` 都带上 `LICENSE` /
> `README.md` / `CHANGELOG.md`，因此 conan 包与 cpack 归档都带 LICENSE。
> **复核**：`scripts/verify_package.ps1` 的 B/C 两段都会断言根文档在归档里。

## 8. 安装 / 分发的三个遗留 [P3]（开放）

1. **Qt 运行时不在 install / cpack 归档里**（归档 65 个条目里 `Qt5*` 为 0）：部署只发生在
   构建树（`POST_BUILD` 把 `Qt5Core/Gui/Widgets.dll` 与 `platforms/qwindows.dll` 复制到
   `bin/<CONFIG>`）。要让 Qt 宿主也"解压即跑"，得先把 Qt 运行时纳入分发——那是 **LGPL
   再分发**的决策（当前明示约定是"Qt 本地安装、消费方自行保证可达"），需要维护者拍板。
2. **NSIS 形态只做了配置，没在装了 NSIS 的机器上产出过安装包**（默认关闭）。
3. **归档里没有宿主可执行文件**——这是**有意**的：产品本身没有宿主 EXE，`tests/` 的宿主
   与 `examples/` 的示例按约定都不进分发（见 `conanfile.py` 的 `exports_sources` 注释）。
   C 段的运行断言因此用"由归档构建出的宿主程序"来完成。

## 9. toolchain file 未统一 [P3]（开放）

目前是两条并列路径：conan 流程用 `conan_toolchain.cmake`，纯 CMake 流程不用 toolchain
file。要不要统一（例如提供一个仓库内的最小 toolchain file 供无 conan 流程使用），
未评估也未做。
