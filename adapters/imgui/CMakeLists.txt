# ./adapters/imgui/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# include GNU best practice
include(GNUInstallDirs)

# target name
set(TARGET_NAME piplugin_imgui)
pi_tar_msg(STATUS ${TARGET_NAME} "building imgui adapter kit...")

# source files（imgui 核心来自 Conan imgui::imgui；Win32 + DX11 backend 为本地 vendored 副本）
set(SRC_FILE_LIST
    pi_imgui_view.cpp
    pi_imgui_view.h
    backends/imgui_impl_win32.cpp
    backends/imgui_impl_win32.h
    backends/imgui_impl_dx11.cpp
    backends/imgui_impl_dx11.h
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# dependency check（imgui 来自 Conan CMakeDeps；找不到则禁用本 kit）
find_package(imgui CONFIG QUIET)
if(NOT imgui_FOUND)
    message(STATUS "imgui not found (Conan) -> ImGui adapter kit (${TARGET_NAME}) disabled")
    return()
endif()

# add library target
add_library(${TARGET_NAME} STATIC)
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# self header（BUILD_INTERFACE/INSTALL_INTERFACE 拆分：构建树平铺 include，安装树平铺到
# include/piplugin/adapters/imgui，两侧 #include "pi_imgui_view.h" 写法一致）
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/piplugin/adapters/imgui>
)
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/backends)

# first party: framework core
target_link_libraries(${TARGET_NAME} PUBLIC piplugin)
target_link_libraries(${TARGET_NAME} PUBLIC imgui::imgui)

# third party: windows 平台系统库
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE user32)
endif()
target_link_libraries(${TARGET_NAME} PRIVATE d3d11 dxgi d3dcompiler)

# ------- 安装（Modern CMake：静态库 + 公共头 + 导出目标；测试件无 install 规则，不进包） -------
# 导出集独立于核心（pipluginTargets）：本 kit 的 find_dependency(imgui)
# 由安装伞配置（piConfig.cmake.in）在 include 本导出文件前先行处理
install(TARGETS ${TARGET_NAME}
    EXPORT pipluginImguiAdapterTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>  # 静态库（linux .a / windows .lib）
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pi_imgui_view.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/piplugin/adapters/imgui
)

install(EXPORT pipluginImguiAdapterTargets
    FILE pipluginImguiAdapterTargets.cmake
    NAMESPACE ${NAMESPACE_NAME}:: # 消费方引用 ${NAMESPACE_NAME}::${TARGET_NAME}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)