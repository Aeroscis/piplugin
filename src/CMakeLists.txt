# ./src/CMakeLists.txt

# include GNU best practice
include(GNUInstallDirs)

#------------------------- build option-------------------------
# pi plugin framework core lib（框架核心，C 实现）
option(PI_BUILD_FRAMEWORK "build pi plugin framework core lib" ON)


# pi plugin framework core
set(LIB_PIPLUGINFRAMEWORK_PATH ${CMAKE_CURRENT_SOURCE_DIR}/pipluginframework)
# pi_exam_var(LIB_PIPLUGINFRAMEWORK_PATH)
if (PI_BUILD_FRAMEWORK)
    add_subdirectory(${LIB_PIPLUGINFRAMEWORK_PATH})
endif ()


# ------- 生成 CMake config 文件（伞配置，等价于 reference 中 dcsConfig.cmake） -------
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/piConfig.cmake.in
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
    "${CMAKE_CURRENT_BINARY_DIR}/piConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/piConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${NAMESPACE_NAME}
)