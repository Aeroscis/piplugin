# ./tests/test_host_qt/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_qt)
pi_tar_msg(STATUS ${TARGET_NAME} "building qt-based test host...")

# version infos
set(PRODUCT_VERSION_MAJOR 1)
set(PRODUCT_VERSION_MINOR 0)
set(PRODUCT_VERSION_PATCH 0)
set(PRODUCT_VERSION_TWEAK 0)
set(PRODUCT_VERSION_COMMA "${PRODUCT_VERSION_MAJOR},${PRODUCT_VERSION_MINOR},${PRODUCT_VERSION_PATCH},${PRODUCT_VERSION_TWEAK}")
set(PRODUCT_VERSION_STRING "${PRODUCT_VERSION_MAJOR}.${PRODUCT_VERSION_MINOR}.${PRODUCT_VERSION_PATCH}.${PRODUCT_VERSION_TWEAK}")

set(FILE_VERSION_MAJOR 1)
set(FILE_VERSION_MINOR 0)
set(FILE_VERSION_PATCH 0)
set(FILE_VERSION_TWEAK 0)
set(FILE_VERSION_COMMA "${FILE_VERSION_MAJOR},${FILE_VERSION_MINOR},${FILE_VERSION_PATCH},${FILE_VERSION_TWEAK}")
set(FILE_VERSION_STRING "${FILE_VERSION_MAJOR}.${FILE_VERSION_MINOR}.${FILE_VERSION_PATCH}.${FILE_VERSION_TWEAK}")

set(COMPANY_NAME "pipluginframework")
set(FILE_DESCRIPTION "qt-based test host for pi plugin framework (hosts imgui plugin)")
set(LEGAL_COPY_RIGHT "Copyright (c) 2025 pipluginframework")
set(PRODUCT_NAME "${TARGET_NAME}")
set(ORIGIN_FILE_NAME "${TARGET_NAME}.exe")#info for windows only

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
    message(WARNING "Qt5 not found -> ${TARGET_NAME} DISABLED")
    return()
endif()
if(NOT TARGET pipluginframework_imgui)
    message(STATUS "ImGui adapter kit (pipluginframework_imgui) not built -> ${TARGET_NAME} DISABLED")
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
target_link_libraries(${TARGET_NAME} PRIVATE pipluginframework)

# third party
target_link_libraries(${TARGET_NAME} PRIVATE Qt5::Widgets)

# 部署 Qt 运行时 DLL 到可执行文件目录（构建树；安装时也会部署到 <root>/bin/<CONFIG>）
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

# ------- 安装到 bin（<root>/bin/<CONFIG>） -------------
install(TARGETS ${TARGET_NAME}
    RUNTIME DESTINATION ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}
)
# Qt 运行时 DLL 与平台插件一并安装，保证安装后可直接运行
install(FILES
    "${QT_BIN_DIR}/Qt5Core.dll"
    "${QT_BIN_DIR}/Qt5Gui.dll"
    "${QT_BIN_DIR}/Qt5Widgets.dll"
    DESTINATION ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}
)
install(FILES "${QT_BIN_DIR}/../plugins/platforms/qwindows.dll"
    DESTINATION ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/platforms
)