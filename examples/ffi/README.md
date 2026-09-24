# FFI examples — piplugin 从别的语言看是什么样

roadmap **ECO-06**。框架的卖点是**纯 C ABI**：`__stdcall` 函数指针、`#[repr(C)]` 能表达的结构、
不跨边界的 C++ 类型。这个承诺是对**别的语言**说的，所以只有真的跑起来才算数。

三个例子做同一件事（各自语言里独立实现，没有任何绑定生成器、没有胶水层）：

1. 加载框架核心 DLL 与一个官方测试插件；
2. 取工厂，**QueryInterface**（COM 风格的那一步：命中返 `PI_OK`，未命中返
   `PI_E_NOINTERFACE` 且 `*out == NULL`）；
3. 读 descriptor（名称 / 版本 / `api_version` / capabilities / properties）；
4. **在宿主语言里造一个宿主对象**（一张函数指针表 + 一个结构体）交给
   `pi_plugin_factory_create_instance`，然后 `pi_plugin_initialize` / `pi_plugin_terminate` / `release`；
5. 卸载后再加载一次 —— 干净的卸载是"能再加载"的前提，也是宿主最常写错的一步。

第 4 步是重点：**宿主侧契约就是一张 C 函数指针表**，所以任何能按 C 调用约定回调的语言
都能写宿主。三个例子里各有一个"用本语言实现的 `IPiPluginHostServices`"。

| 语言 | 目录 | 怎么跑 |
|---|---|---|
| Python | [`python/`](python/) | `python examples/ffi/python/pi_ffi_demo.py bin/Debug/pi_plugin_test_plugin_imgui.dll` |
| Rust | [`rust/`](rust/) | `cargo run --manifest-path examples/ffi/rust/Cargo.toml -- bin/Debug/pi_plugin_test_plugin_imgui.dll` |
| C# | [`csharp/`](csharp/) | `dotnet run --project examples/ffi/csharp -- bin/Debug/pi_plugin_test_plugin_imgui.dll` |

统一跑法（按机器上装了哪些工具链自动决定跑几个，缺的语言打印 SKIP 而不是失败）：

```powershell
pwsh -NoProfile -File scripts/verify_ffi.ps1
```

`scripts/verify.ps1` 把它作为第 3 项检查，所以只要装了对应工具链，CI 也会跑。

## 为什么每个例子里都手抄了一遍类型定义

因为那正是"纯 C ABI"的意思：**类型是自解释的**。三个文件里都能看到同一张表：

```c
typedef struct IPiUnknownVtbl {
    PiResult (PI_CALL *pi_query_interface)(void*, const PiGuid*, void**);
    uint32_t (PI_CALL *pi_add_ref)(void*);
    uint32_t (PI_CALL *pi_release)(void*);
} IPiUnknownVtbl;
```

对照阅读顺序：基础层的 `pibase/pi_base.h`（返回码 / GUID / 上面这张表的根接口）→
`include/piplugin/pi_plugin_types.h`（本库自己的 IID、描述符、API 版本）→
`pi_plugin_factory.h` / `pi_plugin_base.h`（生命周期）→
再回来看任一语言的实现。ABI 的边界在哪、为什么不需要胶水层，看完就够了。

## 已知边界（诚实说明）

- 三个例子目前都只加载**不建窗口**的路径：它们创建实例、初始化、终止，但不
  `pi_plugin_attach`（那需要宿主语言提供真实窗口句柄，属于各自 GUI 工具包的事）；
- 例子的加载器用 `kernel32`（Windows）。Linux/macOS 上把 `LoadLibraryA` 换成
  `dlopen` 即可 —— 本仓库只在 Windows 上验证过（见 freeze review §5）；
- Rust 例子**不依赖任何 crate**（否则就等于承认需要一个绑定层），C# 例子不用 NuGet 包。
