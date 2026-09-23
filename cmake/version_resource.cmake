# version_resource.cmake
#
# 给**产品**目标的 Windows 二进制加版本资源（W-07）：
#
#   piplugin_add_version_resource(<target> "<文件说明，显示在属性页『详细信息』里>")
#
# 做三件事：把工程事实注入模板（@VAR@ 语义）、按配置生成 .rc（$<CONFIG> 参与输出名，
# 于是多配置构建里每个配置各有自己的一份）、把生成的 .rc 挂到目标上。模板与注入的
# 变量清单见 cmake/version_dll.rc.in。
#
# 适用范围：只在本工程的产品目标（核心库 / 适配器套件 / 宿主 kit）上调用。tests/ 与
# examples/ 的测试件、示例件**不**挂（派工板 §0.1 的硬规则：线 C 不碰 tests/）。
#
# 实测结论（决定了怎么用这个函数）：STATIC 库里的 .res **不会**进入消费方的二进制。
# MSVC 链接器只按符号需求拉取静态库成员，纯资源成员永远不被拉进来 —— 用一个带
# 9.9.9.9 版本资源的静态库链接出 exe，exe 的版本信息是空的。所以：
#   * SHARED 库 / 可执行文件：调用即生效，属性页看到版本号；
#   * STATIC 库：资源只是随 .lib 备着（占用几 KB），真正生效要等该目标变成 SHARED
#     或被 /INCLUDE: 强制拉入。本仓库仍对全部产品目标统一调用，理由是套件在 APP-08
#     里刚从 STATIC 改成 SHARED（piplugin_qt）——目标类型是会变的，声明一次比将来
#     逐处补漏更可靠。
include_guard(GLOBAL)

# 模板与本文件同目录（cmake/）
set(PIPLUGIN_VERSION_RC_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/version_dll.rc.in")

function(piplugin_add_version_resource target file_description)
    if(NOT WIN32)
        return()   # 版本资源是 PE 文件的概念：非 Windows 平台不生成、不挂载
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "piplugin_add_version_resource: 目标 '${target}' 不存在")
    endif()
    get_target_property(_already_done ${target} PIPLUGIN_VERSION_RESOURCE)
    if(_already_done)
        return()   # 幂等：重复调用不重复挂载（file(GENERATE) 同一输出文件会报错）
    endif()
    set_target_properties(${target} PROPERTIES PIPLUGIN_VERSION_RESOURCE TRUE)

    # 版本号：PROJECT_VERSION = "0.4.0" -> PE 的 FILEVERSION 固定四段 "0,4,0,0"
    if(NOT PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        message(FATAL_ERROR
            "piplugin_add_version_resource: PROJECT_VERSION='${PROJECT_VERSION}' "
            "不是 major.minor.patch 形式，无法生成 FILEVERSION")
    endif()
    set(PI_VERSION "${PROJECT_VERSION}")
    set(PI_VERSION_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")

    # 文件类型按目标类型选（静态库/动态库/可执行各有自己的 VFT_*）
    get_target_property(_target_type ${target} TYPE)
    if(_target_type STREQUAL "STATIC_LIBRARY")
        set(PI_VERSION_FILE_TYPE "VFT_STATIC_LIB")
    elseif(_target_type STREQUAL "SHARED_LIBRARY" OR _target_type STREQUAL "MODULE_LIBRARY")
        set(PI_VERSION_FILE_TYPE "VFT_DLL")
    else()
        set(PI_VERSION_FILE_TYPE "VFT_APP")
    endif()

    set(PI_VERSION_VENDOR "Aeroscis")
    set(PI_VERSION_PRODUCT_NAME "${PROJECT_NAME}")
    set(PI_VERSION_FILE_DESCRIPTION "${file_description}")
    set(PI_VERSION_COPYRIGHT "Copyright (C) 2026 Aeroscis. MIT licensed.")
    # 这两个走生成器表达式：构建产物名带配置后缀（Debug 为 "d"），configure 阶段
    # 无法知道最终配置，交给 file(GENERATE) 在生成时按配置展开。
    set(PI_VERSION_INTERNAL_NAME "$<TARGET_FILE_BASE_NAME:${target}>")
    set(PI_VERSION_ORIGINAL_FILENAME "$<TARGET_FILE_NAME:${target}>")

    file(READ "${PIPLUGIN_VERSION_RC_TEMPLATE}" _rc_template)
    string(CONFIGURE "${_rc_template}" _rc_content @ONLY)
    set(_rc_generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_version_$<CONFIG>.rc")
    file(GENERATE OUTPUT "${_rc_generated}" CONTENT "${_rc_content}")
    target_sources(${target} PRIVATE "${_rc_generated}")
endfunction()
