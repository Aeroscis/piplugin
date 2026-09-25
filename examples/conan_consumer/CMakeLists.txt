# ./examples/conan_consumer/CMakeLists.txt
#
# roadmap ECO-04 的消费方测试件：它**不** add_subdirectory 本仓库的任何东西，
# 只做每个外部消费者都会做的事 —— find_package 然后链接官方 target。
# 由 scripts/verify_package.ps1 在"安装树 / conan 包 / cpack 归档"三种形态下各跑一遍。
#
# 入口按形态分（见 docs/design/build-system.md §5）：
#   安装树 / cpack 归档：find_package(pi REQUIRED COMPONENTS ...) —— 家族入口，
#                        组件名就是 target 的成员名（pi::base / pi::plugin / ...）
#   conan 包：find_package(piplugin REQUIRED) —— CMakeDeps 按**包名**生成配置，
#             conan 包里没有 lib/cmake/pi/，家族入口在那条路线上不存在
# 默认 auto：先试 pi，不中再退 piplugin。verify_package.ps1 在 A/C 两段显式钉 pi、
# B 段显式钉 piplugin —— 每条路线都断言该走的那条入口，而不是"谁先命中算谁"。
#
# 默认链接：根层 + 核心 + 宿主 kit L0 + 事件路由 + imgui 适配器套件。
# -DPI_PLUGIN_CONSUMER_LINK_IMGUI=OFF 时跳过 imgui 套件。
cmake_minimum_required(VERSION 3.23)
project(piplugin_conan_consumer C)

option(PI_PLUGIN_CONSUMER_LINK_IMGUI "Link the imgui adapter kit as well" ON)
set(PI_PLUGIN_CONSUMER_ENTRY "auto" CACHE STRING "family entry to use: auto | pi | piplugin")

# 家族入口点名要求的组件：都是必到的（根层 + 核心 + 两个宿主 kit）。
# imgui / qt 套件不在这里点名 —— 它们按"能找到就顺势导入"的规则来，见文件末尾。
set(_pi_consumer_family_components base plugin plugin_host plugin_events)

if(PI_PLUGIN_CONSUMER_ENTRY STREQUAL "pi")
    find_package(pi REQUIRED COMPONENTS ${_pi_consumer_family_components})
    set(_pi_consumer_entry "pi")
elseif(PI_PLUGIN_CONSUMER_ENTRY STREQUAL "piplugin")
    find_package(piplugin REQUIRED)
    set(_pi_consumer_entry "piplugin")
elseif(PI_PLUGIN_CONSUMER_ENTRY STREQUAL "auto")
    find_package(pi QUIET COMPONENTS ${_pi_consumer_family_components})
    if(pi_FOUND)
        set(_pi_consumer_entry "pi")
    else()
        find_package(piplugin REQUIRED)
        set(_pi_consumer_entry "piplugin")
    endif()
else()
    message(FATAL_ERROR
        "PI_PLUGIN_CONSUMER_ENTRY must be auto, pi or piplugin, got '${PI_PLUGIN_CONSUMER_ENTRY}'")
endif()
message(STATUS "consumer: entry=${_pi_consumer_entry} version=${${_pi_consumer_entry}_VERSION}")

add_executable(pi_plugin_consumer main.c)

# 家族根层显式链一次：核心的链接接口里有它（pi::plugin 的
# INTERFACE_LINK_LIBRARIES 就是 pi::base），点名它等于断言根层的 config 真被导入过，
# 而不是靠传递侥幸成立。
if(TARGET pi::base)
    target_link_libraries(pi_plugin_consumer PRIVATE pi::base)
else()
    message(WARNING "consumer: pi::base missing from the package")
endif()

target_link_libraries(pi_plugin_consumer PRIVATE pi::plugin)
if(TARGET pi::plugin_host)
    target_link_libraries(pi_plugin_consumer PRIVATE pi::plugin_host)
else()
    message(WARNING "consumer: pi::plugin_host missing from the package")
endif()
if(TARGET pi::plugin_events)
    target_link_libraries(pi_plugin_consumer PRIVATE pi::plugin_events)
else()
    message(WARNING "consumer: pi::plugin_events missing from the package")
endif()

# ---------------------------------------------------------------------------
# imgui 适配器套件：两种分发形态都需要什么（都是实测结论）
#
#   裸 CMake 安装树（cmake --install）：导出 target 自带 imgui::imgui 与平台库，
#   消费方直接 link pi::plugin_imgui 即可。
#
#   conan 包（Conan 2.10.1 + CMakeDeps）：消费方只声明 piplugin 就够了 ——
#     1) 组件的外部依赖（imgui::imgui）随包传播：conanfile.py 用
#        transitive_libs=True 声明 imgui，CMakeDeps 因此生成 imgui-config.cmake，
#        并把 imgui::imgui 写进 pi_plugin_imgui 的 DEPENDENCIES。默认传播规则会
#        把它丢掉（链接期 29 个 imgui 未解析符号），所以这个 trait 不是可选的美化。
#     2) 套件链接的 Windows 平台库（user32/d3d11/dxgi/d3dcompiler）由包信息
#        cpp_info.system_libs 提供（已修：漏掉时报 unresolved external
#        D3D11CreateDeviceAndSwapChain）。
# ---------------------------------------------------------------------------
if(PI_PLUGIN_CONSUMER_LINK_IMGUI AND TARGET pi::plugin_imgui)
    target_link_libraries(pi_plugin_consumer PRIVATE pi::plugin_imgui)
    target_compile_definitions(pi_plugin_consumer PRIVATE PI_PLUGIN_CONSUMER_WITH_IMGUI)
elseif(PI_PLUGIN_CONSUMER_LINK_IMGUI)
    message(WARNING "consumer: pi::plugin_imgui missing from the package")
endif()
