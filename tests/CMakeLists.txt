# ./tests/CMakeLists.txt

# 测试共用头目录：宿主与插件两侧都要用到同一份"app 自定义宿主服务"定义
# （同一 128 位 IID + 同一 vtbl 布局），故集中放 tests/common/，由下面的
# 子目录各自加进 include 路径。
set(PI_TESTS_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")

#------------------------- build option-------------------------
option(PI_BUILD_UNIT_TESTS "Build the core unit tests (ctest target 'unit')" ON)      # 依赖: framework core
option(PI_BUILD_TEST_PLUGIN "Build the Qt-based test plugin" ON)                   # 依赖: Qt5 framework qt adapter
option(PI_BUILD_TEST_PLUGIN_IMGUI "Build the ImGui-based test plugin" ON)          # 依赖: imgui framework imgui adapter
option(PI_BUILD_TEST_PLUGIN_BADVERSION "Build the bad-api_version test plugin (BLK-03 negative case)" ON)  # 依赖: framework core
option(PI_BUILD_TEST_HOST "Build the imgui-based test host" ON)                    # 依赖: imgui
option(PI_BUILD_TEST_HOST_QT "Build the Qt-based test host" ON)                    # 依赖: Qt5 imgui adapter
option(PI_BUILD_HEADLESS_HOST "Build the headless (console) test host" ON)         # 依赖: framework core

# unit tests（核心回归；ctest 一条命令跑完）
if(PI_BUILD_UNIT_TESTS)
    add_subdirectory(unit)
endif()

# ---- 插件先加：宿主里注册的 ctest 冒烟用例要引用插件目标（if(TARGET ...) 守卫）----

# qt-based test plugin
if(PI_BUILD_TEST_PLUGIN)
    add_subdirectory(test_plugin)
endif()

# imgui-based test plugin
if(PI_BUILD_TEST_PLUGIN_IMGUI)
    add_subdirectory(test_plugin_imgui)
endif()

# bad-api_version test plugin（BLK-03 负向用例，纯 C 无依赖）
if(PI_BUILD_TEST_PLUGIN_BADVERSION)
    add_subdirectory(test_plugin_badversion)
endif()

# ---- 宿主 ----

# imgui-based test host
if(PI_BUILD_TEST_HOST)
    add_subdirectory(test_host)
endif()

# qt-based test host
if(PI_BUILD_TEST_HOST_QT)
    add_subdirectory(test_host_qt)
endif()

# headless (console) test host
if(PI_BUILD_HEADLESS_HOST)
    add_subdirectory(test_headless_host)
endif()
