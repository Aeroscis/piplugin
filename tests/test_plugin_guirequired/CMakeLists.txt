# ./tests/test_plugin_guirequired/CMakeLists.txt
#
# ECO-08 的负向测试件：一个**完全合法**但声明 `PI_IID_HOST_UI (REQUIRED)` 的插件。
# 用来断言 headless 宿主在 create_instance 之前就以"缺少必需能力"为由拒绝它。
cmake_minimum_required(VERSION 3.23)

set(TARGET_NAME pi_test_plugin_guirequired)
pi_tar_msg(STATUS ${TARGET_NAME} "building GUI-required test plugin (ECO-08 negative case)...")

set(SRC_FILE_LIST
    pi_guirequired_plugin.c
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# 插件 dll 目前仅 windows 构建（与其它插件一致）
if(NOT WIN32)
    message(STATUS "plugin dll build currently windows-only -> ${TARGET_NAME} DISABLED")
    return()
endif()

add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)

add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)
