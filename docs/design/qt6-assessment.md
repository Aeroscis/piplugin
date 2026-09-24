# Qt6 评估：Qt 5.15 EOL 之后的套件去向（FUT-09）

> **性质**：评估文档，不含代码改动。它回答一个问题：`piplugin_qt` 要不要迁 Qt6、
> 要不要同时支持两个 Qt 大版本。输入是发布评审登记的风险项「Qt 5.15 EOL」
> 与 `docs/todo/adapters.md` #4（Qt 宿主直连）。
> **写作日期**：2026-09-23。文中的「现状」都有可核实的出处（见 §7），
> 到期重估时先复核这几条事实，再看结论还成不成立。

## 1. 结论（先读这一段）

1. **0.4.x / 1.0 冻结窗口内：保持单版本 Qt 5.15.2，不做双版本套件。**
   理由见 §4：套件已经 SHARED 化（APP-08），双版本意味着两份套件 DLL、
   两套 Qt 运行时、两套部署与回归，而本仓库**没有**「同一套件同时服务 Qt5 客户与
   Qt6 客户」的需求——套件是仓库内置的官方适配器，不是对外分发的独立产品
   （要不要把它拆成独立 repo 是另一个决策，不在本文范围内）。
2. **迁移 Qt6 的正解是「一次性切过去」，不是长期双版本共存。** 迁移面比预想小：
   套件里**没有扫到任何 Qt5 专属 API**，而它最关键的私有机制
   （`_q_embedded_native_parent_handle` 外来父窗口嵌入）在 Qt 6.10 里**仍然存在且
   仍被 `qwindows` 平台插件与 `QWidget` 读取**（§3.2）。真正的工作量在构建/部署
   接线与一次几何回归，不是重写套件。
3. **迁移的前置条件是一条 Qt6 编译跑道**（只覆盖 `piplugin_qt` + Qt 插件 + Qt 宿主，
   不含像素回归），加一张把 §3 清单逐条打勾的收尾清单。跑道没绿之前不要切默认
   Qt 版本——否则就是把「套件能不能在 Qt6 编过」的风险压在一次性的迁移提交里。
4. **现在不必急**：5.15 并没有变成无人维护的黑洞——公开归档到 2026-05 仍在出
   5.15.19 的**开源源码包**与 CVE diff（§2.3）。真正的倒计时不是「5.15 发布几年了」，
   而是下面三个触发条件（§6）：ESM/补丁停更、CI 工具链再也编不动 5.15、
   平台矩阵提出 Qt6 才有的要求。

## 2. 事实基线

### 2.1 Qt 5.15 的支持时间线

| 时间 | 事件 |
|---|---|
| 2020-05 | Qt 5.15.0 发布（LTS 通道） |
| 2020-12 | 5.15.2：**在线安装器公开提供的最后一个 Qt 5 二进制** |
| 2025-05-26 | Qt 5.15 的**常规 LTS 支持结束** |
| 2025-05 起 | 转入 ESM（Extended Security Maintenance）等付费服务；此后仍出订阅版补丁版本（5.15.17 / .18 / .19），但只修安全与严重缺陷，不再有新功能 |

对「还能不能用 5.15」的诚实回答：**能用，但从此是「自维护」形态**——
补丁由 Qt 出（订阅侧），开源侧要自己拿源码重建并打 CVE diff。

### 2.2 Qt 6 侧

| 事实 | 数值 |
|---|---|
| 当前 LTS | Qt 6.8（2024-10 发布；商业 LTS 5 年） |
| 已结束支持的例子 | Qt 6.5 于 2026-04 到达 EOS（非 LTS 的 Qt6 版本寿命约 3 年） |
| 最新版本 | Qt 6.11.0（2026-03-23 发布），6.11.2（2026-08-18），6.12 在开发中 |
| C++ 要求 | Qt6 要求 C++17 —— 本仓库已经是 C++17 REQUIRED（`CMakeLists.txt:23`），不是障碍 |

### 2.3 公开归档的证据（5.15 并未停更，只是停了「免费二进制 + 官方支持」）

`https://download.qt.io/archive/qt/5.15/` 的目录本身就能当证据用：

- 版本目录一直排到 **5.15.19**（2026-05-18），中间有 5.15.17（2025-06）、
  5.15.18（2025-10）——ESM 期的补丁版本确实在出；
- `5.15.19/single/qt-everywhere-opensource-src-5.15.19.tar.xz`（634 MB）
  ——**开源源码包仍在发布**，所以「自己编一个打了补丁的 5.15」在许可与技术上都可行；
- 同级目录挂着 `CVE-2025-5455-qtbase-5.15.patch`、`CVE-2025-30348-qtbase-5.15.diff`、
  `CVE-2025-4211-qtbase-5.15.diff` 等——安全补丁以 diff 形式公开，可自行套用；
- 发行版与 KDE 的 [Qt5 Patch Collection](https://community.kde.org/Qt5PatchCollection)
  就是这条路线上的社区维护集合。

**推论**：把「5.15 已 EOL」直接读成「必须在某个日期前迁走」是过度反应；但把它读成
「可以无限期拖着」也不对——**没有官方二进制**意味着换一台干净的 CI 机器就要自己编
Qt（或依赖第三方预编译），这条成本会随时间上升。

## 3. 本仓库与 Qt5 的耦合面（迁移清单）

### 3.1 构建与部署（真正的工作量在这里）

| 位置 | 现状 | 迁移动作 |
|---|---|---|
| `adapters/qt/CMakeLists.txt:23` | `find_package(Qt5 COMPONENTS Widgets Core QUIET)`，找不到就禁用套件 | 改 `find_package(QT NAMES Qt6 Qt5 ...)` + `Qt${QT_VERSION_MAJOR}::Widgets`（一次性切则直接写 Qt6） |
| 同上 `:58` | `Qt5::Widgets` | 同上 |
| `host_kits/qt/`、`tests/test_host_qt`、`tests/test_plugin_qt*`、`examples/minimal_plugin_qt` | 同款 `find_package(Qt5 ...)` | 同款改法（一次性切：全部改 Qt6） |
| 根 `CMakeLists.txt:46~72` | `PI_PLUGIN_QT_PREFIX` / `Qt5_DIR` / `CMAKE_PREFIX_PATH` 优先级链（ECO-05） | 变量名不含版本，逻辑可复用；只需让 `PI_PLUGIN_QT_PREFIX` 指向 Qt6 前缀 |
| 部署 | 套件 SHARED，`bin/<CONFIG>/` 里同时放 `piplugin_qtd.dll` 与 Qt5 运行时 | Qt6 用 `windeployqt` 取运行时；套件 DLL 名与 Qt 大版本的关系需要定名（见 §4.2） |
| `conanfile.py:309~332` | Qt 组件 `piplugin_qt` 的 `libs` / `cmake_target_name`；Qt 不进 conan 依赖 | 若双版本则要能表达「按 Qt 大版本选不同库文件」，这是双版本方案最贵的一块 |

### 3.2 代码层面（比想象中薄）

- **没有 Qt5 专属 API**：对 `adapters/qt/`、`host_kits/qt/`、`examples/minimal_plugin_qt/`、
  `tests/test_host_qt/` 扫 `QRegExp` / `QDesktopWidget` / `QTextCodec` / `QLinkedList` /
  `QMatrix` / `QStringRef` / `setCodecForLocale` 等 Qt5 遗留符号，**零命中**（核实命令见 §7）；
- **嵌入机制在 Qt6 仍然成立**：套件不用 `QWidget::setParent`，而是把宿主容器 HWND 写进
  控件动态属性 `_q_embedded_native_parent_handle`，由 Qt 自己在创建原生窗口时建成
  `WS_CHILD`（见 `adapters/qt/README.md`「宿主窗口嵌入的实现」）。本机 Qt 6.10.0 源码里：
  - `qtbase/src/widgets/kernel/qwidget.cpp` 仍定义
    `activeXNativeParentHandleProperty = "_q_embedded_native_parent_handle"` 并在
    `QWidgetPrivate::createTLSysExtra()` 里转写到 `QWindow`；
  - `qtbase/src/plugins/platforms/windows/qwindowswindow.cpp` 仍有
    `QWindowsWindow::embeddedNativeParentHandleProperty`。
  也就是说这是 **ActiveQt 级别的稳定私有约定**，不是 5.15 的临时补丁；
- **需要留意的语义变化**（迁移时用回归证明，而不是靠猜）：
  1. Qt6 默认启用 high-DPI 缩放。本套件嵌入的是**原生子窗口**，几何走
     `MoveWindow`/父客户区坐标；设备像素比介入后，`pi_plugin_on_resize` 传入的客户区像素与
     Qt 的 `setGeometry()` 之间需要一次「缩放/拖动/多显示器」回归（`tests/test_host_qt`
     已具备窗口，加上 `docs/design/d3d-window-resizing.md` 的像素测量法即可）；
  2. `QApplication` 单实例语义不变（§4.2 的前提）；
  3. Qt6 的 `WId` 是 `quintptr`，套件里 `QVariant::fromValue((WId)(quintptr)parent)`
     的写法两边都成立——但要在 Qt6 上编一次才算数。

## 4. 三个选项的取舍

### 4.1 选项 A：留在 Qt 5.15.2（推荐，0.4.x / 1.0 窗口内）

- **成本**：0（已建成并全绿：`test_host_multi` 双 Qt 插件、`test_host_qt`、像素回归）。
- **收益**：不动就在绿的状态；Qt5 的 API 与嵌入行为是**已知量**。
- **风险**：无官方二进制 → 新 CI 机器要么自带 Qt 5.15.2 安装，要么自己编；
  安全补丁要靠自己打（发行版/KDE 集合）。
- **何时翻案**：§6 的触发条件之一成立。

### 4.2 选项 B：长期双版本套件（不推荐）

做法：同一份源码编两份 SHARED 套件（如 `piplugin_qt5` / `piplugin_qt6`），宿主按自身
Qt 大版本挑一个。

- **SHARED 化（APP-08）到底改变了什么**——这是本卡要回答的核心问题：
  - 它**消除了「同版本多插件」的摩擦**：进程里一份套件状态、一个 `QApplication`。
  - 它**没有**、也不可能让两个 Qt 大版本共存在同一个进程里省事：Qt5 与 Qt6 是两套
    DLL（`Qt5Core.dll` / `Qt6Core.dll`）与两套 `QApplication`，而**一个线程只有一个
    消息队列**，两个 Qt 事件分发器抢同一个 Win32 队列必然互相抢消息；宿主 GUI 线程上
    驱动两套 `processEvents()` 不是「理论上可行」，是「没定义」。
  - 结论：**双版本套件的正确语义是「每个进程选一个」，不是「两种插件混装」**。
    这恰好说明双版本方案的收益上限很低——它服务的场景是「同一台机器上有两个宿主，
    一个 Qt5 一个 Qt6」，而不是「一个宿主里混两种插件」。
- **成本**：两倍 DLL 与部署矩阵、两倍 Qt 运行时、CI 两条 Qt 跑道、套件目标命名与
  conan 组件要能按大版本区分（`conanfile.py` 的 `libs`/`cmake_target_name` 侧最贵），
  以及每次改套件都要在两条 Qt 上各回归一次。
- **判定**：除非真的出现「必须同时供 Qt5 客户与 Qt6 客户」的外部需求，
  否则这是拿长期维护税换一个我们不需要的场景。**否决**。

### 4.3 选项 C：一次性迁到 Qt6（推荐作为 1.0 之后的既定方向）

- **收益**：回到「官方支持 + 官方二进制 + 有 LTS 的版本线」，顺带摆脱 5.15 的
  自维护形态；Qt6 也是后续平台（新编译器、ARM64、非 Windows backend）的现实前提。
- **成本**：§3 的清单一次做完 + 一次几何回归；代码几乎不动（§3.2）。
- **风险**：迁移窗口里 5.15 与 6 的 API 差异若被发现（目前扫描为零），会在跑道上
  暴露——这正是「先加跑道再切」的理由。
- **对既有承诺的影响**：套件**不是** ABI 冻结对象（插件侧 `piplugin_qt`/`piplugin_qt.dll`
  与核心 ABI 无关），所以迁 Qt6 不触碰 `PI_PLUGIN_API_VERSION` 与 1.0 冻结承诺；
  但它是**破坏性部署变更**（Qt 运行时从 `Qt5*.dll` 换成 `Qt6*.dll`），
  故应该在 0.x 内做完，别拖到 1.0 之后当补丁发。

## 5. 前置条件与验收（真要做的时候）

**前置（缺一不可）**：

1. 一条 **Qt6 编译跑道**：CI job 或 `scripts/verify.ps1` 开关，用 Qt6 前缀
   （本机已有 `C:/Qt/6.10.0/msvc2022_64`）configure + build `piplugin_qt`、
   Qt 测试插件、`test_host_qt`，**不跑**像素回归（那是 Qt5 侧的既有资产）。
   与 W-10 的 Linux headless job 同性质：先把「能编」变成事实；
2. 本机/CI 都能**自动拿到 Qt6 运行时**（`windeployqt` 或 `aqtinstall`），
   否则示例与测试宿主在干净机器上跑不起来；
3. 定稿一次「套件目标名与 Qt 大版本的关系」（`piplugin_qt` 保持不变 —— 一次性切，
   还是双版本 `piplugin_qt6` —— 要走选项 B；按 §4.1 的结论，倾向保持不变）。

**验收（迁移提交要能证明的四件事）**：

- Qt6 配置下 `piplugin_qt` 与全部 Qt 目标编过、`bin/<CONFIG>/` 里套件与 `Qt6*.dll` 齐全；
- `tests/test_host_multi`（双 Qt 插件同进程）在 Qt6 上绿 —— 这是 SHARED 套件的核心承诺；
- `tests/test_host_qt`（Qt 宿主，走 `PiPluginEmbedArea`）在 Qt6 上绿；
- 一次**嵌入几何回归**：缩放 / 拖动 / 多显示器 / 最小化恢复后，插件区域不偏移不裁切
  （判据与测量法照 `docs/design/d3d-window-resizing.md` §1 的像素扫描）。

## 6. 触发条件：什么时候必须重新打开这个决策

| 触发条件 | 说明 | 现在的状态 |
|---|---|---|
| 5.15 的补丁停更 | 公开归档不再出现新的 `5.15.x` 目录或 CVE diff | 未触发（最新 5.15.19，2026-05） |
| CI/工具链编不动 5.15 | 例如 MSVC 工具集或 Python 依赖不再支持 Qt5 构建 | 未触发（VS2022 + Qt 5.15.2 仍绿） |
| 平台矩阵提出 Qt6 才有的要求 | ARM64 Windows、新的无障碍/渲染要求、目标发行版只带 Qt6 | 未触发 |
| 有人真的要「Qt5 宿主 + Qt6 插件」混装 | 请先读 §4.2：这条路不通，需求要重新表述 | 未出现 |

到期重估的动作很轻：复核 §2.1~§2.3 四行事实，若其中一行变了，按 §5 的清单执行
选项 C（或按 §4.2 的判定重新评估选项 B）。

## 7. 本次评估用到的核实命令（可复现）

```powershell
# 1) 本仓库对 Qt5 的硬引用面
Select-String -Path CMakeLists.txt,conanfile.py,adapters/qt/CMakeLists.txt `
  -Pattern 'Qt5|Qt6|PI_PLUGIN_QT_PREFIX'

# 2) Qt5 专属 API 扫描（预期：零命中）
Select-String -Path adapters/qt/*.cpp,adapters/qt/*.h,host_kits/qt/*.cpp,`
  host_kits/qt/*.h,examples/minimal_plugin_qt/*.cpp,tests/test_host_qt/*.cpp `
  -Pattern 'QRegExp|QDesktopWidget|setCodecForLocale|QTextCodec|QLinkedList|QMatrix|QStringRef'

# 3) 嵌入机制在 Qt6 是否还在（本机 Qt 6.10.0 源码）
Select-String -Path C:/Qt/6.10.0/Src/qtbase/src/widgets/kernel/qwidget.cpp,`
  C:/Qt/6.10.0/Src/qtbase/src/plugins/platforms/windows/qwindowswindow.cpp `
  -Pattern '_q_embedded_native_parent_handle'

# 4) 本机 Qt 安装（5.15.2 走 msvc2019，6.10.0 走 msvc2022）
Get-ChildItem C:/Qt/5.15.2,C:/Qt/6.10.0 | Select-Object FullName
```

外部出处：

- Qt 5.15 ESM 启动公告（2025-05）：
  https://www.qt.io/zh-cn/blog/extended-security-maintenance-for-qt-5.15-begins-may-2025
  （英文原文 <https://www.qt.io/blog/extended-security-maintenance-for-qt-5.15-begins-may-2025> ；
  5.15 常规 LTS 于 2025-05-26 结束的摘要见
  <https://devbytes.co.in/news/qt-51519-lts-is-now-available>）
- 5.15 公开归档（版本目录、开源源码包、CVE diff）：
  <https://download.qt.io/archive/qt/5.15/>
- Qt6 发布节奏与最新版本：<https://wiki.qt.io/Qt_6.11_Release>
- Qt 6.5 结束支持（非 LTS 版本寿命的实例）：<https://www.qt.io/blog/qt-6.5-reaches-end-of-support>
- Qt 维护期策略（LTS vs 社区）：<https://www.qt.io/development/qt-framework/maintenance-periods>
- KDE Qt5 Patch Collection：<https://community.kde.org/Qt5PatchCollection>
