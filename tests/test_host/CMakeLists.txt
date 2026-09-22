# ./tests/test_host/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_imgui)
pi_tar_msg(STATUS ${TARGET_NAME} "building imgui-based test host...")

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
set(FILE_DESCRIPTION "imgui-based test host for pi plugin framework")
set(LEGAL_COPY_RIGHT "Copyright (c) 2025 pipluginframework")
set(PRODUCT_NAME "${TARGET_NAME}")
set(ORIGIN_FILE_NAME "${TARGET_NAME}.exe")#info for windows only

# source files（imgui 核心来自 Conan imgui::imgui；Win32 + DX11 backend 使用 adapters/imgui/backends 副本）
set(IMGUI_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../adapters/imgui/backends)
set(SRC_FILE_LIST
    pi_imgui_test_host.cpp
    ${IMGUI_BACKEND_DIR}/imgui_impl_win32.cpp
    ${IMGUI_BACKEND_DIR}/imgui_impl_dx11.cpp
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# dependency check
find_package(imgui CONFIG QUIET)
if(NOT imgui_FOUND OR NOT EXISTS ${IMGUI_BACKEND_DIR}/imgui_impl_win32.cpp)
    message(WARNING "imgui (Conan) or vendored backends missing -> ${TARGET_NAME} DISABLED")
    message(WARNING "  run: conan install . -o PI_BUILD_TEST_HOST=True --build=missing")
    return()
endif()

# add executable target
add_executable(${TARGET_NAME})
set_target_properties(${TARGET_NAME} PROPERTIES
    WIN32_EXECUTABLE ON
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# self include（adapters/imgui/backends 头目录）
target_include_directories(${TARGET_NAME} PRIVATE ${IMGUI_BACKEND_DIR})

# first party
target_link_libraries(${TARGET_NAME} PRIVATE pipluginframework)

# third party
target_link_libraries(${TARGET_NAME} PRIVATE imgui::imgui)
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE d3d11 dxgi d3dcompiler)
endif()

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段；详见 src/pipluginframework/CMakeLists.txt 的说明） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)