# minimal_host — 最小宿主

一个窗口、一个容器、一个插件的宿主，**机制全部来自宿主 kit**（加载 / 双向能力门禁 /
实例化 / 七步卸载序列），本文件只剩宿主自己的决策：窗口长什么样、容器放哪、什么时候 pump。

它同时能驱两类插件：

- 有 view 的（imgui / Qt 插件）→ attach 到宿主的容器并每帧 `pi_plugin_view_on_idle()`；
- 只有 service 的（headless 插件）→ `pi_plugin_service_start/poll/stop`。

## 三步跑通

```powershell
# 1) 在仓库根构建（会连带构建 examples/ 下的插件）
cmake --preset conan-default
cmake --build --preset conan-debug --parallel

# 2) 进开发目录（依赖 DLL 都在这里）
cd bin\Debug

# 3) 跑：参数是插件 DLL，不给就用 imgui 示例插件
.\pi_plugin_example_minimal_host.exe pi_plugin_example_plugin_imgui.dll
.\pi_plugin_example_minimal_host.exe pi_plugin_example_service.dll
```

期望输出（imgui 插件）：

```
== piplugin minimal host ==
plugin: pi_plugin_example_plugin_imgui.dll
  [kit] load[0]: gate passed (category=Example/UI, capabilities=2)
  [kit] load[0]: ready (view:Y service:N events:N)
loaded: Example ImGui Plugin 1.0.0 (Example/UI)
  [kit] attach[0]: plugin window=... container=...
view: attached to our container (native window ...)
running for 1500 ms...
  [kit] unload[0]: detach view / release view / terminate plugin / unload module / done
RESULT: PASS
```

服务插件那次会一路打印 `[plugin message] msg=0x8001 wparam=N`（插件每 poll 一次就报一次数），
最后 `service: status=2 before stop` → 卸载序列里 stop → `RESULT: PASS`。

## 读代码的顺序

1. `pi_plugin_host_services_create_default()` + `pi_plugin_host_session_create()`：宿主对象的两个前提；
2. `pi_plugin_host_session_load()`：一次调用里完成"加载 → 门禁 → 实例化 → 初始化"；
3. `pi_plugin_host_session_attach_view()`：**容器是我们创建的**，kit 只接收它；
4. 主循环：`PeekMessage` + `pi_plugin_host_session_drive_idle()` + `pi_plugin_service_poll()`；
5. `pi_plugin_host_session_unload()`：七步卸载序列内化，宿主不手写顺序。

下一步可以看：

- `examples/service_plugin`：这个宿主正在驱动的那个服务插件；
- `examples/specialized_app`：app 定义自己的协议并对插件做门禁（通道 A/B）；
- `docs/tutorial/write-host.md`：完整教程；`host_kits/README.md`：kit 的三层纪律。
