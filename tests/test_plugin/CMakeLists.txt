# ./tests/test_plugin/CMakeLists.txt
#
# Qt 测试插件的两个变体：A = pi_plugin_test_plugin_qt.dll（默认），B = pi_plugin_test_plugin_qt2.dll。
# 同一份源码，只差 class GUID / 显示名 / 心跳消息码（见 pi_qt_test_plugin.cpp）。
#
# 为什么要两个：多插件同进程的验收（roadmap APP-08，tests/test_host_multi）需要进程里
# 同时存在两个**不同的** Qt 插件模块 —— 同一个 DLL 加载两次共享同一份静态数据，
# "套件状态是每模块一份还是进程一份"这个问题根本测不出来。
cmake_minimum_required(VERSION 3.23)

set(CMAKE_AUTOMOC ON)

# dependency check（需要 Qt5 与 qt adapter kit；插件 dll 目前仅 windows 构建）
find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(WARNING "Qt5 not found -> Qt test plugins DISABLED
  ${PI_PLUGIN_QT_MISSING_HINT}")
    return()
endif()
if(NOT TARGET piplugin_qt)
    message(STATUS "Qt adapter kit (piplugin_qt) not built -> Qt test plugins DISABLED")
    return()
endif()
if(NOT WIN32)
    message(STATUS "plugin dll build currently windows-only -> Qt test plugins DISABLED")
    return()
endif()

# Qt 运行时 DLL 目录（两个变体共用同一份 Qt 安装）
get_target_property(QT_CORE_DLL Qt5::Core LOCATION)
get_filename_component(QT_BIN_DIR "${QT_CORE_DLL}" DIRECTORY)

foreach(PI_PLUGIN_QT_VARIANT IN ITEMS 1 2)
    if(PI_PLUGIN_QT_VARIANT STREQUAL "1")
        set(TARGET_NAME pi_plugin_test_plugin_qt)
    else()
        set(TARGET_NAME pi_plugin_test_plugin_qt2)
    endif()
    pi_tar_msg(STATUS ${TARGET_NAME} "building qt-based test plugin (variant ${PI_PLUGIN_QT_VARIANT})...")

    # add plugin target
    add_library(${TARGET_NAME} SHARED)
    target_sources(${TARGET_NAME}
        PRIVATE
            pi_qt_test_plugin.cpp
            pi_qt_test_plugin.h
            pi_qt_main.cpp
        )

    # self header + 测试共用头（app 自定义宿主服务：插件侧 QI 它）
    target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(${TARGET_NAME} PRIVATE ${PI_PLUGIN_TESTS_COMMON_DIR})

    # first party
    target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
    # Qt 套件：SHARED（APP-08）—— 所有 Qt 插件共用进程里同一份套件状态，
    # 因此插件运行时必须能找到 piplugin_qt*.dll（下面的 POST_BUILD 会部署它）
    target_link_libraries(${TARGET_NAME} PRIVATE piplugin_qt)

    # third party
    target_link_libraries(${TARGET_NAME} PRIVATE Qt5::Widgets)

    if(PI_PLUGIN_QT_VARIANT STREQUAL "2")
        # 变体 B：不同 class GUID / 显示名 / 心跳消息码
        target_compile_definitions(${TARGET_NAME} PRIVATE PI_PLUGIN_TEST_QT_VARIANT_B)
    endif()

    # 部署 Qt 运行时 DLL（构建树；开发目录由下方的 POST_BUILD 一并部署，与 test hosts 同目录）
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
        COMMENT "Deploying Qt runtime DLLs (variant ${PI_PLUGIN_QT_VARIANT})"
    )

    # ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段） -------------
    # Qt 运行时 DLL、平台插件与 Qt 适配器套件 DLL 也在这里一并部署，
    # 使该目录自洽、可直接被宿主加载。
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
endforeach()
