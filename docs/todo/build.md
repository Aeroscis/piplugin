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

## 2. CI 流水线 [P1] —— Windows 已完成（roadmap BLK-05）；Linux job 已上收（W-10）

> **状态**：Windows 部分已落地：`.github/workflows/ci.yml` 是薄壳（装依赖 +
> 构建），检查全部交给 `scripts/verify.ps1`（ctest + 一致性验收 + clang-format
> 漂移报告），跑在 GitHub 镜像仓库上（双仓分工见 README）。原建议里的
> **Linux job 没做**——已上收为下一波工作 **W-10**（`docs/todo/parallel-improvements.md`
> 派工板，CI/脚本线）：ubuntu + gcc/clang + conan、Qt/imgui 关闭，把 README
> 的「预期可编译」变成「CI 证明可编译」。

无 CI 配置。建议建立（GitHub Actions / Gitee Go 均可）：
- Windows：VS2022 + conan + Qt 5.15.2，跑完整构建与测试宿主自动验证；
- Linux：gcc/clang + conan（不含 UI，或最小 UI 依赖）；
- 每次 push 跑 `conan install` + `cmake --preset` + 构建；
- headless 测试宿主作为冒烟测试（退出码断言）。

## 3. CMake presets / 工具链现代化 [P2] —— 已上收（W-09）

> **状态**：仓库内置 `CMakePresets.json` 仍未做（`CMakeUserPresets.json` 仍由
> conan 生成、勿手工改）——已上收为下一波工作 **W-09**（`docs/todo/parallel-improvements.md`
> 派工板，构建打包线）：不依赖 conan 的通用 configure/build preset（Qt 目标
> 允许跳过），无 conan 环境可 configure。

- `CMakeUserPresets.json` 由 conan 管理，勿手工改；但可在仓库内置
  `CMakePresets.json`（不含 conan 生成的 preset）提供通用 configure/build presets，
  方便无 conan 的 CI 阶段；
- 评估 `toolchain files` 统一（conan toolchain 已承担）。

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

## 5. 安装/部署布局统一 [P2] —— install 验证已完成；cpack 已上收（W-08）

> **状态**：前半已达成——`scripts/verify_package.ps1`（ECO-04）对
> `cmake --install` 到干净前缀后的 install 树做「消费方可直接构建运行」验证
> （双形态：install 树 + conan 包）。**cpack 产出**（zip，可选 NSIS）未做——
> 已上收为下一波工作 **W-08**（派工板，构建打包线），顺带把「install 到
> 干净前缀后宿主可直接运行」并入该验证。

- 核心库产物在 `lib/<CONFIG>/`，宿主/插件在 `bin/<CONFIG>/`，install 阶段
  统一复制 Qt 运行时。建议文档化+测试 `cmake --install` 到干净前缀后
  宿主可直接运行（无 Qt 环境变量依赖）。
- 可增加 `cpack` 配置产出 zip/installer。

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

- `conanfile.py` / `CMakeLists.txt` 的 `url` 目前是占位
  （`https://github.com/example/piplugin`）；HOMEPAGE_URL 亦为占位。
  补充真实仓库地址与 LICENSE 文件传播（backends/LICENSE.txt 是 imgui 的）。
- 打包时随包带上 LICENSE。