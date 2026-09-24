# plugin_scan — 插件发现试水（FUT-07 的第一步）

宿主目前只能**硬编码路径**逐个加载插件。`FUT-07`（发现 / 分发）整个方向没有代码事实，
这个例子就是最小的那一步：**扫一个目录，把里面的插件清单打印出来**。

它证明的是一件很具体的事：**光靠 descriptor，宿主就能知道一个目录里有什么**——
不需要实例化任何插件，也不需要插件提供清单文件。它**不改核心 ABI**，也没有清单格式、
没有依赖求解、没有签名（那些是 FUT-07 的后续）。

## 三步跑通

```powershell
# 1) 在仓库根构建
cmake --preset conan-default
cmake --build --preset conan-debug --parallel

# 2) 进开发目录（官方测试插件与示例插件都在这里）
cd bin\Debug

# 3) 扫（不给参数就扫当前目录）
.\pi_plugin_example_plugin_scan.exe
.\pi_plugin_example_plugin_scan.exe ..\..\build\some_plugin_folder
```

实测输出（`bin\Debug`，节选）：

```
== piplugin plugin scan (FUT-07 first step) ==
directory: .

candidates: 18 file(s), 18 inspected

[13] pi_plugin_test_plugin_qt.dll
     name     : Qt Test Plugin
     vendor   : piplugin
     version  : 1.2.0
     category : UI/Test
     api      : 0x00000004
     caps     : provides=1 required=0 optional=1
     props    : 3
       com.example.kind = qt-plugin
       com.example.ui.toolkit = qt5
       com.example.variant = A

== inventory: usable plugins ==
name                               version   category          file
Example ImGui Plugin               1.0.0     Example/UI        pi_plugin_example_plugin_imgui.dll
Example Qt Plugin                  1.0.0     Example/UI        pi_plugin_example_plugin_qt.dll
Qt Test Plugin                     1.2.0     UI/Test           pi_plugin_test_plugin_qt.dll
Qt Test Plugin B                   1.2.0     UI/Test           pi_plugin_test_plugin_qt2.dll
...（共 11 个）

== rejected / not a plugin ==
piplugind.dll                      pi_plugin_module_load failed: ".\piplugind.dll" does not export pi_plugin_entry
piplugin_qtd.dll                   pi_plugin_module_load failed: ".\piplugin_qtd.dll" does not export pi_plugin_entry
pi_plugin_test_plugin_badversion.dll      plugin api_version 0x00020000 (major 2, minor 0) is incompatible with host 0x00000004 (major 0, minor 4) ...
pi_plugin_test_plugin_guirequired.dll     plugin requires capability iid data1=0x00000011 but this host does not provide it
Qt5Core.dll                        pi_plugin_module_load failed: ".\Qt5Core.dll" does not export pi_plugin_entry
...（共 7 个）

usable=11 rejected=7
RESULT: PASS
```

三件事值得注意：

1. **"不是插件"是正常结果，不是错误**。部署目录里大多数 DLL 是别的东西（框架 DLL、
   Qt 运行时、平台插件），扫描器把它们**连同原因**一起报出来，而不是中断；
2. **被门禁拒绝的插件也在清单里**：`badversion`（api_version 不兼容）与
   `guirequired`（本扫描器是无头宿主，插件 REQUIRE 了 `HOST_UI`）——发现阶段就能看到
   "这个目录里哪些插件在本宿主里不可用、为什么"；
3. **什么都没被实例化**：扫描用的是 `pi_plugin_host_session_inspect()`（加载模块 + 门禁后停住），
   读完 descriptor 立刻 `pi_plugin_host_session_unload()`。跑完进程里零个插件模块。

## 版本选择：这里是**占位**

同名（descriptor 的 `name`）多版本时选最高版本，用的是数字分段比较（`1.10 > 1.9`，
缺段按 0）。`bin\Debug` 里没有同名多版本插件，所以正常输出是：

```
== version selection (PLACEHOLDER: highest version per name) ==
no descriptor name appears twice - nothing to choose
```

想验证分组逻辑，可以把同一个插件复制两份换个文件名再扫：

```powershell
mkdir build\scan_demo
copy bin\Debug\pi_plugin_test_plugin_qt.dll build\scan_demo\dup_a.dll
copy bin\Debug\pi_plugin_test_plugin_qt.dll build\scan_demo\dup_b.dll
cd bin\Debug ; .\pi_plugin_example_plugin_scan.exe ..\..\build\scan_demo
# => 'Qt Test Plugin': 2 candidate(s) -> picks 1.2.0 (dup_a.dll)
```

**这只是占位**：真正的解析还要看 `api_version`、能力/依赖、平台、签名——那是 FUT-07
的正文，不是本卡的范围。

## 读代码的顺序

1. `main()`：一个**无头** session（`PI_INVALID_WINDOW`，没有窗口、没有消息循环，
   因为发现不需要跑插件）；
2. `ScanDirectory()`：`FindFirstFileA` 遍历 `*.dll`（Windows；其余平台需要一个分支）；
3. `ScanFile()`：`pi_plugin_host_session_inspect()` → 读 descriptor → **深拷贝** →
   `pi_plugin_host_session_unload()`；
4. `PrintInventory()` / `PrintVersionSelection()`：清单与占位选版。

第 3 步的深拷贝不是洁癖：**descriptor 的字符串属于模块**，`unload` 之后原指针就是
悬垂指针。这是每个写发现/清单类工具的人都会踩一次的坑，所以这里明确写在注释里。

## 下一步（FUT-07 还没做的）

- 清单格式（外部 manifest vs. 纯 descriptor）与缓存；
- 真正的版本/依赖/能力求解，以及"同名不同 ABI"的处理；
- 目录之外的分发形态（包 / 仓库 / 签名校验）。
