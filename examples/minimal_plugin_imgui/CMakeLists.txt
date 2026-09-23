# ./examples/minimal_plugin_imgui/CMakeLists.txt
#
# 最小 imgui 插件：只写 draw 回调，嵌入/上下文/驱动全在套件里。
cmake_minimum_required(VERSION 3.23)

set(TARGET_NAME pi_example_plugin_imgui)
pi_tar_msg(STATUS ${TARGET_NAME} "building example: minimal imgui plugin...")

find_package(imgui CONFIG QUIET)
if(NOT imgui_FOUND)
    message(STATUS "imgui not found (Conan) -> ${TARGET_NAME} skipped")
    return()
endif()
if(NOT TARGET piplugin_imgui)
    message(STATUS "imgui adapter kit (piplugin_imgui) not built -> ${TARGET_NAME} skipped")
    return()
endif()

add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME} PRIVATE pi_example_imgui_plugin.cpp)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin piplugin_imgui imgui::imgui)

add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)