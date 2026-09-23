# ./examples/minimal_plugin_qt/CMakeLists.txt
#
# 最小 Qt 插件：只写 widget 工厂，QApplication/嵌入/驱动全在套件里。
# Qt 是本地安装（ECO-05：用 -DPI_QT_PREFIX / -DQt5_DIR / CMAKE_PREFIX_PATH 指定），
# 找不到时本目标自动跳过，不影响其余例子。
cmake_minimum_required(VERSION 3.23)

set(CMAKE_AUTOMOC ON)

set(TARGET_NAME pi_example_plugin_qt)
pi_tar_msg(STATUS ${TARGET_NAME} "building example: minimal Qt plugin...")

find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(STATUS "Qt5 not found -> ${TARGET_NAME} skipped
  ${PI_QT_MISSING_HINT}")
    return()
endif()
if(NOT TARGET piplugin_qt)
    message(STATUS "Qt adapter kit (piplugin_qt) not built -> ${TARGET_NAME} skipped")
    return()
endif()
if(NOT WIN32)
    message(STATUS "plugin dll build currently windows-only -> ${TARGET_NAME} skipped")
    return()
endif()

add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME} PRIVATE pi_example_qt_plugin.cpp)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin piplugin_qt Qt5::Widgets)

# 部署到 bin/<CONFIG>：插件运行时需要同目录的 piplugin_qt*.dll 与 Qt 运行时 DLL
# （套件与 Qt 运行时由它们各自的 target 负责部署，这里只放本插件）。
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)
