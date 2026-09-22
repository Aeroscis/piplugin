# ./tests/test_host_multi/CMakeLists.txt
#
# roadmap APP-08 的验收宿主：一个进程里**同时**加载两个不同的 Qt 插件 DLL，
# 各自嵌进自己的容器，由同一个 session 驱动，然后一起卸载。
# 刻意的极简 Win32 宿主（无 D3D、无 resize 循环）：脆弱的渲染机器在
# tests/test_host 里，这个宿主只负责"多插件同进程"这一件事。
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_multi)
pi_tar_msg(STATUS ${TARGET_NAME} "building multi-plugin test host...")

# source files（C++，仅依赖框架核心 + 宿主 kit L0）
set(SRC_FILE_LIST
    pi_multi_host.cpp
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# 宿主 kit L0（加载/门禁/多槽位/七步卸载序列）缺失则禁用本宿主
if(NOT TARGET piplugin_host)
    message(WARNING "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} DISABLED")
    message(WARNING "  enable -o PI_BUILD_HOST_KITS=True -o PI_BUILD_HOST_KIT_CORE=True")
    return()
endif()

# add executable target（console 子系统：证据打在 stdout 与 pi_multi_host.log 里）
add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_host)
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE ole32 user32)
endif()

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- ctest 注册：多插件同进程（APP-08 的验收） -------------
# 需要两个**不同的** Qt 插件模块都存在；缺任何一个就不注册（CI 上 Qt 是关的）。
# 宿主自己断言：两个插件都发布了视图、各自嵌进自己的容器且可见、两套 Qt 定时器
# 都在跑（心跳消息分别计数）、双双干净卸载。任一不符 -> 退出码 1。
if(TARGET pi_test_plugin_qt AND TARGET pi_test_plugin_qt2)
    add_test(NAME multi_plugin_qt_in_one_process
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
                "pi_test_plugin_qt.dll" "pi_test_plugin_qt2.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(multi_plugin_qt_in_one_process PROPERTIES TIMEOUT 180)
endif()
