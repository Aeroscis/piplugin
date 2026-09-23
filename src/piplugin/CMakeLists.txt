# ./src/piplugin/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# include GNU best practice
include(GNUInstallDirs)

# namespace name
if(NOT NAMESPACE_NAME)
    set(NAMESPACE_NAME pi)
endif()

# component name（库名与工程同名，FULL_NAME 不再拼接 namespace 前缀）
set(COMPONENT_NAME piplugin)

# target name & full name
set(TARGET_NAME ${COMPONENT_NAME})
set(FULL_NAME ${COMPONENT_NAME})
pi_tar_msg(STATUS ${FULL_NAME} "library configuring...")

#生成配置头文件 #再出现更复杂的配置需求之前暂时不使用，且该 best practice 不支持其他构建系统，如果要使用qmake得编写额外PY脚本实现该函数的相同效果
# set(TARGET_CONFIG_HEADER_TEMPLATE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include/${TARGET_NAME}/config/config.h.in)
# set(TARGET_CONFIG_HEADER_PATH ${CMAKE_CURRENT_BINARY_DIR}/include/${TARGET_NAME}/config/config.h)
# configure_file(
#     ${TARGET_CONFIG_HEADER_TEMPLATE_PATH}
#     ${TARGET_CONFIG_HEADER_PATH}
#     @ONLY #只替换@强包裹的
# )
if(WIN32)
    #生成 windows .rc 文件（模板 version_dll.rc.in 暂未提供，模板就绪后取消注释）
    # set(TARGET_VERSION_RES_TEMPLATE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/config/version_dll.rc.in)
    # set(TARGET_VERSION_RES_PATH ${CMAKE_CURRENT_BINARY_DIR}/src/config/version_dll.rc)
    # configure_file(
    #     ${TARGET_VERSION_RES_TEMPLATE_PATH}
    #     ${TARGET_VERSION_RES_PATH}
    #     @ONLY #只替换@强包裹的
    # )
    # set_source_files_properties(
    #     ${TARGET_VERSION_RES_PATH}
    #     PROPERTIES GENERATED TRUE)
endif(WIN32)

# source files（框架源码位于工程根 src/ 与 include/，此处以相对路径引用）
set(SRC_FILE_LIST
    ../pi_plugin_host.c
    ../pi_plugin_unknown.c
    )
set(HEADER_FILE_LIST
    ../../include/piplugin/pi_plugin.h
    ../../include/piplugin/pi_plugin_base.h
    ../../include/piplugin/pi_cpp.h
    ../../include/piplugin/pi_plugin_events.h
    ../../include/piplugin/pi_plugin_factory.h
    ../../include/piplugin/pi_plugin_host.h
    ../../include/piplugin/pi_plugin_host_services.h
    ../../include/piplugin/pi_plugin_service.h
    ../../include/piplugin/pi_plugin_types.h
    ../../include/piplugin/pi_plugin_unknown.h
    ../../include/piplugin/pi_plugin_view.h
    )

set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

set(TARGET_HEADER_FILE_LIST
    ${HEADER_FILE_LIST}
    )

# add library target
add_library(${TARGET_NAME} SHARED)                           # 本体
add_library(${NAMESPACE_NAME}::${TARGET_NAME} ALIAS ${TARGET_NAME}) # 别名
set_target_properties(${TARGET_NAME} PROPERTIES              # 属性
    OUTPUT_NAME ${FULL_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
# 构建产物（.dll/.lib）输出到 <root>/lib/<CONFIG>，沿用原始布局（lib/ 存放库本体与符号表）。
# 注意：per-config 输出属性的后缀是 CMake 约定的大写（DEBUG/RELEASE/...），
# 但目录名使用与 $<CONFIG> 一致的规范名（Debug/Release/RelWithDebInfo/MinSizeRel）。
if(DEFINED GLOBAL_PROJECT_PATH)
    foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(_cfg_dir_name ${cfg})
        if(_cfg_dir_name STREQUAL "RELWITHDEBINFO")
            set(_cfg_dir_name "RelWithDebInfo")
        elseif(_cfg_dir_name STREQUAL "MINSIZEREL")
            set(_cfg_dir_name "MinSizeRel")
        else()
            string(SUBSTRING "${_cfg_dir_name}" 0 1 _cfg_head)
            string(SUBSTRING "${_cfg_dir_name}" 1 -1 _cfg_tail)
            string(TOLOWER "${_cfg_tail}" _cfg_tail)
            set(_cfg_dir_name "${_cfg_head}${_cfg_tail}")
        endif()
        set_property(TARGET ${TARGET_NAME} PROPERTY "RUNTIME_OUTPUT_DIRECTORY_${cfg}" ${GLOBAL_PROJECT_PATH}/lib/${_cfg_dir_name})
        set_property(TARGET ${TARGET_NAME} PROPERTY "LIBRARY_OUTPUT_DIRECTORY_${cfg}" ${GLOBAL_PROJECT_PATH}/lib/${_cfg_dir_name})
        set_property(TARGET ${TARGET_NAME} PROPERTY "ARCHIVE_OUTPUT_DIRECTORY_${cfg}" ${GLOBAL_PROJECT_PATH}/lib/${_cfg_dir_name})
    endforeach()
endif()
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        FILES ${TARGET_HEADER_FILE_LIST}
)

# 预制宏定义
target_compile_definitions(${TARGET_NAME} PRIVATE PI_PLUGIN_BUILDING)

# self header
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# third party（框架本体无第三方依赖；windows 平台系统库）
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE user32)
endif()

# morden cmake install and export
# ------- 安装 -------------
install(TARGETS ${TARGET_NAME}
    EXPORT ${FULL_NAME}Targets # 导出集名称 内部使用
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG> # linux .so 动态库文件; windows N/A
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG> # linux .a 静态库文件; windows .lib 动态库符号表文件
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/$<CONFIG> # linux ELF 可执行文件; windows .dll 动态库实现文件
    FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ------- 部署 dll 到开发目录 bin/<CONFIG>（供可执行程序同目录运行；.lib 已随构建产出在 lib/） -------------
# 用 POST_BUILD 而非绝对安装目标：构建该 target 即刷新 bin/<CONFIG>，不依赖 install 链，
# 也不会在 install 阶段被前面（如 ${CMAKE_INSTALL_PREFIX}/lib/...）的失败连带跳过。
# 注意 1：$<TARGET_PDB_FILE> 在 Release/RelWithDebInfo 下为空，必须用 $<IF:> 包一层，
#         否则 cmake -E copy 会因源文件不存在而让整个构建失败（MSB3073）。
# 注意 2：目标目录可能尚不存在，file(INSTALL) 与 copy 都不会递归建目录，故先 make_directory。
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "$<IF:$<CONFIG:Debug>,$<TARGET_PDB_FILE:${TARGET_NAME}>,>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- 生成并导出 CMake targets 文件 -------
install(EXPORT ${FULL_NAME}Targets
    FILE ${FULL_NAME}Targets.cmake
    NAMESPACE ${NAMESPACE_NAME}:: # 此处需要带冒号，CMAKE遵循显式原则。外部库引用使用${NAMESPACE_NAME}::${TARGET_NAME}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)

# ------- 生成 CMake config 文件 -------
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/${FULL_NAME}Config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/${FULL_NAME}Config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)

# ------- 安装 CMake config 文件 -------
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${FULL_NAME}Config.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)