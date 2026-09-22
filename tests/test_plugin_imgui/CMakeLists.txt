# ./tests/test_plugin_imgui/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_plugin_imgui)
pi_tar_msg(STATUS ${TARGET_NAME} "building imgui-based test plugin...")

# source files
set(SRC_FILE_LIST
    pi_imgui_test_plugin.cpp
    pi_imgui_test_plugin.h
    pi_imgui_main.cpp
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# dependency check（需要 imgui 与 imgui adapter kit）
find_package(imgui CONFIG QUIET)
if(NOT imgui_FOUND)
    message(WARNING "imgui not found (Conan) -> ${TARGET_NAME} DISABLED")
    return()
endif()
if(NOT TARGET piplugin_imgui)
    message(STATUS "ImGui adapter kit (piplugin_imgui) not built -> ${TARGET_NAME} DISABLED")
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
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_imgui)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)