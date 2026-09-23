# ./examples/conan_consumer/CMakeLists.txt
#
# roadmap ECO-04 的消费方测试件：它**不** add_subdirectory 本仓库的任何东西，
# 只做每个外部消费者都会做的事 —— find_package(piplugin) 然后链接官方 target。
# 由 scripts/verify_package.ps1 在"安装树"和"conan 包"两种形态下各跑一遍。
#
# 默认链接：核心 + 宿主 kit L0 + 事件路由 + imgui 适配器套件（四种都验证一遍）。
# -DPI_CONSUMER_LINK_IMGUI=OFF 时跳过 imgui 套件：conan 形态下目前必须这样，
# 原因见下方注释与 README（CMakeDeps 不传播该组件的外部 imgui 依赖）。
cmake_minimum_required(VERSION 3.23)
project(piplugin_conan_consumer C)

option(PI_CONSUMER_LINK_IMGUI "Link the imgui adapter kit as well" ON)

find_package(piplugin REQUIRED)
message(STATUS "consumer: piplugin_VERSION=${piplugin_VERSION}")

add_executable(pi_consumer main.c)

target_link_libraries(pi_consumer PRIVATE pi::piplugin)
if(TARGET pi::piplugin_host)
    target_link_libraries(pi_consumer PRIVATE pi::piplugin_host)
else()
    message(WARNING "consumer: pi::piplugin_host missing from the package")
endif()
if(TARGET pi::piplugin_events)
    target_link_libraries(pi_consumer PRIVATE pi::piplugin_events)
else()
    message(WARNING "consumer: pi::piplugin_events missing from the package")
endif()

# ---------------------------------------------------------------------------
# imgui 适配器套件：两种分发形态的差别（实测结论，脚本两种形态都跑）
#
#   裸 CMake 安装树（cmake --install）：导出 target 自带 imgui::imgui，
#   消费方直接 link pi::piplugin_imgui 即可，什么都不用补。
#
#   conan 包（Conan 2.10.1 + CMakeDeps）：CMakeDeps 给 pi::piplugin_imgui 生成的
#   target 只带了 pi::piplugin 一个依赖 —— piplugin-debug-x86_64-data.cmake 里
#   piplugin_pi_piplugin_imgui_DEPENDENCIES_DEBUG 只有 pi::piplugin，
#   piplugin_FIND_DEPENDENCY_NAMES 为空 —— 于是 imgui.lib 不在链接行上，
#   静态套件里对 imgui 核心的引用变成 29 个 unresolved external。
#   这就是 verify_package.ps1 在 conan 形态下传 -DPI_CONSUMER_LINK_IMGUI=OFF 的原因：
#   那是**待修的打包问题**，不是消费方写错了。
# ---------------------------------------------------------------------------
if(PI_CONSUMER_LINK_IMGUI AND TARGET pi::piplugin_imgui)
    target_link_libraries(pi_consumer PRIVATE pi::piplugin_imgui)
    target_compile_definitions(pi_consumer PRIVATE PI_CONSUMER_WITH_IMGUI)
elseif(PI_CONSUMER_LINK_IMGUI)
    message(WARNING "consumer: pi::piplugin_imgui missing from the package")
endif()
