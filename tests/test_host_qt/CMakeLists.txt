# ./tests/test_host_qt/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_qt)
pi_tar_msg(STATUS ${TARGET_NAME} "building qt-based test host...")

# source files
set(SRC_FILE_LIST
    pi_qt_test_host.cpp
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

set(CMAKE_AUTOMOC ON)

# dependency check（需要 Qt5 与 imgui adapter kit）
find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(WARNING "Qt5 not found -> ${TARGET_NAME} DISABLED
  ${PI_QT_MISSING_HINT}")
    return()
endif()
if(NOT TARGET piplugin_imgui)
    message(STATUS "ImGui adapter kit (piplugin_imgui) not built -> ${TARGET_NAME} DISABLED")
    return()
endif()
# 宿主 kit 缺失则禁用本宿主（L0 会话 + L1 Qt 嵌入区域）
if(NOT TARGET piplugin_host)
    message(WARNING "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} DISABLED")
    message(WARNING "  enable -o PI_BUILD_HOST_KITS=True -o PI_BUILD_HOST_KIT_CORE=True")
    return()
endif()
if(NOT TARGET piplugin_host_qt)
    message(WARNING "host kit L1 qt (piplugin_host_qt) not built -> ${TARGET_NAME} DISABLED")
    message(WARNING "  enable -o PI_BUILD_HOST_KITS=True -o PI_BUILD_HOST_KIT_QT=True")
    return()
endif()

# add executable target
add_executable(${TARGET_NAME})
set_target_properties(${TARGET_NAME} PROPERTIES
    WIN32_EXECUTABLE ON
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
# 宿主侧 kit：本宿主直接用两层的 API（L0 会话 + L1 Qt 嵌入区域），故两者都显式链接
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_host)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_host_qt)

# third party
target_link_libraries(${TARGET_NAME} PRIVATE Qt5::Widgets)

# 部署 Qt 运行时 DLL 到可执行文件目录（构建树；开发目录由下方的 POST_BUILD 一并部署）
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
    COMMENT "Deploying Qt runtime DLLs (Qt host)"
)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段） -------------
# Qt 运行时 DLL 与平台插件也在这里一并部署，使该目录自洽、可直接双击运行。
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