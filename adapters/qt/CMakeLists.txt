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
    message(STATUS "Qt5 not found -> Qt adapter kit (${TARGET_NAME}) disabled
  ${PI_QT_MISSING_HINT}")
    return()
endif()

# add library target
# SHARED（roadmap APP-08）：套件持有**进程级**状态（唯一的 QApplication、活视图
# 登记表）。STATIC 时每个 Qt 插件 DLL 各带一份，一个进程加载两个 Qt 插件就会去
# 建第二个 QApplication —— Qt 直接断言 "there should be only one application
# object"（tests/test_host_multi 复现并回归这一点）。代价是插件部署要带套件 DLL。
add_library(${TARGET_NAME} SHARED)
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# 导出宏开关：本套件自己的翻译单元定义 PI_QT_API = dllexport，消费方（插件）
# 看到的是 dllimport。刻意不开 CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS（见 F1）。
target_compile_definitions(${TARGET_NAME} PRIVATE PI_PLUGIN_QT_BUILDING)

# self header（BUILD_INTERFACE/INSTALL_INTERFACE 拆分：两侧 #include "pi_qt_view.h" 写法一致）
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/piplugin/adapters/qt>
)

# first party: framework core
target_link_libraries(${TARGET_NAME} PUBLIC piplugin)

# third party: qt
target_link_libraries(${TARGET_NAME} PUBLIC Qt5::Widgets)

# ------- 安装（SHARED 库 + 公共头 + 导出目标；Qt5 为本地安装，消费方需自行可达） -------
# Windows 上 DLL 走 RUNTIME，导入库走 ARCHIVE；Linux 走 LIBRARY。
install(TARGETS ${TARGET_NAME}
    EXPORT pipluginQtAdapterTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/$<CONFIG>  # windows 套件 DLL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>  # linux .so
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>  # 导入库（windows .lib / linux .a）
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pi_qt_view.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/piplugin/adapters/qt
)

install(EXPORT pipluginQtAdapterTargets
    FILE pipluginQtAdapterTargets.cmake
    NAMESPACE ${NAMESPACE_NAME}:: # 消费方引用 ${NAMESPACE_NAME}::${TARGET_NAME}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)

# ------- 部署套件 DLL 到开发目录 bin/<CONFIG>（构建该 target 即刷新） -------------
# SHARED 之后，任何加载 Qt 插件的可执行程序都必须能找到这个 DLL；本仓库所有
# 测试宿主都在 bin/<CONFIG> 里运行（插件 DLL 也在那里），故与核心库同一做法。
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "$<IF:$<CONFIG:Debug>,$<TARGET_PDB_FILE:${TARGET_NAME}>,>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)