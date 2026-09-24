# ./CMakeLists.txt
#------------------------- BEGIN -------------------------
cmake_minimum_required(VERSION 3.23)

# include pi module
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pi/pi.cmake)

# Windows 版本资源（W-07）：定义 piplugin_add_version_resource()，供各产品目标的
# CMakeLists 调用（核心库 / 适配器套件 / 宿主 kit；tests/ 与 examples/ 不挂）。
include(${CMAKE_CURRENT_LIST_DIR}/cmake/version_resource.cmake)

# ------------------------------- Project --------------------------------
project(piplugin
        VERSION 0.5.0
        DESCRIPTION "Cross-platform plugin framework with COM-style C ABI"
        HOMEPAGE_URL "https://gitee.com/Aeroscis/piplugin"
        LANGUAGES C CXX
    )
pi_proj_msg(STATUS "project [${PROJECT_NAME}] declared")# project msg

# ------------------------------- 安装前缀默认值 --------------------------------
# 本仓库的安装前缀默认落在 <build>/install：Windows 上 CMAKE_INSTALL_PREFIX 的平台
# 默认值是 "C:/Program Files/piplugin"，无提权必然失败（ECO-04 踩过这个坑）。
# 只在 CMake 把前缀初始化成平台默认值时改写，用户/conan/工具链显式给出的前缀一律尊重
# ——原先这条约定只写在 CMakeUserPresets.json 的 conan-default-local 预设里（那是个
# 本地文件，见 CMakePresets.json 头部注释），现在收进工程本身，三种流程行为一致。
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH
        "install prefix (this repo defaults to <build>/install)" FORCE)
    pi_proj_msg(STATUS "安装前缀未显式指定 -> 使用 ${CMAKE_INSTALL_PREFIX}")
endif()

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

# CMake 包配置的安装子目录（ECO-04）：用**包名**而不是命名空间名。
# find_package(<name>) 只搜索 <prefix>/lib/cmake/<name>*/，所以放在 cmake/pi/ 下的
# 配置对 find_package(piplugin) 是不可见的；统一放到 cmake/piplugin/。
# 旧的 find_package(pi) 入口保留一个转发配置，见 src/CMakeLists.txt。
set(PI_CMAKE_PACKAGE_SUBDIR "cmake/piplugin" CACHE STRING "CMake package config install subdir")
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

#------------------------- 分发产物（W-08） -------------------------
# 三种分发形态：conan 包（conanfile.py）、裸 install 树（cmake --install）、以及这里的
# cpack 归档。归档内容 = install 规则装出来的东西（bin/<CONFIG>/、lib/<CONFIG>/、
# include/、cmake 包配置 + 根文档），因此"装出来的能用"与"解压出来的能用"是同一件事：
# scripts/verify_package.ps1 的 C 段把 zip 解到干净目录，只用它（不带 conan 工具链、
# PATH 上没有本仓库）构建出一个宿主程序并运行来断言。
#
# 归档里**不含**任何宿主可执行文件：产品本身没有宿主 EXE，tests/ 的宿主与 examples/ 的
# 示例按既有约定都不进分发（见 conanfile.py 的 exports_sources 注释）。

# 根文档随包分发：LICENSE 是分发的基本义务（build.md #7 的遗留项之一），README/CHANGELOG
# 让拿到归档的人不必回仓库就能知道这是什么、这一版改了什么。
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md
    DESTINATION .
)

set(CPACK_PACKAGE_NAME "piplugin")
set(CPACK_PACKAGE_VENDOR "Aeroscis")
set(CPACK_PACKAGE_CONTACT "Aeroscis")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
# 归档根直接是 bin/、lib/、include/：Windows 上 CPack 默认继承 CMAKE_INSTALL_PREFIX，
# 那会把整棵树塞进 zip 里的 /Program Files/piplugin/...（解压的人要先绕过一层假路径）。
set(CPACK_PACKAGING_INSTALL_PREFIX "")
# 归档名带配置，但**不能**在这里算：CPACK_PACKAGE_FILE_NAME 既拿不到多配置生成器的
# 配置，也不支持生成器表达式（写成尖括号表达式会得到一个含 '<' '>' 的字面目录名，
# Windows 上建目录即失败 —— 实测）。改成在打包时由 CPack 引入的项目脚本里算，
# 那里 CPACK_BUILD_CONFIG 已经就绪。
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackProjectConfig.cmake")
# 兜底值（本文件被 -D 覆盖或项目脚本没生效时用），同样不含配置后缀。
set(CPACK_PACKAGE_FILE_NAME "piplugin-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "ZIP")
# NSIS 是可选形态（需要本机装 NSIS）：默认关，避免没有 NSIS 的机器上 cpack 直接失败。
option(PI_CPACK_NSIS "Also generate an NSIS installer (requires NSIS on PATH)" OFF)
if(PI_CPACK_NSIS)
    list(APPEND CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "piplugin")
    set(CPACK_NSIS_PACKAGE_NAME "piplugin")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()
include(CPack)

#------------------------- END -------------------------