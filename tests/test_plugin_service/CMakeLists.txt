# ./tests/test_plugin_service/CMakeLists.txt
#
# roadmap APP-07 的落地示例：一个纯 C 的"服务插件"（无 UI，实现 IPiPluginService），
# 由 headless 测试宿主驱动并做全生命周期断言（见 tests/test_headless_host）。
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_plugin_test_plugin_service)
pi_tar_msg(STATUS ${TARGET_NAME} "building service (headless) test plugin...")

# source files（纯 C，无第三方依赖；协议常量与宿主共用 tests/common）
set(SRC_FILE_LIST
    pi_service_test_plugin.c
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# 插件 dll 目前仅 windows 构建（与其它插件一致）
if(NOT WIN32)
    message(STATUS "plugin dll build currently windows-only -> ${TARGET_NAME} DISABLED")
    return()
endif()

# add plugin target
add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )

# 测试共用头：服务插件与宿主约定的消息码 / 选项键
target_include_directories(${TARGET_NAME} PRIVATE ${PI_PLUGIN_TESTS_COMMON_DIR})

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)
