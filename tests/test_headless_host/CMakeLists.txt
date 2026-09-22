# ./tests/test_headless_host/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_headless)
pi_tar_msg(STATUS ${TARGET_NAME} "building headless (console) test host...")

# source files（纯 C，链接框架核心 + 宿主 kit L0）
set(SRC_FILE_LIST
    pi_headless_host.c
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# add executable target
add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# 宿主 kit L0（加载/门禁/实例化/七步卸载序列）缺失则禁用本宿主
if(NOT TARGET piplugin_host)
    message(WARNING "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} DISABLED")
    message(WARNING "  enable -o PI_BUILD_HOST_KITS=True -o PI_BUILD_HOST_KIT_CORE=True")
    return()
endif()

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
# 宿主侧 kit L0（宿主 kit；STATIC）
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_host)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- ctest 注册：headless 冒烟测试（严格按退出码判定） -------------
# 用部署目录里的插件文件名（与可执行文件同目录，Qt 运行时 DLL 也在那里），
# 所以工作目录必须是部署目录。插件关掉时该用例不注册。
if(TARGET pi_test_plugin_qt)
    add_test(NAME headless_host_smoke
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_qt.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(headless_host_smoke PROPERTIES TIMEOUT 120)
endif()

# BLK-03 负向用例：声明不兼容 api_version 的插件必须在 create_instance 之前被拒绝。
# 这里**故意**使用 PASS_REGULAR_EXPRESSION —— 设了它 ctest 就不再检查退出码，
# 而本用例的宿主本来就以非 0 退出（"拒绝"正是期望结果），所以要断言的是
# "拒绝理由出现在宿主输出里"。上半句由 unit 用例负责验证纯函数边界。
if(TARGET pi_test_plugin_badversion)
    add_test(NAME version_gate_rejects_incompatible_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_badversion.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(version_gate_rejects_incompatible_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "incompatible with host"
        TIMEOUT 120)
endif()