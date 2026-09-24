# ./examples/specialized_app/CMakeLists.txt
#
# 特化 app 示例：app 自定义协议（通道 A）+ 宿主自定义服务（通道 B）+ 能力门禁。
# 三个目标：协议头（共享）、插件（实现协议）、app（消费协议）。
cmake_minimum_required(VERSION 3.23)

if(NOT TARGET piplugin_host)
    message(STATUS "host kit L0 (piplugin_host) not built -> specialized_app skipped")
    return()
endif()

# --- the plugin half (implements the app protocol) ---
set(PLUGIN_TARGET pi_plugin_example_specialized_plugin)
pi_tar_msg(STATUS ${PLUGIN_TARGET} "building example: specialized plugin...")
add_library(${PLUGIN_TARGET} SHARED)
target_sources(${PLUGIN_TARGET} PRIVATE pi_specialized_plugin.c)
target_include_directories(${PLUGIN_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${PLUGIN_TARGET} PRIVATE piplugin)
add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PLUGIN_TARGET}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${PLUGIN_TARGET} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# --- the app half (defines the protocol and gates on it) ---
set(APP_TARGET pi_plugin_example_specialized_app)
pi_tar_msg(STATUS ${APP_TARGET} "building example: specialized app...")
add_executable(${APP_TARGET})
target_sources(${APP_TARGET} PRIVATE pi_specialized_app.c)
target_include_directories(${APP_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${APP_TARGET} PRIVATE piplugin piplugin_host)
add_custom_command(TARGET ${APP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${APP_TARGET}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${APP_TARGET} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)
# ------- ctest 注册：特化 app 示例（无 GUI，纯逻辑，CI 里也跑） -------------
# app 要求自定义协议 -> 合规插件被接受且协议被调用 -> 不合规插件被门禁挡在实例化之前。
# 两个插件都来自 examples/，因此这个用例不依赖 tests/ 里的任何东西。
if(TARGET pi_plugin_example_specialized_plugin AND TARGET pi_plugin_example_service)
    add_test(NAME example_specialized_app
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/pi_plugin_example_specialized_app.exe"
                "pi_plugin_example_specialized_plugin.dll" "pi_plugin_example_service.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(example_specialized_app PROPERTIES TIMEOUT 120)
endif()
