# ./adapters/imgui/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pipluginframework_imgui)
pi_tar_msg(STATUS ${TARGET_NAME} "building imgui adapter kit...")

# version infos
set(PRODUCT_VERSION_MAJOR 1)
set(PRODUCT_VERSION_MINOR 0)
set(PRODUCT_VERSION_PATCH 0)
set(PRODUCT_VERSION_TWEAK 0)
set(PRODUCT_VERSION_COMMA "${PRODUCT_VERSION_MAJOR},${PRODUCT_VERSION_MINOR},${PRODUCT_VERSION_PATCH},${PRODUCT_VERSION_TWEAK}")
set(PRODUCT_VERSION_STRING "${PRODUCT_VERSION_MAJOR}.${PRODUCT_VERSION_MINOR}.${PRODUCT_VERSION_PATCH}.${PRODUCT_VERSION_TWEAK}")

set(FILE_VERSION_MAJOR 1)
set(FILE_VERSION_MINOR 0)
set(FILE_VERSION_PATCH 0)
set(FILE_VERSION_TWEAK 0)
set(FILE_VERSION_COMMA "${FILE_VERSION_MAJOR},${FILE_VERSION_MINOR},${FILE_VERSION_PATCH},${FILE_VERSION_TWEAK}")
set(FILE_VERSION_STRING "${FILE_VERSION_MAJOR}.${FILE_VERSION_MINOR}.${FILE_VERSION_PATCH}.${FILE_VERSION_TWEAK}")

set(COMPANY_NAME "pipluginframework")
set(FILE_DESCRIPTION "dear imgui ui adapter kit for pi plugin framework")
set(LEGAL_COPY_RIGHT "Copyright (c) 2025 pipluginframework")
set(PRODUCT_NAME "${TARGET_NAME}")
set(ORIGIN_FILE_NAME "${TARGET_NAME}.dll")#info for windows only

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
target_compile_definitions(${TARGET_NAME} PRIVATE "LIBRARY_VERSION=\"${FILE_VERSION_STRING}\"")

# self header
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/backends)

# first party: framework core
target_link_libraries(${TARGET_NAME} PUBLIC pipluginframework)
target_link_libraries(${TARGET_NAME} PUBLIC imgui::imgui)

# third party: windows 平台系统库
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE user32)
endif()
target_link_libraries(${TARGET_NAME} PRIVATE d3d11 dxgi d3dcompiler)