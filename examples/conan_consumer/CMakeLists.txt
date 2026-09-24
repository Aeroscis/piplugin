# ./examples/conan_consumer/CMakeLists.txt
#
# roadmap ECO-04 的消费方测试件：它**不** add_subdirectory 本仓库的任何东西，
# 只做每个外部消费者都会做的事 —— find_package(piplugin) 然后链接官方 target。
# 由 scripts/verify_package.ps1 在"安装树"和"conan 包"两种形态下各跑一遍。
#
# 默认链接：核心 + 宿主 kit L0 + 事件路由 + imgui 适配器套件（四种都验证一遍）。
# -DPI_PLUGIN_CONSUMER_LINK_IMGUI=OFF 时跳过 imgui 套件。
cmake_minimum_required(VERSION 3.23)
project(piplugin_conan_consumer C)

option(PI_PLUGIN_CONSUMER_LINK_IMGUI "Link the imgui adapter kit as well" ON)

find_package(piplugin REQUIRED)
message(STATUS "consumer: piplugin_VERSION=${piplugin_VERSION}")

add_executable(pi_plugin_consumer main.c)

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
