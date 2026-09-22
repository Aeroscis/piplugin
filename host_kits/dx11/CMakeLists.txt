# ./host_kits/dx11/CMakeLists.txt
#
# L1 DX11 嵌入胶水：D3D11 设备 + flip-model 交换链的正确创建参数、resize 策略、
# 容器与窗口风格。仅 Windows。
cmake_minimum_required(VERSION 3.23)

# include GNU best practice
include(GNUInstallDirs)

# target name
set(TARGET_NAME pipluginframework_host_dx11)
pi_tar_msg(STATUS ${TARGET_NAME} "building host kit L1 (dx11 embed glue)...")

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
set(FILE_DESCRIPTION "host-side kit L1 (dx11 embed glue) for pi plugin framework")
set(LEGAL_COPY_RIGHT "Copyright (c) 2025 pipluginframework")
set(PRODUCT_NAME "${TARGET_NAME}")
set(ORIGIN_FILE_NAME "${TARGET_NAME}.dll")#info for windows only

# 仅 Windows：D3D11/DXGI 是平台 API（Linux/macOS 的等价物属 FUT-01/FUT-02）
if(NOT WIN32)
    message(STATUS "not Windows -> DX11 host kit (${TARGET_NAME}) disabled")
    return()
endif()

# source files
set(SRC_FILE_LIST
    pi_host_dx11.cpp
    pi_host_dx11.h
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# add library target
add_library(${TARGET_NAME} STATIC)
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )
target_compile_definitions(${TARGET_NAME} PRIVATE "LIBRARY_VERSION=\"${FILE_VERSION_STRING}\"")

# self header（构建树平铺 include，安装树平铺到 host_kits/dx11，两侧写法一致）
target_include_directories(${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/pipluginframework/host_kits/dx11>
)

# first party：框架核心（本 kit 只用 PiNativeWindow/PiResult，不依赖 L0）
target_link_libraries(${TARGET_NAME} PUBLIC pipluginframework)

# third party：Windows 平台库（PUBLIC：消费方要用到 ID3D11Device 等类型）
target_link_libraries(${TARGET_NAME} PUBLIC d3d11 dxgi)

# ------- 安装（静态库 + 公共头 + 导出目标） -------
install(TARGETS ${TARGET_NAME}
    EXPORT pipluginframeworkHostDx11Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/$<CONFIG>
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pi_host_dx11.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pipluginframework/host_kits/dx11
)

install(EXPORT pipluginframeworkHostDx11Targets
    FILE pipluginframeworkHostDx11Targets.cmake
    NAMESPACE ${NAMESPACE_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)
