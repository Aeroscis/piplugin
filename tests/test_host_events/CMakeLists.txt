# ./tests/test_host_events/CMakeLists.txt
#
# roadmap APP-06 的验收宿主：用可选的 piplugin_events 路由器把事件接口跑通，
# 逐条断言双向闭环、pump 重入规则、owner 退订（D9）与"无 sink 插件优雅降级"。
# console 宿主（证据打在 stdout），无 GUI，故 CI（Qt 关着）里也照跑。
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_events)
pi_tar_msg(STATUS ${TARGET_NAME} "building events test host...")

# source files（纯 C）
set(SRC_FILE_LIST
    pi_events_host.c
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# 依赖宿主 kit：L0 会话（加载/门禁/卸载序列 + 事件 sink 记账）与事件路由器
if(NOT TARGET piplugin_host)
    message(WARNING "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} DISABLED")
    return()
endif()
if(NOT TARGET piplugin_events)
    message(WARNING "host-side event router (piplugin_events) not built -> ${TARGET_NAME} DISABLED")
    return()
endif()

# add executable target
add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )

# 测试共用头：事件 topic / payload key 的唯一来源
target_include_directories(${TARGET_NAME} PRIVATE ${PI_TESTS_COMMON_DIR})

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_host)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_events)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- ctest 注册：双向事件闭环（APP-06 的验收） -------------
# 需要事件插件 + 一个**没有 sink** 的插件（用来断言优雅降级）；两者缺任一就不注册。
# 宿主内部逐条断言并以退出码收尾，故只用退出码判定（比正则更严格）。
if(TARGET pi_test_plugin_events AND TARGET pi_test_plugin_imgui)
    add_test(NAME events_two_way_loop
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
                "pi_test_plugin_events.dll" "pi_test_plugin_imgui.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(events_two_way_loop PROPERTIES TIMEOUT 120)
endif()
