# examples — 可构建、可运行的最小示范

roadmap **ECO-03**：把 `docs/tutorial/` 里的代码段变成能跑的工程。每个例子一个目录、
一个 `CMakeLists.txt`、一份 README（三步跑通），**只依赖公开 API**（核心库 + 可选套件/宿主 kit），
不引用 `tests/` 里的任何东西 —— 读者可以把单个目录拷出去当起点。

| 例子 | 是什么 | 需要什么 | 谁驱动它 |
|---|---|---|---|
| `minimal_host/` | 一个窗口 + 一个容器 + 一个插件的宿主（view / service 都能驱） | 宿主 kit L0 | — |
| `service_plugin/` | headless 服务插件（`IPiPluginService`：start/poll/status/stop） | 无（纯 C） | `minimal_host` |
| `minimal_plugin_imgui/` | 最小 imgui 插件（一个 draw 回调） | conan imgui + imgui 套件 | `minimal_host` |
| `minimal_plugin_qt/` | 最小 Qt 插件（一个 widget 工厂） | 本地 Qt5 + Qt 套件（SHARED） | `minimal_host` |
| `qt_host_direct/` | **Qt 宿主直连 Qt 插件**（宿主自己持有 `QApplication`，收编插件的 `QWidget*`，不走套件） | 本地 Qt5 + 宿主 kit L0 | 自己（一个 exe + 一个插件，带 `--self-test`） |
| `minimal_kit_win32/` | **一个最小适配器套件**（纯 C + GDI，零工具包）+ 用它的插件 | 无（Windows） | `minimal_host` / 官方一致性宿主 |
| `ffi/` | 用 **Python / Rust / C#** 各写一遍宿主（含本语言实现的宿主对象） | 对应语言的工具链 | 自己（`scripts/verify_ffi.ps1`） |
| `specialized_app/` | app 自定义协议（通道 A）+ 宿主自定义服务（通道 B）+ 能力门禁 | 宿主 kit L0 | 自己（一个 exe + 一个插件） |
| `conan_consumer/` | **站在"外部消费者"位置的测试件**：`find_package(pi COMPONENTS ...)`（安装树/归档）或 `find_package(piplugin)`（conan 包）→ 链接 → 运行 | 已安装/已打包的本库 | `scripts/verify_package.ps1` 的 A/B/C 三段 |
| `plugin_scan/` | **插件发现试水**（FUT-07 第一步）：扫目录 + 读 descriptor + 立刻卸载，打印清单 | 宿主 kit L0 | 自己（扫 `bin/<CONFIG>` 之类的目录） |

## 一起构建

```powershell
cmake --preset conan-default
cmake --build --preset conan-debug --parallel
```

产物都部署到 `bin/<CONFIG>/`（与框架 DLL、套件 DLL、Qt 运行时 DLL 同目录），所以直接进去跑即可：

```powershell
cd bin\Debug
.\pi_plugin_example_minimal_host.exe pi_plugin_example_plugin_imgui.dll
.\pi_plugin_example_minimal_host.exe pi_plugin_example_service.dll
.\pi_plugin_example_minimal_host.exe pi_plugin_example_plugin_qt.dll
.\pi_plugin_example_minimal_host.exe pi_plugin_example_plugin_win32.dll
.\pi_plugin_example_specialized_app.exe pi_plugin_example_specialized_plugin.dll pi_plugin_example_service.dll
.\pi_plugin_example_qt_direct_host.exe --self-test        # Qt 宿主 × Qt 插件（直连，退出码判定）
.\pi_plugin_example_plugin_scan.exe                       # 发现：把本目录的插件清单打出来
```

`minimal_kit_win32/` 还额外跑官方一致性验收（`scripts/verify.ps1` 会自动带上它）：

```powershell
pwsh -NoProfile -File scripts\run_selftest.ps1 -Plugin pi_plugin_example_plugin_win32.dll -Cycles 3
```

不需要例子时：`-DPI_BUILD_EXAMPLES=OFF`（或 conan 侧 `-o PI_PLUGIN_BUILD_EXAMPLES=False`）。

## 与 tests/ 的分工

- `examples/` = **怎么用**：代码尽量少、注释尽量多、跑起来能看见输出；
- `tests/` = **对不对**：断言、退出码、负向用例、CI 门禁。

其中两个例子同时注册成了 ctest 用例（`example_minimal_host_service`、
`example_specialized_app`）—— 它们没有 GUI 工具包依赖，值得每天被自动跑一遍；
GUI 例子留给 README 里的人工三步。
