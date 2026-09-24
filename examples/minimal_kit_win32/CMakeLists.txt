# ./examples/minimal_kit_win32/CMakeLists.txt
#
# roadmap ECO-01 的可执行附录：一个**没有工具包**的适配器套件（纯 C + Win32 GDI），
# 按 docs/design/adapter-spec.md 写出来，用官方宿主与一致性验收跑通。
# 套件是 STATIC（与官方套件同一形态：源码随插件编译），插件把它链进去。
cmake_minimum_required(VERSION 3.23)

if(NOT WIN32)
    message(STATUS "the Win32 example kit is Windows-only -> skipped")
    return()
endif()

# --- the kit itself (a static library, like the official adapter kits) ---
set(KIT_TARGET pi_example_kit_win32)
pi_tar_msg(STATUS ${KIT_TARGET} "building example: minimal Win32 view kit...")
add_library(${KIT_TARGET} STATIC pi_win32_view.c pi_win32_view.h)
target_include_directories(${KIT_TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${KIT_TARGET} PUBLIC piplugin)
target_link_libraries(${KIT_TARGET} PRIVATE user32 gdi32)

# --- a plugin that uses it (the whole plugin is a paint callback) ---
set(PLUGIN_TARGET pi_plugin_example_plugin_win32)
pi_tar_msg(STATUS ${PLUGIN_TARGET} "building example: Win32 plugin (uses the kit)...")
add_library(${PLUGIN_TARGET} SHARED pi_example_win32_plugin.c)
target_link_libraries(${PLUGIN_TARGET} PRIVATE piplugin ${KIT_TARGET})
add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PLUGIN_TARGET}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${PLUGIN_TARGET} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- 一致性验收（ECO-01 的验收） -------------
# 官方 harness 由 scripts/verify.ps1 统一驱动（它按 bin/<CONFIG> 里存在的插件自动发现），
# 本套件对应的插件名已加入那份列表：于是"按 spec 写出的最小套件可用"由同一条命令断言，
# 不必在这里再起一个 ctest 用例（也就不会把 pwsh 依赖塞进 ctest）。
