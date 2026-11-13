# ./tests/test_plugin_multiview/CMakeLists.txt
#
# 0.6 视图能力拆分的验收件：**一个插件、两个视图**（id "alpha" / "beta"）。
# 纯 C、无 GUI 工具包 —— 它验证的是框架与宿主 kit 的账本（按 id 枚举 / 分别取用 /
# 分别 attach / 分别 pump / 分别 detach），不是渲染。
# 回归用例：ctest multi_view_one_plugin_two_containers（tests/test_host_multi --multi-view）。
cmake_minimum_required(VERSION 3.23)

set(TARGET_NAME pi_plugin_test_plugin_multiview)
pi_tar_msg(STATUS ${TARGET_NAME} "building multi-view test plugin (0.6 view capability)...")

set(SRC_FILE_LIST
    pi_multiview_test_plugin.c
    )

# 插件 dll 目前仅 windows 构建（与其它插件一致；宿主用例用的是 Win32 容器窗口）
if(NOT WIN32)
    message(STATUS "plugin dll build currently windows-only -> ${TARGET_NAME} DISABLED")
    return()
endif()

add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME}
    PRIVATE
        ${SRC_FILE_LIST}
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
