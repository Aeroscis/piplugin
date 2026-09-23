# service_plugin — 服务插件（headless，IPiService）

没有 UI 的插件：宿主的主循环调 `start / poll / stop`，插件干活。
任务服务器、导入器、后台 worker 在 piplugin 里就长这样。

## 三步跑通

```powershell
cmake --preset conan-default
cmake --build --preset conan-debug --parallel
cd bin\Debug
.\pi_example_minimal_host.exe pi_example_service.dll
```

期望输出：`service: start -> 0`，然后每 poll 一条 `[plugin message] msg=0x8001 wparam=N`，
`service: status=2 before stop`（2 = `PI_SERVICE_RUNNING`），
卸载序列里再 stop 一次（幂等），`RESULT: PASS`。

## 值得抄的几点

- descriptor 只声明 `PI_IID_SERVICE PROVIDES`、**不 REQUIRE 任何东西**：GUI 宿主与
  headless 宿主都能加载它；
- 一个插件对象承载 `IPiPluginBase`，`QueryInterface(PI_IID_SERVICE)` 交出一个**独立包装对象**
  —— 两个接口需要两套 vtbl 布局，不能指望同一个指针按两种布局解释（否则
  `pi_service_start()` 会调到 `pi_initialize()` 的槽位）；
- `pi_service_stop()` **幂等**：卸载序列总会再调一次；
- `poll()` 里不阻塞宿主线程，进度用 `pi_host_post_message()` 报出去；
- `pi_get_view()` 返回 `PI_E_NOINTERFACE` —— 这是"我没有 UI"的正确说法。

配套阅读：`docs/tutorial/write-host.md` §4（headless 宿主要点）、
`docs/design/interfaces.md` 2.6（IPiService 的四条行为承诺）。