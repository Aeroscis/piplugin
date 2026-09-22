# ./tests/unit_cpp/CMakeLists.txt
#
# C++ RAII 层（include/piplugin/pi_cpp.h）的测试：行为 + 泄漏。
# 与 tests/unit 的分工：那边是纯 C 的核心回归（C ABI），这边只测 C++ 糖，
# 并在 Debug CRT 下用 _CrtDumpMemoryLeaks() 让"没有泄漏"成为可断言的退出码。
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_unit_cpp_tests)
pi_tar_msg(STATUS ${TARGET_NAME} "building C++ RAII layer tests...")

# source files（C++17，仅依赖框架核心）
set(SRC_FILE_LIST
    pi_cpp_tests.cpp
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

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪） -------------
# 核心是 SHARED，可执行文件必须与 piplugin*.dll 同目录才能启动；
# ctest 运行的是部署后的副本（见下面的 add_test）。
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- ctest 注册 -------------
# 只按退出码判定（0 = 全过，其中已包含 Debug CRT 的泄漏判定）。
# 有 imgui 测试插件时把它的路径传进去，让 PiUniqueModule 对着真实模块跑一遍
# 完整的加载 -> 实例化 -> 卸载链；CI 上 Qt 是关的，imgui 插件则在。
if(TARGET pi_test_plugin_imgui)
    add_test(NAME unit_cpp
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_imgui.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
else()
    add_test(NAME unit_cpp
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
endif()
set_tests_properties(unit_cpp PROPERTIES TIMEOUT 120)
