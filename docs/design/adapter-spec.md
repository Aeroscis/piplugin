# 适配器套件规范（Adapter Kit Spec）

> 对应 roadmap **ECO-01**（配套 `docs/todo/adapters.md`）。
> 读者：想为 piplugin 写一个官方或第三方 UI 适配器套件的人。
> 判据：**照本规范 + 官方宿主，能独立写出一个可用的最小套件** —— 这件事不是口号：
> `examples/minimal_kit_win32/` 就是照本文写的（~250 行纯 C，零工具包），
> 并且通过了官方一致性验收（见 §9）。
>
> 本文只讲"套件与框架、宿主之间的契约"；具体某个工具包怎么嵌窗口属于实现细节
> （Qt 的做法见 `adapters/qt/README.md`，D3D 宿主侧的知识见 `docs/design/d3d-window-resizing.md`）。

## 1. 套件是什么

套件把"某个 UI 工具包的兼容层"从插件代码里抽出来，**一行 C ABI 都不改**。它由两部分组成：

| 部分 | 形态 | 谁链接 | 例子 |
|---|---|---|---|
| **套件库** | STATIC 或 SHARED（见 §8） | 插件 | `piplugin_qt`、`piplugin_imgui`、`examples/minimal_kit_win32` |
| **套件头** | 一个描述符结构 + create 函数 | 插件 | `pi_qt_view.h`、`pi_imgui_view.h`、`pi_win32_view.h` |

插件侧只需要写"纯 UI 逻辑"：Qt 套件要一个 widget 工厂，imgui 套件要一个 draw 回调，
最小 Win32 套件要一个 paint 回调。

**套件不能做的**（越层即违约）：

1. 不创建顶层窗口、不决定布局、不选择父窗口 —— 容器是**宿主**创建并交给 `pi_attach(parent)` 的；
2. 不在宿主线程之外跑自己的消息循环（见 §5）；
3. 不假设自己是进程里唯一的套件/唯一的插件（见 §8）。

## 2. 必须实现的接口

套件至少要实现 `IPiPluginView`（7 个槽位 + 继承来的 3 个），逐条要求如下。
`examples/minimal_kit_win32/pi_win32_view.c` 是每条的最小实现；官方两个套件是完整实现。

| 槽位 | 必须做到 | 常见错误 |
|---|---|---|
| `pi_query_interface` | 认领 `PI_IID_UNKNOWN` 与 `PI_IID_PLUGIN_VIEW`，**返回时已 AddRef**；失败时 `*out = NULL` | 返回未 AddRef 的指针（宿主一定会 release） |
| `pi_add_ref` / `pi_release` | 用 `PiRefCountedBase` 实现；归零时同步销毁（见 §6） | 在别的线程里异步析构 |
| `pi_attach(parent)` | **第一次**才创建资源；成功返回 `PI_OK`；已 attach 再调返回失败（不要静默重复创建） | 在 create 时就建窗口（此时还没有容器） |
| `pi_detach()` | **幂等**；返回时资源全部销毁；之后**可以再次 attach** | 只隐藏窗口、不销毁；或 detach 后留下悬垂 HWND |
| `pi_get_native_window()` | 返回**借用**句柄；未 attach 时返回 `PI_INVALID_WINDOW` | 在这里创建窗口 |
| `pi_on_resize(w, h)` | 让插件内容跟随宿主给的尺寸；未 attach 时是安全的空操作 | 自己决定尺寸、忽略宿主给的 w/h |
| `pi_on_idle()` | 宿主每帧（或每个循环）调一次；把"一帧"跑完；未 attach 时安全 | 在里面阻塞、或自己起定时器决定节奏 |
| `pi_get_preferred_size()` | 有真意见就给建议值；**没有就返回 `PI_E_NOTIMPL`** | 硬编码一个数字并返回 `PI_OK`（见 freeze review F7） |
| `pi_set_visible(v)` | attach 之后可切换可见性；未 attach 时安全 | 把可见性与"是否创建"混为一谈 |

**宿主侧的两条义务**（写在这里，因为套件可以依赖它们）：

1. 每帧调用一次 `pi_on_idle()`（这是插件 UI 的心跳）；
2. 卸载插件前先 `pi_detach()` + `pi_release()`；套件的 `pi_terminate` 兜底钩子（§7）在。

## 3. 生命周期契约（最容易出错的部分）

```
宿主编译期            插件模块
────────────────────────────────────────────────────────────
pi_module_load        ← 只有代码被映射，什么都还没创建
pi_factory_create_instance
pi_plugin_initialize
pi_plugin_get_view    → 套件 create：**只造对象，不造资源**
pi_view_attach(容器)  → 套件在这里创建第一个窗口/设备/控件
   … 每帧 pi_on_idle / 尺寸变化 pi_on_resize …
pi_view_detach        → 套件销毁资源（同步！）
pi_iunknown_release   → 引用归零：若仍 attach，先内部 detach，再释放对象
pi_plugin_terminate   → 插件调套件的 shutdown 兜底（幂等）
工厂/实例 release
pi_module_unload      ← 到这里，套件在模块里的一切必须已经不存在
```

规则（按重要性排序）：

1. **attach 之前不建任何原生资源**。宿主可能只是探测、也可能永不 attach；
   在 create 里建窗口的套件会让"没有容器的宿主"必崩。
2. **detach 必须同步**：返回时资源已经没了。宿主返回后可能立刻 `FreeLibrary`。
3. **detach → attach 必须可重复**：宿主可以反复挂载/卸载同一实例（多插件宿主、
   布局切换都会这么做）。
4. **对象与资源分开**：view 对象在 `pi_get_view()` 之后就可能被宿主持有；
   资源只在 attach 期间存在。
5. **不要缓存宿主给的裸指针**（比如 `IPiPluginView*` 自身之外的宿主对象）：
   需要什么就现取，或者加引用。

## 4. 线程模型申报（每个套件必须二选一，并写进自己的头文件）

| 模型 | 含义 | 套件必须满足 | 插件侧的代价 |
|---|---|---|---|
| **A. 全宿主线程**（推荐） | 套件一切调用都在宿主调 `pi_on_idle()` 的那条线程上 | 没有私有线程；回调直接同步执行 | 插件无需加锁，可直接碰 UI/句柄 |
| **B. 私有线程 + marshal** | 套件自建线程跑工具包的消息循环，宿主调用 marshal 过去 | 必须保证：`pi_detach()` 返回时控件已销毁；`pi_release()` 返回时线程已 join | 插件回调在私有线程上，插件必须自己加锁 |

本仓库的现状与理由：

- `piplugin_imgui`：模型 A（imgui 是立即模式，没有自己的事件循环，没有线程）；
- `examples/minimal_kit_win32`：模型 A（GDI + 宿主消息泵）；
- `piplugin_qt`：**曾经**是模型 B，并且**因此付过代价**——后台线程跑
  `QApplication::exec()` + queued invocation + 等控件销毁的信号量，结果是卸载崩溃与
  detach 死锁。现在也是模型 A（`QApplication` 建在宿主 GUI 线程上，`pi_on_idle()` 里
  `processEvents()` 给 Qt 一小片时间）。**要选 B，请先读完 `adapters/qt/README.md` 的
  "线程模型（重要，不要改回去）"。**

**为什么 A 是推荐**：Win32 的父子窗口必须同线程（销毁/重挂子窗口会向父窗口同步发
`WM_PARENTNOTIFY`/`WM_DESTROY`），而插件的控件窗口通常正是宿主容器的子窗口；
"哪个线程做 marshal"这件事一旦交给套件，就等于把死锁风险复制到每个套件里。

## 5. 宿主交互的两个细节

**容器**：`pi_attach(parent)` 给的 `PiNativeWindow` 是宿主创建的容器。套件要做的只有
"让我的控件成为它的子窗口/嵌入其中"：

- Qt：用 `_q_embedded_native_parent_handle` 让 **Qt 自己**把控件建成容器的 `WS_CHILD`
  （`SetParent` 兜底路径在 README 里写明）；
- imgui：套件自己 `CreateWindowExW(..., WS_CHILD, parent, ...)`；
- 最小 Win32 套件：同上。

**尺寸**：只信 `pi_on_resize(w, h)` 给的数字，别去读容器的实际大小当唯一真相
（宿主可能在布局发生前就通知你）。

## 6. 每一帧做什么（`pi_on_idle`）

- 把工具包的"一帧"跑完：imgui 是 `NewFrame → draw → Render → Present`；
  Qt 是 `processEvents(AllEvents, 有上限的毫秒数)`；GDI 套件是
  `InvalidateRect + UpdateWindow`。
- **节奏归宿主**：套件不得自己创建定时器决定帧率（宿主可能 60Hz，也可能只在
  有输入时驱动）。官方 Qt 套件提供的内部 `QTimer(0)` 是**默认关闭的 opt-in**。
- **不要在里面阻塞**：宿主的主循环被你占住多久，宿主的界面就卡多久。

## 7. shutdown-before-FreeLibrary 契约

插件模块被卸载后，模块里的代码、窗口类、WndProc 全都不存在了。所以：

- 宿主的正常路径：`pi_detach()` + `pi_release()` → 套件在 `pi_release()` 归零时
  同步销毁一切（这就是 §3 的 detach 必须同步的原因）；
- **兜底路径**：套件应提供一个 `pi_<kit>_shutdown()`（幂等、可在 attach 状态下调用），
  由插件的 `pi_terminate()` 调用 —— 这样"宿主直接丢模块"也不会崩；
- 兜底路径还要**注销自己注册的窗口类**（Windows 上窗口类的 WndProc 属于模块，
  模块卸载后类必须已经不存在）。`examples/minimal_kit_win32` 的
  `pi_win32_view_shutdown()` 演示了这一点；
- 注册窗口类时用
  `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, &WndProc, &module)`
  取**套件所在的模块**，不要用 `GetModuleHandle(NULL)`（那是 exe）。

## 8. 进程级状态与部署形态（STATIC vs SHARED）

| 形态 | 适用 | 要求 |
|---|---|---|
| **STATIC** | 套件没有进程级状态（或状态天然只属于一个模块），如 `piplugin_imgui` | 源码随插件编译；注意**每个插件 DLL 一份**静态数据 |
| **SHARED** | 套件持有进程级状态（唯一 `QApplication`、全局登记表），如 `piplugin_qt`（APP-08） | 显式导出（`PI_QT_API` 这类宏）、部署时把套件 DLL 放在插件旁边、**不能靠"每模块一份"来隔离状态** |

判定口诀：**"两个插件同时加载"时会不会互相踩？** 会 → SHARED，并且套件要提供
按 owner 作用域的销毁入口（`pi_qt_view_shutdown_owner(owner)`），
因为"拆掉整个进程的视图"在多插件进程里是错的。

## 9. 一致性验收（进生态列表的条件）

套件作者的自验流程（与官方套件同一条路径）：

```powershell
# 1) 用套件写一个测试插件（照 examples/ 任一插件改）
# 2) 让官方一致性宿主跑它：load → attach → idle → resize → unload，多轮
pwsh -NoProfile -File scripts/run_selftest.ps1 -Plugin my_plugin.dll -Cycles 3
# 3) 想看生命周期每一步：开套件的 trace（§10），再跑一次
```

验收通过的标准（`run_selftest.ps1` 的退出码）：

- 每一轮都完整跑完，宿主日志出现 `selftest: PASS`，没有 `selftest: FAIL`；
- 尺寸往返（拉伸/还原）不卡、不崩；
- 卸载后进程仍然健康（能继续加载下一个插件）。

**"跑过 cycles = 进生态列表"**：README 的适配器列表只收录通过上述验收的套件
（见 `docs/design/conformance.md`）。`examples/minimal_kit_win32` 已经跑过，
它的验收输出记录在 ECO-01 的提交里。

## 10. trace 约定（`PI_<KIT>_TRACE=1`）

有生命周期状态的套件应当提供可开关的 trace，命名与行为统一：

- 环境变量：`PI_<KIT>_TRACE=1`（例如 `PI_QT_VIEW_TRACE`、`PI_WIN32_VIEW_TRACE`）；
- 输出：可执行文件目录下的 `pi_<kit>_view.log`（追加写）；
- 每行：`[<kit> <线程 id>] <事件>`，至少要覆盖 attach / detach / 资源创建与销毁；
- 开关关闭时**零开销、零文件**（惰性打开日志）；
- 为什么值得做：本仓库历史上每一次"卸载崩溃 / detach 卡死"，第一步都是看 trace 里
  "哪一步在哪个线程"。

## 11. 交付前自查清单

- [ ] `pi_attach` 之前没有创建任何原生资源（`pi_get_view()` 只造对象）；
- [ ] `pi_detach()` 幂等、同步、之后可再次 attach；
- [ ] `pi_release()` 归零时把所有资源同步销毁；
- [ ] 提供了幂等的 `pi_<kit>_shutdown()`，并在 §7 的意义下注销了窗口类；
- [ ] 头文件里写明了线程模型（A 或 B）与 user_data 的 retain/release 语义；
- [ ] `pi_get_preferred_size()` 没有硬编码谎言（要么真意见，要么 `PI_E_NOTIMPL`）；
- [ ] 每帧工作有上限、不阻塞宿主；
- [ ] STATIC/SHARED 的选择按 §8 的判定口诀做过，并处理了多插件同进程；
- [ ] 通过 `scripts/run_selftest.ps1` 的多轮验收；
- [ ] README 写清"宿主必须做什么"（通常只有两条：每帧 idle、卸载前 detach+release）。

## 12. 参考实现对照

| 条款 | `piplugin_imgui` | `piplugin_qt` | `examples/minimal_kit_win32` |
|---|---|---|---|
| 形态 | STATIC | **SHARED** | STATIC |
| 线程模型 | A | A（历史上是 B，付过代价） | A |
| 资源创建时机 | attach（子 HWND + D3D + ImGui context） | attach（控件 + `QApplication`） | attach（子 HWND） |
| detach 语义 | 同步销毁全部 | 同步销毁全部 | 同步销毁全部 |
| shutdown 兜底 | 无（无跨模块状态） | `pi_qt_view_shutdown_owner()` | `pi_win32_view_shutdown()` |
| `get_preferred_size` | 硬编码 400×300（F7，待改） | 硬编码 400×300（F7，待改） | **`PI_E_NOTIMPL`（推荐做法）** |
| trace | 无 | `PI_QT_VIEW_TRACE=1` | `PI_WIN32_VIEW_TRACE=1` |
