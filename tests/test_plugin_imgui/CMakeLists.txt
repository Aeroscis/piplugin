# ./tests/test_plugin_imgui/CMakeLists.txt
#
# imgui 测试插件的两个变体：A = pi_test_plugin_imgui.dll（默认），
# B = pi_test_plugin_imgui2.dll。同一份源码，只差 class GUID / 显示名 /
# 心跳消息码（见 pi_imgui_test_plugin.cpp）。
#
# 为什么要两个（W-05）：imgui 套件是 STATIC 库，每个插件模块各带一份套件与
# imgui 状态，而"第二个模块的窗口类注册 / 各自的 ImGui context 与 D3D 设备"
# 这类进程级问题，只有加载两个**不同的模块**才测得出来 —— 同一个 DLL 加载两次
# 共享静态数据，什么都看不见。回归用例：ctest `multi_plugin_imgui_in_one_process`。
cmake_minimum_required(VERSION 3.23)

# dependency check（需要 imgui 与 imgui adapter kit）
find_package(imgui CONFIG QUIET)
if(NOT imgui_FOUND)
    message(WARNING "imgui not found (Conan) -> imgui test plugins DISABLED")
    return()
endif()
if(NOT TARGET piplugin_imgui)
    message(STATUS "ImGui adapter kit (piplugin_imgui) not built -> imgui test plugins DISABLED")
    return()
endif()

foreach(PI_IMGUI_VARIANT IN ITEMS 1 2)
    if(PI_IMGUI_VARIANT STREQUAL "1")
        set(TARGET_NAME pi_test_plugin_imgui)
    else()
        set(TARGET_NAME pi_test_plugin_imgui2)
    endif()
    pi_tar_msg(STATUS ${TARGET_NAME} "building imgui-based test plugin (variant ${PI_IMGUI_VARIANT})...")

    # source files
    set(SRC_FILE_LIST
        pi_imgui_test_plugin.cpp
        pi_imgui_test_plugin.h
        pi_imgui_main.cpp
        )

    # add plugin target
    add_library(${TARGET_NAME} SHARED)
    target_sources(${TARGET_NAME}
        PRIVATE
            ${SRC_FILE_LIST}
        )

    # self header + 测试共用头（app 自定义宿主服务：插件侧 QI 它）
    target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(${TARGET_NAME} PRIVATE ${PI_TESTS_COMMON_DIR})

    # first party
    target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
    target_link_libraries(${TARGET_NAME} PRIVATE piplugin_imgui)

    if(PI_IMGUI_VARIANT STREQUAL "2")
        # 变体 B：不同 class GUID / 显示名 / 心跳消息码
        target_compile_definitions(${TARGET_NAME} PRIVATE PI_TEST_IMGUI_VARIANT_B)
    endif()

    # ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段） -------------
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${TARGET_NAME}>"
            "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    )
endforeach()
