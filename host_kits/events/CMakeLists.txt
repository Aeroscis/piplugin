# ./host_kits/events/CMakeLists.txt
#
# 宿主侧事件路由：IPiPluginHostEvents 的一个现成实现（订阅表 + 有界队列 + pump + owner 退订）。
# 与 L0 同一形态：纯 C、零第三方依赖、STATIC、只给宿主用（插件只看到核心头里的接口）。
# 它不是必须的 —— 宿主完全可以自己实现 IPiPluginHostEvents；这一层只是把重复的记账消掉。
cmake_minimum_required(VERSION 3.23)

# include GNU best practice
include(GNUInstallDirs)

# target name
set(TARGET_NAME piplugin_events)
set(MEMBER_NAME plugin_events)   # 家族内成员名，决定导出为 pi::plugin_events
pi_tar_msg(STATUS ${TARGET_NAME} "building host-side event router...")

# source files
set(SRC_FILE_LIST
    pi_event_router.c
    pi_event_router.h
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# add library target（STATIC：宿主侧机制，不进插件 ABI）
add_library(${TARGET_NAME} STATIC)
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )

# self header（BUILD_INTERFACE/INSTALL_INTERFACE 拆分：两侧 #include "pi_event_router.h" 写法一致）
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/piplugin/host_kits/events>
)

# first party: framework core
target_link_libraries(${TARGET_NAME} PUBLIC piplugin)

# 非 Windows 下路由器需要互斥量（publish 允许任意线程调用）
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(${TARGET_NAME} PUBLIC Threads::Threads)
endif()

# ------- 安装（Modern CMake：静态库 + 公共头 + 导出目标） -------------
install(TARGETS ${TARGET_NAME}
    EXPORT pipluginEventsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pi_event_router.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/piplugin/host_kits/events
)

add_library(pi::${MEMBER_NAME} ALIAS ${TARGET_NAME})
set_target_properties(${TARGET_NAME} PROPERTIES EXPORT_NAME ${MEMBER_NAME})

install(EXPORT pipluginEventsTargets
    FILE pipluginEventsTargets.cmake
    NAMESPACE ${NAMESPACE_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PI_PLUGIN_CMAKE_PACKAGE_SUBDIR}
)

# ------- Windows 版本资源（W-07） -------
# 本 kit 是 STATIC：资源随 .lib 备着（静态库成员按符号需求拉取，纯资源成员不会进入
# 消费方二进制 —— 详见 cmake/version_resource.cmake），声明在这里是为了目标类型变化
# （STATIC -> SHARED）时不必回头补。
piplugin_add_version_resource(${TARGET_NAME} "piplugin host-side event router")
