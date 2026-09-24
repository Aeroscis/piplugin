# ./tests/test_plugin_badversion/CMakeLists.txt
#
# BLK-03 的负向测试件：一个**除了 api_version 之外完全合法**的插件，
# 用来断言宿主在 create_instance 之前就以版本不兼容为由拒绝它。
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_plugin_test_plugin_badversion)
pi_tar_msg(STATUS ${TARGET_NAME} "building bad-version test plugin...")

# source files（纯 C，无第三方依赖）
set(SRC_FILE_LIST
    pi_badversion_plugin.c
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
