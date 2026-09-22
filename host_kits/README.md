# 宿主侧 kit（Host Kits）

宿主侧 kit 的目的只有一个：**消灭宿主程序里重复的「机制」代码，不占有任何 UI 决策权**。

它存在的原因是三份测试宿主曾各自手抄同一段「加载 → 能力门禁 → 实例化 → 七步卸载」序列
（`tests/test_host`、`tests/test_host_qt`、`tests/test_headless_host`）——顺序抄错一次就是卸载崩溃。
kit 把这段顺序收拢成一份实现，把「窗口长什么样」完整留给宿主。

## 三层结构（release-roadmap.md §1.1）

| 层 | 形态 | 内容 | 决策权归属 |
|---|---|---|---|
| **L0 会话管理** | 纯逻辑库，零 GUI 依赖 | 加载 / 能力门禁 / 实例化 / 多插件列表 / 七步卸载序列 | 布局、样式全归宿主 |
| **L1 嵌入胶水** | per-framework 小库 / header | 把**宿主自己创建的**容器变成 embed host：attach、idle 驱动、resize 转发；D3D 宿主的 flip-model 交换链 + `WS_CLIPCHILDREN` 正确创建 | 容器是谁、在哪、多大、几个、可否见，全归宿主 |
| **L2 现成控件** | opt-in 糖 | 开箱即用的「插件面板」控件（如带 tab 的 dock） | 使用者自愿放弃部分自由换速度 |

依据：ABI 层 `pi_attach(parent_window)` 本来就是宿主递容器、`pi_host_default_set_ui_window`
支持运行时切换、`pi_set_visible` 支持 attach 后隐藏——**「窗口 / 内嵌 / 隐藏 / 不取 view」
本来就是宿主的自由**，kit 不得越层。

## 纪律（新增代码必须遵守）

1. **L1 只接收宿主提供的容器**，绝不替宿主创建顶层窗口或决定布局；
2. **L2 保持极薄**：纯容器包装、无内嵌视觉装饰、样式 API 全穿透（QSS 等可完整覆盖）；
3. **L2 必须显式 opt-in**（单独 target / 单独头文件），文档明示「不用它完全没问题」；
4. **L0 不含任何 UI 决策**：不创建窗口、不持计时器、不决定布局与可见性策略；
   它只做机制，并让宿主决定「何时 pump、把 view 嵌到哪」。

## 目录与开关

```
host_kits/
  core/    L0：pipluginframework_host       （STATIC，纯 C，零 GUI 依赖）
  qt/      L1：pipluginframework_host_qt    （STATIC，Qt5；依赖 L0）
  dx11/    L1：pipluginframework_host_dx11  （STATIC，仅 Windows；只依赖核心）
```

开关树与 `adapters/` 同构（Conan 侧同名选项整批转发，见 `conanfile.py`）：

- `PI_BUILD_HOST_KITS` —— 总开关；关死后所有宿主 kit 一律不编；
- `PI_BUILD_HOST_KIT_CORE` / `PI_BUILD_HOST_KIT_QT` / `PI_BUILD_HOST_KIT_DX11` —— 分层分开关。

## 当前状态

- **L0（`core/`）已落地**：`pipluginframework_host`，API 见 `core/pi_host_session.h`。
- **L1（`qt/`、`dx11/`）已落地**：
  - `qt/`：`PiPluginEmbedArea` —— 容器包装 + attach + resize 转发 + idle 驱动（`driveIdle()`
    手动或 `setAutoIdleEnabled()` 内部 `QTimer(0)`，默认关闭）；
  - `dx11/`：设备 + flip-model 交换链的正确创建参数（`DXGI_SCALING_NONE` 降级路径、
    帧延迟等待对象、背景色）、resize 策略（只增不减 / 精确跟随）、容器与窗口风格。
- 三个测试宿主都已改用这些 kit；进程名、路径、命令行参数与日志契约保持不变
  （`scripts/run_selftest.ps1` 与 `scripts/verify_resize_fix.ps1` 是出口判据）。
- **L2 本期不做**：留到有真实需求再评估，避免生态引力过早固化样式。

### 一处对 roadmap 草图的偏离（有意）

roadmap §APP-03 把 Qt 侧写成 `attach(view)`。实际实现是
`attach(PiPluginHostSession* session, uint32_t slot, bool set_visible)`，原因是**悬垂指针**：
若控件缓存裸 `IPiPluginView*`，插件卸载后控件仍握着已释放的 view，下一次 resize 转发就是
use-after-free。绑定 `(session, slot)` 后，每次需要 view 都向 session 现取，卸载后自动拿到
NULL，"忘掉清理"这个出错面被彻底消掉。宿主侧仍然只有一句
`g_embedArea->detachBinding()` + `pi_host_session_unload()`。

## 库形态为什么是分层的

- **L0 = 真库**（STATIC）：纯逻辑、无框架/工具包 ABI 耦合，库化零代价；session 是实例对象、
  没有进程级全局状态，因此不会重演 APP-08 那个「每 DLL 一份全局状态」的坑。
- **L1 Qt = CMake 上是库 target、物理上是源码**：`PiPluginEmbedArea` 是 `QWidget` 子类、要跑 moc，
  预编译库会把宿主的 Qt 版本 + 编译器版本 + 运行库锁死——**那正是本框架用 C ABI 要消灭的
  C++ ABI 耦合，宿主 kit 自己不该把它引回来**。所以做成 STATIC target：宿主
  `target_link_libraries` 即可，源码随宿主编译。
- **L1 DX11 = 真库**（STATIC）：它只碰 Win32/D3D11 这类系统 ABI（不碰任何 C++ 运行时 ABI），
  没有 moc 那类问题；固化的是一组**创建参数与策略**，正是最该被复用而不是被抄写的东西。
