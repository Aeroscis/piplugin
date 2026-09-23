# ./tests/unit/CMakeLists.txt
#
# 核心回归单测（roadmap BLK-06）：裸 C，无第三方测试框架，由 ctest 注册。
# 两个目标：
#   pi_unit_tests  -> ctest `unit`        功能回归（含 W-01 的加载错误串线程隔离）
#   pi_unit_threads-> ctest `unit_threads` 线程安全压测（W-04：跨线程 post + 并发引用计数）
cmake_minimum_required(VERSION 3.23)

# 线程（W-01/W-04 的并发用例）：Windows 上是空依赖，Linux/macOS 上给出
# pthread 链接选项 —— 这样测试文件里的 WIN32/pthread 双分支都能编。
find_package(Threads REQUIRED)

# target name
set(TARGET_NAME pi_unit_tests)
pi_tar_msg(STATUS ${TARGET_NAME} "building unit tests...")

# source files（纯 C，仅依赖框架核心）
set(SRC_FILE_LIST
    pi_unit_tests.c
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
target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)

# 测试共用头：单测复用测试宿主/插件用的那份 app 自定义宿主服务（APP-01）
target_include_directories(${TARGET_NAME} PRIVATE ${PI_TESTS_COMMON_DIR})

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
# 只按退出码判定（0 = 全过），刻意不加 PASS_REGULAR_EXPRESSION —— 设了它
# ctest 就不再检查退出码，反而更松。
#
# 有测试插件时把它的名字传给单测：工厂负向用例（ECO-08）需要一个真实模块
# （未知 class GUID / 非法参数 / 正向控制），没有就跳过那一段。
if(TARGET pi_test_plugin_imgui)
    add_test(NAME unit
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_imgui.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
else()
    add_test(NAME unit
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
endif()
set_tests_properties(unit PROPERTIES TIMEOUT 120)

# ------- W-04：线程安全压测（单独目标 + 单独用例） -------------
# 三个跨线程场景里不需要 UI 工具包的两个：插件子线程 pi_host_post_message、
# 并发 AddRef/Release。单独一个用例是因为它会真的起线程互相踩 —— 反复跑
# （`ctest -R unit_threads --repeat until-fail:20`）比单次通过更有意义。
set(THREADS_TARGET_NAME pi_unit_threads)
pi_tar_msg(STATUS ${THREADS_TARGET_NAME} "building thread-safety stress tests...")

add_executable(${THREADS_TARGET_NAME} pi_thread_tests.c)
target_link_libraries(${THREADS_TARGET_NAME} PRIVATE piplugin)
target_link_libraries(${THREADS_TARGET_NAME} PRIVATE Threads::Threads)
target_include_directories(${THREADS_TARGET_NAME} PRIVATE ${PI_TESTS_COMMON_DIR})

add_custom_command(TARGET ${THREADS_TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${THREADS_TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${THREADS_TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# 同样只按退出码判定（不设 PASS_REGULAR_EXPRESSION）。
add_test(NAME unit_threads
    COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${THREADS_TARGET_NAME}.exe"
    WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    )
set_tests_properties(unit_threads PROPERTIES TIMEOUT 300)
