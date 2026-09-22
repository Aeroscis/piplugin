# ./adapters/qt/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# include GNU best practice
include(GNUInstallDirs)

# target name
set(TARGET_NAME piplugin_qt)
pi_tar_msg(STATUS ${TARGET_NAME} "building qt adapter kit...")

# source files
set(SRC_FILE_LIST
    pi_qt_view.cpp
    pi_qt_view.h
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

set(CMAKE_AUTOMOC ON)

# dependency check（Qt5 本地安装，非 conan 依赖；找不到则禁用本 kit）
find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(STATUS "Qt5 not found -> Qt adapter kit (${TARGET_NAME}) disabled")
    return()
endif()

# add library target
add_library(${TARGET_NAME} STATIC)
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# self header（BUILD_INTERFACE/INSTALL_INTERFACE 拆分：两侧 #include "pi_qt_view.h" 写法一致）
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/piplugin/adapters/qt>
)

# first party: framework core
target_link_libraries(${TARGET_NAME} PUBLIC piplugin)

# third party: qt
target_link_libraries(${TARGET_NAME} PUBLIC Qt5::Widgets)

# ------- 安装（Modern CMake：静态库 + 公共头 + 导出目标；Qt5 为本地安装，消费方需自行可达） -------
install(TARGETS ${TARGET_NAME}
    EXPORT pipluginQtAdapterTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>  # 静态库（linux .a / windows .lib）
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pi_qt_view.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/piplugin/adapters/qt
)

install(EXPORT pipluginQtAdapterTargets
    FILE pipluginQtAdapterTargets.cmake
    NAMESPACE ${NAMESPACE_NAME}:: # 消费方引用 ${NAMESPACE_NAME}::${TARGET_NAME}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)