# ./host_kits/core/CMakeLists.txt
#
# L0 会话管理：纯逻辑库，零 GUI 依赖（不 find_package 任何第三方），
# 因此没有 adapter kit 那种「依赖缺失 -> 禁用本 kit」的分支。
cmake_minimum_required(VERSION 3.23)

# include GNU best practice
include(GNUInstallDirs)

# target name
set(TARGET_NAME piplugin_host)
pi_tar_msg(STATUS ${TARGET_NAME} "building host kit L0 (plugin session)...")

# source files
set(SRC_FILE_LIST
    pi_host_session.c
    pi_host_session.h
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# add library target（STATIC：与既有 adapter kit 约定一致；L0 无进程级全局状态，
# 故不存在"每 DLL 一份状态"的重演风险，也不需要多部署一个 DLL）
add_library(${TARGET_NAME} STATIC)
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )

# self header（BUILD_INTERFACE/INSTALL_INTERFACE 拆分：两侧 #include "pi_host_session.h" 写法一致）
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/piplugin/host_kits/core>
)

# first party: framework core
target_link_libraries(${TARGET_NAME} PUBLIC piplugin)

# ------- 安装（Modern CMake：静态库 + 公共头 + 导出目标） -------------
install(TARGETS ${TARGET_NAME}
    EXPORT pipluginHostCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>  # 静态库（linux .a / windows .lib）
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pi_host_session.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/piplugin/host_kits/core
)

install(EXPORT pipluginHostCoreTargets
    FILE pipluginHostCoreTargets.cmake
    NAMESPACE ${NAMESPACE_NAME}:: # 消费方引用 ${NAMESPACE_NAME}::${TARGET_NAME}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PI_PLUGIN_CMAKE_PACKAGE_SUBDIR}
)

# ------- Windows 版本资源（W-07） -------
# STATIC（资源随 .lib 备着；静态库里的 .res 不会进入消费方二进制，详见
# cmake/version_resource.cmake —— 这里声明是为了将来改成 SHARED 时自动生效）
piplugin_add_version_resource(${TARGET_NAME} "piplugin host kit L0 (plugin session)")
