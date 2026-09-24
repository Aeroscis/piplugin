# ./src/CMakeLists.txt

# include GNU best practice
include(GNUInstallDirs)

#------------------------- build option-------------------------
# pi plugin framework core lib（框架核心，C 实现）
option(PI_PLUGIN_BUILD_FRAMEWORK "build pi plugin framework core lib" ON)


# pi plugin framework core
set(LIB_PIPLUGIN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/piplugin)
# pi_exam_var(LIB_PIPLUGIN_PATH)
if (PI_PLUGIN_BUILD_FRAMEWORK)
    add_subdirectory(${LIB_PIPLUGIN_PATH})
endif ()


# ------- 生成 CMake config 文件（伞配置，等价于 reference 中 dcsConfig.cmake） -------
# ECO-04：伞配置以**包名** pipluginConfig.cmake 安装到 lib/cmake/piplugin/，
# 于是 find_package(piplugin) 直接可用（Conan 侧生成的配置也是这个名字）。
# 同时保留旧入口 find_package(pi)：在 lib/cmake/pi/ 放一个三行转发配置。
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/piConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/pipluginConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PI_PLUGIN_CMAKE_PACKAGE_SUBDIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/pipluginConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# 旧入口：find_package(pi) -> 转发到 piplugin 的伞配置（含版本文件）
configure_package_config_file(
    cmake/piForwardConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/piConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/piConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# ------- 安装 CMake config 文件 -------
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pipluginConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pipluginConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/${PI_PLUGIN_CMAKE_PACKAGE_SUBDIR}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/piConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/piConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)