# ./tests/CMakeLists.txt

#------------------------- build option-------------------------
option(PI_BUILD_TEST_PLUGIN "Build the Qt-based test plugin" ON)                   # 依赖: Qt5 framework qt adapter
option(PI_BUILD_TEST_PLUGIN_IMGUI "Build the ImGui-based test plugin" ON)          # 依赖: imgui framework imgui adapter
option(PI_BUILD_TEST_HOST "Build the imgui-based test host" ON)                    # 依赖: imgui
option(PI_BUILD_TEST_HOST_QT "Build the Qt-based test host" ON)                    # 依赖: Qt5 imgui adapter
option(PI_BUILD_HEADLESS_HOST "Build the headless (console) test host" ON)         # 依赖: framework core

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

# qt-based test plugin
if(PI_BUILD_TEST_PLUGIN)
    add_subdirectory(test_plugin)
endif()

# imgui-based test plugin
if(PI_BUILD_TEST_PLUGIN_IMGUI)
    add_subdirectory(test_plugin_imgui)
endif()