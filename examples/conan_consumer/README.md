# conan_consumer — 站在"外部消费者"位置的测试件

roadmap **ECO-04**。它**不** `add_subdirectory` 本仓库的任何东西，只做每个消费者都会做的事：
`find_package(piplugin)` → 链接官方 target → 运行。单元测试与一致性验收都回答不了
"别人拿到这个包能不能用"，这个目录就是为那个问题存在的。

由 [`scripts/verify_package.ps1`](../../scripts/verify_package.ps1) 驱动，两种分发形态各跑一遍：

| 形态 | 怎么产生 | 消费方拿到什么 |
|---|---|---|
| **A. 安装树** | `cmake --install build --prefix <前缀>` | 仓库自己导出的 target 文件（`pi::plugin_imgui` 自带 `imgui::imgui`） |
| **B. conan 包** | `conan create .` + `conan install`（CMakeDeps） | CMakeDeps 依据 `conanfile.py` 的 `package_info()` 生成的 target |

## 跑

```powershell
pwsh -NoProfile -File scripts\verify_package.ps1            # 两种形态都跑
pwsh -NoProfile -File scripts\verify_package.ps1 -SkipConan  # 只跑 A（快）
```

实测输出（两种形态都 PASS）：

```
A. install tree
   == piplugin packaged consumer ==
   PI_PLUGIN_API_VERSION = 0.4 (0x00000004)
   core + host kit L0 + event router: OK
   imgui adapter kit linked: pi_plugin_imgui_view_create = 00007FF6BB971B3B
   RESULT: PASS
B. Conan package
   == piplugin packaged consumer ==
   PI_PLUGIN_API_VERSION = 0.4 (0x00000004)
   core + host kit L0 + event router: OK
   imgui adapter kit linked: pi_plugin_imgui_view_create = 00007FF60E941B3B
   RESULT: PASS
```

## 消费方要写的东西

```cmake
find_package(piplugin REQUIRED)          # 包名就是 piplugin（旧入口 find_package(pi) 仍可用）
target_link_libraries(app PRIVATE
    pi::plugin            # 核心（SHARED）
    pi::plugin_host       # 宿主 kit L0 会话
    pi::plugin_events     # 宿主侧事件路由
    pi::plugin_imgui      # imgui 适配器套件（静态；imgui 依赖见下一节）
)
```

头文件写法与仓库内一致（不需要写长路径）：`#include "pi_host_session.h"`、
`#include "pi_imgui_view.h"` —— 各 kit 的安装接口目录（`include/piplugin/host_kits/core` 等）
已经在包信息里，组件清单见下。

## Conan 形态下的依赖传播（实测结论，含一处 Conan 2.10 的限制）

**用 conan 包时，消费方必须在自己 conanfile 里把 `imgui` 也声明为依赖**：

```
[requires]
piplugin/0.4.0
imgui/1.92.8        # <- 必须；版本与本仓库 conanfile.py 保持一致
```

这不是"图省事"，而是 Conan 2.10.1 + CMakeDeps 的行为：**组件级的外部 require，只有在
消费方自己也依赖那个包时才会被传播**。否则它被**静默丢弃** —— 不报错、不警告：

| | 消费方只声明 piplugin | 消费方同时声明 imgui |
|---|---|---|
| 生成的配置 | 只有 `piplugin-config.cmake` | `piplugin-config.cmake` + `imgui-config.cmake` |
| `piplugin_FIND_DEPENDENCY_NAMES` | 空 | `imgui` |
| `piplugin_pi_plugin_imgui_DEPENDENCIES_DEBUG` | `pi::plugin` | `pi::plugin imgui::imgui` |
| 链接 `pi::plugin_imgui` | 29 个 imgui 未解析符号 | 通过 |

代码位置在 Conan 自己身上：`conan/tools/cmake/cmakedeps/templates/target_configuration.py`
的 `get_deps_targets_names()` 把"声明的组件 requires"拿去和**消费方的**依赖集合求交，
取不到就 `except KeyError: pass` —— 于是依赖静静地消失，而不是报"找不到 imgui"。
（对照：`cpp_info.components["piplugin_imgui"].requires` 里确实写着 `imgui::imgui`，
用探针打印可见 —— 声明是对的，丢的是传播。）

**裸 CMake 安装树没有这个限制**：导出 target 里就带着 `imgui::imgui`，消费方 link
`pi::plugin_imgui` 即可。所以 `verify_package.ps1` 的 A 形态不需要写 imgui，
B 形态的 `conanfile.txt` 里有（脚本从 `conanfile.py` 解析版本，不会漂移）。

**另一个坑是我们自己的，已修**：imgui / dx11 这些静态 kit 以 `PRIVATE`/`PUBLIC` 链接
Windows 平台库（`user32 d3d11 dxgi d3dcompiler`），静态库的消费方在链接期仍然需要它们。
CMake 导出 target 会自动带上，CMakeDeps 只能靠 `cpp_info.system_libs` —— 缺了就在消费方报
`unresolved external symbol D3D11CreateDeviceAndSwapChain`。现在两个组件都声明了
（`conanfile.py` 的 `package_info()`，按 `settings.os` 限定 Windows）。

## 包布局（随 install 规则）

```
include/piplugin/…                     核心公共头（#include "piplugin/pi_plugin.h"）
include/piplugin/host_kits/core/…      kit 头（平铺，便于 "pi_host_session.h"）
include/piplugin/adapters/imgui/…      套件头（平铺，便于 "pi_imgui_view.h"）
lib/<Config>/piplugin<d>.lib           核心导入库（SHARED，DLL 在 bin）
lib/<Config>/piplugin_{host,events,host_dx11,host_qt,imgui,qt}<d>.lib
lib/cmake/piplugin/pipluginConfig.cmake 伞配置：核心 + 按需导入各 kit
bin/<Config>/piplugind.dll             运行期核心；套件 DLL（Qt）也在 bin
```

Qt 相关组件（`piplugin_host_qt` / `piplugin_qt`）只在本地装了 Qt5、且 CMake 侧没被禁用时才进包；
它们是**本地安装依赖**，包本身从不要求 Qt5（详见 `src/cmake/piConfig.cmake.in` 的策略注释）。
