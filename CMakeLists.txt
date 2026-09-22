# ./CMakeLists.txt
#------------------------- BEGIN -------------------------
cmake_minimum_required(VERSION 3.23)

# include pi module
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pi/pi.cmake)

# ------------------------------- Project --------------------------------
project(pipluginframework
        VERSION 1.0.0
        DESCRIPTION "Cross-platform plugin framework with COM-style C ABI"
        HOMEPAGE_URL "https://github.com/example/pipluginframework"
        LANGUAGES C CXX
    )
pi_proj_msg(STATUS "project [${PROJECT_NAME}] declared")# project msg

# Windows路径长度
set(CMAKE_AUTOGEN_INTERMEDIATE_DIR_STRATEGY "SHORT" CACHE STRING "")
set(CMAKE_SHORT_OBJECTS YES)
set(CMAKE_OBJECT_PATH_MAX 512)

# 当前工程为顶级工程时作为全局工程初始化
# (C++17 REQUIRED + C11 REQUIRED: 框架核心与 headless host 为 C11, 其余为 C++17)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    pi_init_glob_proj(CXX 17 C 11 REQUIRED)
endif()

# debug
pi_exam_var(GLOBAL_PROJECT_BUILD_TYPE_SUFFIX)

if(NOT NAMESPACE_NAME)
    set(NAMESPACE_NAME pi)
endif()

#------------------------- build option-------------------------
# 构建产物位置由各模块自行决定：
#   - 框架库（pipluginframework）构建产物 -> <root>/lib/<CONFIG>（沿用原始布局，.lib/.dll/cmake 均位于 lib/）
#   - demo/插件构建到各自构建树，通过 install 部署到 <root>/bin/<CONFIG>（参考 xdcs-naspes-common）
# 部署统一在 install 阶段完成（cmake --install --config <CONFIG> 或 --prefix 指定安装前缀）

# Qt 本地安装路径（Qt 为本地安装，非 conan 依赖）
list(APPEND CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2019_64")

# src path（框架核心）
add_subdirectory(${GLOBAL_PROJECT_SRC_PATH})
# adapter kits path（UI 兼容层，独立于 src 存在）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/adapters)
# host kits path（宿主侧机制库，独立于 src 与 adapters 存在；tests 里的宿主会链接它）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/host_kits)
# tests path
option(PI_BUILD_TESTS "Build test hosts and plugins" ON)
if(PI_BUILD_TESTS AND DEFINED GLOBAL_PROJECT_TESTS_PATH)
    add_subdirectory(${GLOBAL_PROJECT_TESTS_PATH})
endif()

#------------------------- END -------------------------