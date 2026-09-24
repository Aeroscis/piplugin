# ./examples/qt_host_direct/CMakeLists.txt
#
# Qt 宿主 + Qt 插件的**直连**示例：宿主自己持有 QApplication 与布局，插件把
# QWidget* 交给宿主收编，全程不经过 Qt 适配器套件（套件面向"非 Qt 宿主"，在
# 已有 QApplication 的进程里 attach 会失败）。
#
# 依赖：框架核心 + 宿主 kit L0（加载/门禁/七步卸载序列）+ Qt5 Widgets。
# Qt 是本地安装（ECO-05：-DPI_QT_PREFIX / -DQt5_DIR / CMAKE_PREFIX_PATH），
# 找不到 Qt 时本目录整体跳过，不影响其余例子。
#
# 注意本示例**不链接** piplugin_qt，也不含任何 Win32 代码：它是跨平台的
# （没有原生容器、没有嵌入），这与"走套件"的例子（minimal_plugin_qt）不同。
cmake_minimum_required(VERSION 3.23)

set(CMAKE_AUTOMOC ON)

set(PLUGIN_NAME pi_plugin_example_plugin_qt_direct)
set(HOST_NAME  pi_plugin_example_qt_direct_host)

pi_tar_msg(STATUS ${HOST_NAME} "building example: Qt host with a Qt plugin (direct)...")

find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(STATUS "Qt5 not found -> ${HOST_NAME} / ${PLUGIN_NAME} skipped
  ${PI_QT_MISSING_HINT}")
    return()
endif()
if(NOT TARGET piplugin_host)
    message(STATUS "host kit L0 (piplugin_host) not built -> ${HOST_NAME} skipped")
    return()
endif()

# ---------------- 插件：Qt 插件，交出一个 QWidget*（不链接任何套件） ----------------
add_library(${PLUGIN_NAME} SHARED)
target_sources(${PLUGIN_NAME} PRIVATE
    pi_qt_direct_plugin.cpp
    pi_qt_direct_protocol.h
    )
target_link_libraries(${PLUGIN_NAME} PRIVATE piplugin Qt5::Widgets)

add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PLUGIN_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${PLUGIN_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ---------------- 宿主：自己的 QApplication + 自己的 QLayout ----------------
add_executable(${HOST_NAME})
target_sources(${HOST_NAME} PRIVATE
    pi_qt_direct_host.cpp
    pi_qt_direct_protocol.h
    )
target_link_libraries(${HOST_NAME} PRIVATE piplugin piplugin_host Qt5::Widgets)

# 默认插件文件名（不给参数时用）：宿主是控制台程序，方便"三步跑通"看到输出
if(WIN32)
    set(PI_QT_DIRECT_DEFAULT_PLUGIN "${PLUGIN_NAME}.dll")
elseif(APPLE)
    set(PI_QT_DIRECT_DEFAULT_PLUGIN "lib${PLUGIN_NAME}.dylib")
else()
    set(PI_QT_DIRECT_DEFAULT_PLUGIN "lib${PLUGIN_NAME}.so")
endif()
target_compile_definitions(${HOST_NAME} PRIVATE
    PI_QT_DIRECT_DEFAULT_PLUGIN="${PI_QT_DIRECT_DEFAULT_PLUGIN}")

# ---------------- 部署到开发目录 bin/<CONFIG> ----------------
if(WIN32)
    # Qt 运行时 DLL + 平台插件（与 tests/test_host_qt 同一做法：该目录自洽、可双击运行）
    get_target_property(QT_CORE_DLL Qt5::Core LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_CORE_DLL}" DIRECTORY)
    add_custom_command(TARGET ${HOST_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${HOST_NAME}>"
            "${QT_BIN_DIR}/Qt5Core.dll"
            "${QT_BIN_DIR}/Qt5Gui.dll"
            "${QT_BIN_DIR}/Qt5Widgets.dll"
            "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_BIN_DIR}/../plugins/platforms/qwindows.dll"
            "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/platforms"
        COMMENT "Deploy ${HOST_NAME} + Qt runtime -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    )
else()
    add_custom_command(TARGET ${HOST_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${HOST_NAME}>"
            "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        COMMENT "Deploy ${HOST_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    )
endif()
