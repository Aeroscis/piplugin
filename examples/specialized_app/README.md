# specialized_app — 特化 app（通道 A + 通道 B）

一个 app 想要**超出框架词汇表**的东西时该怎么做。本例把三件事放在一起：

| 通道 | 谁定义 | 谁实现 | 本例子里的东西 |
|---|---|---|---|
| **A** | app | 插件 | `IMyAppJobQueue`（提交任务 / 读任务数），GUID `0x8E52B1D7…` |
| **B** | app | app（宿主对象） | `IMyAppInfo`（应用名 / 已加载插件数），GUID `0x3F91C6A4…` |
| **C** | — | 双向 | 事件机制，见 `tests/test_host_events`（本例不重复演示） |

关键在于：**这两套接口的 GUID 与 vtbl 都是 app 自己的**，框架只提供一个 GUID 比较函数、
一个 QI、以及"实例化之前先跑门禁"的机制。app 因此不需要 fork 框架。

## 三步跑通

```powershell
cmake --preset conan-default
cmake --build --preset conan-debug --parallel
cd bin\Debug
.\pi_example_specialized_app.exe pi_example_specialized_plugin.dll pi_example_service.dll
```

期望输出（节选）：

```
[app] this app requires com.example job-queue plugins
[kit] load[0]: gate passed (category=Example/Worker, capabilities=2)
[job plugin] running inside 'ExampleApp' (the app says 1 plugin(s) are loaded)   <- 通道 B
[app] accepted 'pi_example_specialized_plugin.dll'
[job plugin] accepted job 'import-photo' as #0 (total 1)                          <- 通道 A
[job plugin] accepted job 'export-video' as #1 (total 2)
[app] the plugin accepted 2 job(s)
[kit] load[1]: pi_example_service.dll
[app] rejected 'pi_example_service.dll' (hr=-8): plugin does not provide iid data1=0x8E52B1D7 …
[app] as expected: the gate kept a non-conforming plugin out
RESULT: PASS
```

第三个参数可以是**任意**插件：只要它没有声明 `PI_PLUGIN_CAP_PROVIDES MY_APP_JOB_IID`，
就会被门禁挡在 `create_instance` 之前（`hr=-8` = `PI_E_MISSINGCAPABILITY`）。

## 三个角色的分工

**app 定义协议**（`my_app_protocol.h`）

- GUID 用**随机 128 位 UUID**；`data1 < 0x80000000` 是框架保留区，不要自己去编号
  （`python -c "import uuid; print(uuid.uuid4())"`）；
- vtbl 里只放 C 类型，第一个成员仍是 `IPiUnknownVtbl`（于是它也享受 QI 与引用计数）；
- 顺手写两个 inline 帮助函数，调用方就不用直接碰 `lpVtbl`。

**插件实现协议**（`pi_specialized_plugin.c`）

- 在 descriptor 里**如实声明** `PI_PLUGIN_CAP_PROVIDES` —— 门禁读的就是它；
- `QueryInterface(MY_APP_JOB_IID)` 交出**独立的包装对象**（两套 vtbl 不能共用一个指针）；
- 反向也要能降级：app 没提供 `IMyAppInfo` 时只是打印一行，不影响加载（声明为 `PI_PLUGIN_CAP_OPTIONAL`）。

**app 消费协议并做门禁**（`pi_specialized_app.c`）

```c
/* 1) 声明"我的生态里插件必须实现它" —— 这一条在 create_instance 之前生效 */
pi_plugin_host_session_require(session, &MY_APP_JOB_IID);

/* 2) 把自己的服务挂到宿主对象上（通道 B）：框架 IID 之外的 QI 转给你 */
pi_plugin_host_services_create_ex(&OnMessage, NULL, PI_INVALID_WINDOW,
                           &AppExtraQi, NULL, &services);

/* 3) 门禁通过后取协议：QI 返回的是 add-ref 过的指针，用完 release */
IPiPluginBase* plugin = pi_plugin_host_session_get_plugin(session, slot);   /* 借用 */
IMyAppJobQueue* jobs = NULL;
if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)plugin, &MY_APP_JOB_IID,
                                             (void**)&jobs))) {
    int32_t id = -1;
    my_app_submit_job(jobs, "import-photo", &id);
    pi_iunknown_release((IPiUnknown*)jobs);
}
```

配套阅读：`docs/design/interfaces.md` §5（GUID 规则、三步走、版本演进、三条禁令）。
