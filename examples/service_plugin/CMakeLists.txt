# ./examples/service_plugin/CMakeLists.txt
#
# 服务插件示例：纯 C，无 UI，实现 IPiService（start/poll/status/stop）。
cmake_minimum_required(VERSION 3.23)

set(TARGET_NAME pi_example_service)
pi_tar_msg(STATUS ${TARGET_NAME} "building example: service plugin...")

if(NOT WIN32)
    message(STATUS "plugin dll build currently windows-only -> ${TARGET_NAME} skipped")
    return()
endif()

add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME} PRIVATE pi_example_service.c)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)

add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)