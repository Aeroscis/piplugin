# conan_consumer — 站在"外部消费者"位置的测试件

roadmap **ECO-04**。它**不** `add_subdirectory` 本仓库的任何东西，只做每个消费者都会做的事：
`find_package(piplugin)` → 链接官方 target → 运行。单元测试与一致性验收都回答不了
"别人拿到这个包能不能用"，这个目录就是为那个问题存在的。

由 [`scripts/verify_package.ps1`](../../scripts/verify_package.ps1) 驱动，两种分发形态各跑一遍：

| 形态 | 怎么产生 | 消费方拿到什么 |
|---|---|---|
| **A. 安装树** | `cmake --install build --prefix <前缀>` | 仓库自己导出的 target 文件（`pi::piplugin_imgui` 自带 `imgui::imgui`） |
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
   imgui adapter kit linked: pi_imgui_view_create = 00007FF6BB971B3B
   RESULT: PASS
B. Conan package
   == piplugin packaged consumer ==
   PI_PLUGIN_API_VERSION = 0.4 (0x00000004)
   core + host kit L0 + event router: OK
   imgui adapter kit: not linked in this configuration
   RESULT: PASS
```

## 消费方要写的东西

```cmake
find_package(piplugin REQUIRED)          # 包名就是 piplugin（旧入口 find_package(pi) 仍可用）
target_link_libraries(app PRIVATE
    pi::piplugin            # 核心（SHARED）
    pi::piplugin_host       # 宿主 kit L0 会话
    pi::piplugin_events     # 宿主侧事件路由
    pi::piplugin_imgui      # imgui 适配器套件（静态；把 conan imgui 带过来）
)
```

头文件写法与仓库内一致（不需要写长路径）：`#include "pi_host_session.h"`、
`#include "pi_imgui_view.h"` —— 各 kit 的安装接口目录（`include/piplugin/host_kits/core` 等）
已经在包信息里，组件清单见下。

## 已知问题：conan 形态下 imgui 套件链接不上（待修）

**现象**：B 形态里如果直接 `target_link_libraries(app PRIVATE pi::piplugin_imgui)`，
链接阶段报 29 个 `unresolved external symbol`（全是 imgui 核心符号，如
`ImGuiIO::AddKeyCharacter`），因为 `imgui.lib` 不在链接行上。

**证据**（Conan 2.10.1 + CMakeDeps，`piplugin-debug-x86_64-data.cmake`）：

```cmake
set(piplugin_FIND_DEPENDENCY_NAMES )                                  # 空
set(piplugin_pi_piplugin_imgui_DEPENDENCIES_DEBUG pi::piplugin)       # 只有核心，没有 imgui::imgui
```

即 `cpp_info.components["piplugin_imgui"].requires = [..., "imgui::imgui"]` 里的外部引用
没有传播到消费方。**A 形态没有这个问题**（导出 target 自带 `imgui::imgui`），
所以这是 conan 打包侧待修项，而不是消费方写错。

**在修好之前的绕法**：消费方自己把 imgui 拉进来（conan 生成的 `imgui-config.cmake`
已在 `CMAKE_PREFIX_PATH` 上）：

```cmake
if(TARGET pi::piplugin_imgui AND NOT TARGET imgui::imgui)
    find_package(imgui CONFIG QUIET)
endif()
target_link_libraries(app PRIVATE pi::piplugin_imgui imgui::imgui)
```

这就是 `verify_package.ps1` 在 B 形态下传 `-DPI_CONSUMER_LINK_IMGUI=OFF` 的原因：
它验证的是**确实能工作**的部分（核心 + 两个宿主 kit + 事件路由 + 运行），
把待修项显式记录在这里而不是假装通过。

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

Qt 相关组件（`pi_host_qt` / `piplugin_qt`）只在本地装了 Qt5、且 CMake 侧没被禁用时才进包；
它们是**本地安装依赖**，包本身从不要求 Qt5（详见 `src/cmake/piConfig.cmake.in` 的策略注释）。
