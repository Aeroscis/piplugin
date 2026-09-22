# ./tests/test_plugin/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_plugin_qt)
pi_tar_msg(STATUS ${TARGET_NAME} "building qt-based test plugin...")

# source files
set(SRC_FILE_LIST
    pi_qt_test_plugin.cpp
    pi_qt_test_plugin.h
    pi_qt_main.cpp
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

set(CMAKE_AUTOMOC ON)

# dependency check（需要 Qt5 与 qt adapter kit；插件 dll 目前仅 windows 构建）
find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(WARNING "Qt5 not found -> ${TARGET_NAME} DISABLED")
    return()
endif()
if(NOT TARGET piplugin_qt)
    message(STATUS "Qt adapter kit (piplugin_qt) not built -> ${TARGET_NAME} DISABLED")
    return()
endif()
if(NOT WIN32)
    message(STATUS "plugin dll build currently windows-only -> ${TARGET_NAME} DISABLED")
    return()
endif()

# add plugin target
add_library(${TARGET_NAME} SHARED)
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# self header + 测试共用头（app 自定义宿主服务：插件侧 QI 它）
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET_NAME} PRIVATE ${PI_TESTS_COMMON_DIR})

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_qt)

# third party
target_link_libraries(${TARGET_NAME} PRIVATE Qt5::Widgets)

# 部署 Qt 运行时 DLL（构建树；开发目录由下方的 POST_BUILD 一并部署，与 test hosts 同目录）
get_target_property(QT_CORE_DLL Qt5::Core LOCATION)
get_filename_component(QT_BIN_DIR "${QT_CORE_DLL}" DIRECTORY)
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TARGET_NAME}>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_BIN_DIR}/Qt5Core.dll"
        "${QT_BIN_DIR}/Qt5Gui.dll"
        "${QT_BIN_DIR}/Qt5Widgets.dll"
        "$<TARGET_FILE_DIR:${TARGET_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_BIN_DIR}/../plugins/platforms/qwindows.dll"
        "$<TARGET_FILE_DIR:${TARGET_NAME}>/platforms"
    COMMENT "Deploying Qt runtime DLLs"
)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段） -------------
# Qt 运行时 DLL 与平台插件也在这里一并部署，使该目录自洽、可直接被宿主加载。
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${QT_BIN_DIR}/Qt5Core.dll"
        "${QT_BIN_DIR}/Qt5Gui.dll"
        "${QT_BIN_DIR}/Qt5Widgets.dll"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_BIN_DIR}/../plugins/platforms/qwindows.dll"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/platforms"
    COMMENT "Deploy ${TARGET_NAME} + Qt runtime -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)