# ./examples/minimal_host/CMakeLists.txt
#
# 最小宿主：一个窗口 + 一个容器 + 一个插件（view 与 service 都能驱）。
# 依赖：框架核心 + 宿主 kit L0（加载/门禁/实例化/七步卸载序列）。
cmake_minimum_required(VERSION 3.23)

set(TARGET_NAME pi_plugin_example_minimal_host)
pi_tar_msg(STATUS ${TARGET_NAME} "building example: minimal host...")

if(NOT TARGET piplugin_host)
    message(STATUS "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} skipped")
    return()
endif()

add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME} PRIVATE pi_minimal_host.c)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin piplugin_host)
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE user32)
endif()

# 例子跑起来需要与 piplugin*.dll / 套件 DLL 同目录，故与测试件一样部署到 bin/<CONFIG>
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)
# ------- ctest 注册：例子也是可自动跑的（服务插件，无第三方依赖） -------------
# 宿主会开一个窗口（Windows 上 ctest 跑在桌面会话里），加载例子服务插件、驱动 1.5s、
# 干净卸载；退出码 0 = 这一课跑通。GUI 例子（imgui/Qt）留给 README 的人工三步。
if(TARGET pi_plugin_example_service)
    add_test(NAME example_minimal_host_service
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/pi_plugin_example_minimal_host.exe"
                "pi_plugin_example_service.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(example_minimal_host_service PROPERTIES TIMEOUT 120)
endif()
