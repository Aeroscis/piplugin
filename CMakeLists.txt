# ./CMakeLists.txt
#------------------------- BEGIN -------------------------
cmake_minimum_required(VERSION 3.23)

# include pi module
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pi/pi.cmake)

# ------------------------------- Project --------------------------------
project(piplugin
        VERSION 0.4.0
        DESCRIPTION "Cross-platform plugin framework with COM-style C ABI"
        HOMEPAGE_URL "https://gitee.com/Aeroscis/piplugin"
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
#   - 框架库（piplugin）构建产物 -> <root>/lib/<CONFIG>（沿用原始布局，.lib/.dll/cmake 均位于 lib/）
#   - demo/插件构建到各自构建树，通过 install 部署到 <root>/bin/<CONFIG>（参考 xdcs-naspes-common）
# 部署统一在 install 阶段完成（cmake --install --config <CONFIG> 或 --prefix 指定安装前缀）

# Qt 本地安装路径（ECO-05：Qt 是本地安装，不是 conan 依赖，但仓库不再把某台机器上的
# 绝对路径写死 —— 那样别人必须改仓库文件才能构建 Qt 相关目标）。
#
# 优先级：PI_QT_PREFIX（本文件的缓存变量）> 标准 CMake 变量 Qt5_DIR /
# CMAKE_PREFIX_PATH（含环境变量 CMAKE_PREFIX_PATH）。
# 只有当用户没给任何线索、而本机常见位置确实存在时，才把它当作**提示**用一下。
set(PI_QT_PREFIX "" CACHE PATH
    "Qt5 安装前缀（其下有 lib/cmake/Qt5）；留空则用 Qt5_DIR / CMAKE_PREFIX_PATH")
if(NOT PI_QT_PREFIX AND NOT Qt5_DIR AND NOT CMAKE_PREFIX_PATH AND
   EXISTS "C:/Qt/5.15.2/msvc2019_64")
    set(PI_QT_PREFIX "C:/Qt/5.15.2/msvc2019_64" CACHE PATH
        "Qt5 安装前缀（其下有 lib/cmake/Qt5）；留空则用 Qt5_DIR / CMAKE_PREFIX_PATH" FORCE)
    message(STATUS "PI_QT_PREFIX 未指定：暂用本机常见的 Qt 位置 ${PI_QT_PREFIX}"
                   "（可 -DPI_QT_PREFIX=<path> 覆盖，或改用 -DQt5_DIR=<path>/lib/cmake/Qt5）")
endif()
if(PI_QT_PREFIX)
    if(EXISTS "${PI_QT_PREFIX}")
        list(APPEND CMAKE_PREFIX_PATH "${PI_QT_PREFIX}")
    else()
        message(WARNING "PI_QT_PREFIX=${PI_QT_PREFIX} 不存在 -> Qt 相关目标会被禁用；"
                        "用 -DPI_QT_PREFIX=<真实路径> 或 -DQt5_DIR=<路径>/lib/cmake/Qt5 指定")
    endif()
endif()

# 各子目录在 find_package(Qt5) 失败时统一用这句话给出可操作的指引（别让人去猜）。
set(PI_QT_MISSING_HINT
    "hint: -DPI_QT_PREFIX=<Qt 前缀> 或 -DQt5_DIR=<前缀>/lib/cmake/Qt5，也可设环境变量 CMAKE_PREFIX_PATH")

# src path（框架核心）
add_subdirectory(${GLOBAL_PROJECT_SRC_PATH})
# adapter kits path（UI 兼容层，独立于 src 存在）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/adapters)
# host kits path（宿主侧机制库，独立于 src 与 adapters 存在；tests 里的宿主会链接它）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/host_kits)

# tests 总开关（examples 里也有可注册的 ctest 用例，故 enable_testing 要在两者之前）
option(PI_BUILD_TESTS "Build test hosts and plugins" ON)
if((PI_BUILD_TESTS OR PI_BUILD_EXAMPLES) AND DEFINED GLOBAL_PROJECT_TESTS_PATH)
    # ctest 入口（roadmap BLK-06）：必须在顶级目录调用，且要先于任何 add_test
    enable_testing()
endif()

# examples path（roadmap ECO-03：可构建可运行的最小示范；只依赖公开 API）
option(PI_BUILD_EXAMPLES "Build the examples/ directory" ON)
if(PI_BUILD_EXAMPLES)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/examples)
endif()

# tests path
if(PI_BUILD_TESTS AND DEFINED GLOBAL_PROJECT_TESTS_PATH)
    add_subdirectory(${GLOBAL_PROJECT_TESTS_PATH})
endif()

#------------------------- END -------------------------