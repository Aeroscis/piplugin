# 待办清单总览

> 本文件夹按主题拆分待办：`platform.md`（跨平台）、`adapters.md`（UI 适配器）、
> `framework.md`（核心框架）、`build.md`（构建/打包）、`tests.md`（测试与质量）。
> 每个文件只记**还没做**的事（标题不带完成标记的条目）与已完成项的**结论 + 复核入口**；
> 已完成项当时的方案原文在 `git log` 与 `CHANGELOG.md` 里，通用知识在 `docs/design/`。

## 优先级图例

| 标记 | 含义 |
|---|---|
| [P0] | 高优先级：影响正确性/安全/核心承诺 |
| [P1] | 中优先级：明确的功能缺口 |
| [P2] | 低优先级：打磨/增强 |
| [P3] | 备忘：不影响使用，等一个决策或一次顺手 |

## 开放项（全部待办都在这张表里）

| 主题 | 条目 |
|---|---|
| 跨平台 | `platform.md` #1 Linux（X11）UI 嵌入 [P1] · #2 macOS（NSView）UI 嵌入 [P1] · #3 插件 DLL 非 Windows 构建 [P1]（W-10 实测钉死了范围）· #4 macOS 侧编译器矩阵 [P2] |
| UI 适配器 | `adapters.md` #2 gtk 套件 [P2] · #3 webview 套件 [P2] · #5 imgui 套件非 Windows backend [P1]（与 `platform.md` #1/#2 同一件事） |
| 核心框架 | `framework.md` #3 插件热重载（FUT-04）[P2] · #9 ABI 2.0 多视图 API 的时间窗 [P2] |
| 构建打包 | `build.md` #6 库名/产物名一致性（约定已成文，剩下在 CI 里断言一次）[P2] · #8 安装/分发的三个遗留 [P3] · #9 toolchain file 未统一 [P3] |
| 测试质量 | `tests.md` #4 多场景覆盖矩阵里"imgui 宿主 + Qt 插件"一格仍未纳入自动化 [P2] |

## 待维护者拍板（不由贡献者或 agent 替拍）

| 事项 | 输入 | 说明 |
|---|---|---|
| clang-format 强制与否 | `verify.ps1` 第 4 项的漂移清单 | 现状：只报告不拦截（对 BLK-05 的有意偏离）；强制的代价是一次覆盖全仓的格式化 diff |
| clang-tidy 的检查集与允许表 | `tests.md` #8 的实测（LLVM 22：4 warning / 0 error） | 已评估"值得接入"，但先定检查集与允许表再谈强制 |
| Qt6 方向 | `docs/design/qt6-assessment.md` | 单版本 5.15.2 维持 vs 一次性切 Qt6；拍板前置是 Qt6 编译跑道 |
| ABI 2.0（多视图 API）的时间窗 | `docs/todo/framework.md` #9 | 与 1.0 的冻结承诺绑在一起，需要显式决策 |
| Qt 运行时是否纳入分发 | `docs/todo/build.md` #8 | LGPL 再分发决策；现在只有构建树里有 Qt 运行时 |
| 双仓 / CI 主机长期策略 | `docs/todo/build.md` #2 的预案 | 现状：Gitee 主 + GitHub 镜像跑 Actions |

> **关于未入库的派工件**：发布路线图与并行派工板是**一次性的派工件**（描述当时的执行
> 计划，入库就会变成需要长期维护的"第二份真相"），所以它们**有意不入库**；本仓库的
> 文档因此不出现它们的文件名或链接。已落地的 W-01…W-13 逐条记在 `CHANGELOG.md` 里。
