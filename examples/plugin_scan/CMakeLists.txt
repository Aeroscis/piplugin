# ./examples/plugin_scan/CMakeLists.txt
#
# 插件发现试水（FUT-07 的第一步）：扫目录 + 读 descriptor + 立刻卸载，打印清单。
# 依赖：框架核心 + 宿主 kit L0（加载/门禁/卸载序列）。无 GUI、无第三方、不含 Qt。
# 不改核心 ABI，也不碰 tests/。
cmake_minimum_required(VERSION 3.23)

# 目录枚举目前用 FindFirstFileA，故仅 Windows（其余平台需要一个分支；
# 扫描逻辑本身与平台无关，也不依赖任何 GUI/ABI）
if(NOT WIN32)
    message(STATUS "the plugin scanner example is Windows-only for now -> skipped")
    return()
endif()

set(TARGET_NAME pi_plugin_example_plugin_scan)
pi_tar_msg(STATUS ${TARGET_NAME} "building example: plugin scanner (discovery, FUT-07)...")

if(NOT TARGET piplugin_host)
    message(STATUS "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} skipped")
    return()
endif()

add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME} PRIVATE pi_plugin_scan.c)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin piplugin_host)

# 扫描对象是 bin/<CONFIG> 里的插件，所以自己也部署到那里（与其它例子一致）
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)
