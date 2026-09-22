# 构建 / 打包待办（Build & Packaging）

## 1. 适配器套件纳入 Conan 包 [P1]

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

## 2. CI 流水线 [P1]

无 CI 配置。建议建立（GitHub Actions / Gitee Go 均可）：
- Windows：VS2022 + conan + Qt 5.15.2，跑完整构建与测试宿主自动验证；
- Linux：gcc/clang + conan（不含 UI，或最小 UI 依赖）；
- 每次 push 跑 `conan install` + `cmake --preset` + 构建；
- headless 测试宿主作为冒烟测试（退出码断言）。

## 3. CMake presets / 工具链现代化 [P2]

- `CMakeUserPresets.json` 由 conan 管理，勿手工改；但可在仓库内置
  `CMakePresets.json`（不含 conan 生成的 preset）提供通用 configure/build presets，
  方便无 conan 的 CI 阶段；
- 评估 `toolchain files` 统一（conan toolchain 已承担）。

## 4. Qt 依赖策略 [P1]

**现状**：Qt 路径硬编码在根 `CMakeLists.txt`
（`list(APPEND CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2019_64")`），
他人机器必须先改路径才能构建 Qt 相关目标。

**建议**：
1. 改为 **CMake 缓存变量 / 环境变量**（如 `PI_QT_PREFIX` 或
   `Qt5_DIR`），缺省时仅提示而不硬编码；
2. 或评估用 conan 的 `qt/5.15.x` 包替代本地 Qt（需要 conan-center 的 Qt 配方，
   注意许可与体积）；
3. CI 中通过变量注入 Qt 路径。

## 5. 安装/部署布局统一 [P2]

- 核心库产物在 `lib/<CONFIG>/`，宿主/插件在 `bin/<CONFIG>/`，install 阶段
  统一复制 Qt 运行时。建议文档化+测试 `cmake --install` 到干净前缀后
  宿主可直接运行（无 Qt 环境变量依赖）。
- 可增加 `cpack` 配置产出 zip/installer。

## 6. 库名/产物名一致性 [P2]

- Debug 库名带 `d` 后缀（`piplugind.dll`），Release 不带——
  这是既有约定；建议在文档与 CI 中固定，避免误用。

## 7. 许可证/元数据完善 [P2]

- `conanfile.py` / `CMakeLists.txt` 的 `url` 目前是占位
  （`https://github.com/example/piplugin`）；HOMEPAGE_URL 亦为占位。
  补充真实仓库地址与 LICENSE 文件传播（backends/LICENSE.txt 是 imgui 的）。
- 打包时随包带上 LICENSE。