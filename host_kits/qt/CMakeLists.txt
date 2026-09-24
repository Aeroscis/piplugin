# ./host_kits/qt/CMakeLists.txt
#
# L1 Qt 嵌入胶水：PiPluginEmbedArea（容器包装 + resize 转发 + idle 驱动）。
# 形态说明见 host_kits/README.md：CMake 上是 STATIC target（宿主 link 即可），
# 物理上源码随宿主编译 —— Qt 的 moc 与 C++ ABI 不允许把它预编译成跨版本二进制。
cmake_minimum_required(VERSION 3.23)

# include GNU best practice
include(GNUInstallDirs)

# target name
set(TARGET_NAME piplugin_host_qt)
pi_tar_msg(STATUS ${TARGET_NAME} "building host kit L1 (qt embed area)...")

# source files
set(SRC_FILE_LIST
    pi_host_embed_area.cpp
    pi_host_embed_area.h
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

set(CMAKE_AUTOMOC ON)

# dependency check（Qt5 本地安装，非 conan 依赖；L0 缺失也不成立 —— 本 kit 依赖它）
find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(STATUS "Qt5 not found -> Qt host kit (${TARGET_NAME}) disabled
  ${PI_PLUGIN_QT_MISSING_HINT}")
    return()
endif()
if(NOT TARGET piplugin_host)
    message(STATUS "host kit L0 (piplugin_host) not built -> Qt host kit (${TARGET_NAME}) disabled")
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

# self header（构建树平铺 include，安装树平铺到 host_kits/qt，两侧写法一致）
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/piplugin/host_kits/qt>
)

# first party：框架核心 + 宿主 kit L0（本 kit 用 (session, slot) 绑定，故依赖 L0）
target_link_libraries(${TARGET_NAME} PUBLIC piplugin)
target_link_libraries(${TARGET_NAME} PUBLIC piplugin_host)

# third party: qt
target_link_libraries(${TARGET_NAME} PUBLIC Qt5::Widgets)

# ------- 安装（静态库 + 公共头 + 导出目标；Qt5 为本地安装，消费方需自行可达） -------
install(TARGETS ${TARGET_NAME}
    EXPORT pipluginHostQtTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pi_host_embed_area.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/piplugin/host_kits/qt
)

install(EXPORT pipluginHostQtTargets
    FILE pipluginHostQtTargets.cmake
    NAMESPACE ${NAMESPACE_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PI_PLUGIN_CMAKE_PACKAGE_SUBDIR}
)

# ------- Windows 版本资源（W-07） -------
# STATIC（同 events kit：资源随 .lib 备着，见 cmake/version_resource.cmake 的说明）
piplugin_add_version_resource(${TARGET_NAME} "piplugin host kit L1 (Qt embed area)")
