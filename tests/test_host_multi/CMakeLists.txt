# ./tests/test_host_multi/CMakeLists.txt
#
# roadmap APP-08 的验收宿主：一个进程里**同时**加载两个不同的 Qt 插件 DLL，
# 各自嵌进自己的容器，由同一个 session 驱动，然后一起卸载。
# 刻意的极简 Win32 宿主（无 D3D、无 resize 循环）：脆弱的渲染机器在
# tests/test_host 里，这个宿主只负责"多插件同进程"这一件事。
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_multi)
pi_tar_msg(STATUS ${TARGET_NAME} "building multi-plugin test host...")

# source files（C++，仅依赖框架核心 + 宿主 kit L0）
set(SRC_FILE_LIST
    pi_multi_host.cpp
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# 宿主 kit L0（加载/门禁/多槽位/七步卸载序列）缺失则禁用本宿主
if(NOT TARGET piplugin_host)
    message(WARNING "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} DISABLED")
    message(WARNING "  enable -o PI_BUILD_HOST_KITS=True -o PI_BUILD_HOST_KIT_CORE=True")
    return()
endif()

# add executable target（console 子系统：证据打在 stdout 与 pi_multi_host.log 里）
add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
    )

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_host)
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE ole32 user32)
endif()

# 测试共用头：W-04 的宿主侧 marshal 队列用 PiPluginTestMutex（tests/common/pi_test_thread.h）
target_include_directories(${TARGET_NAME} PRIVATE ${PI_TESTS_COMMON_DIR})

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- ctest 注册：多插件同进程（APP-08 的验收） -------------
# 需要两个**不同的** Qt 插件模块都存在；缺任何一个就不注册（CI 上 Qt 是关的）。
# 宿主自己断言：两个插件都发布了视图、各自嵌进自己的容器且可见、两套 Qt 定时器
# 都在跑（心跳消息分别计数）、双双干净卸载。任一不符 -> 退出码 1。
if(TARGET pi_test_plugin_qt AND TARGET pi_test_plugin_qt2)
    add_test(NAME multi_plugin_qt_in_one_process
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
                "pi_test_plugin_qt.dll" "pi_test_plugin_qt2.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(multi_plugin_qt_in_one_process PROPERTIES TIMEOUT 180)
endif()

# ---- W-04：插件子线程 → 宿主，以及套件的跨线程 pi_plugin_qt_view_post ----
# 两件事在同一个 ctest 用例里断言（同一台宿主、同一次运行）：
#   1. 插件的子线程直接调 pi_plugin_host_post_message()：宿主回调**就在那条子线程上**
#      被调到（契约），宿主自己排队并在主线程上投递（不丢不重）；
#   2. 插件的子线程调 pi_plugin_qt_view_post()：回调必须跑在**宿主 GUI 线程**上。
# 插件侧的探针由环境变量打开 —— 同一份插件还被一致性验收 / APP-08 / headless
# 冒烟共用，默认不能改变它的行为。
if(TARGET pi_test_plugin_qt)
    add_test(NAME qt_view_post_from_worker_thread
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
                "--post-thread" "pi_test_plugin_qt.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(qt_view_post_from_worker_thread PROPERTIES
        ENVIRONMENT "PI_PLUGIN_QT_TEST_POST_THREAD=1"
        TIMEOUT 180)
endif()

# ---- W-02：嵌入窗口运行时切换（测试矩阵里唯一没覆盖的那一格）----
# 加载插件 -> attach 到容器 A -> 用 pi_plugin_host_default_set_ui_window() 把宿主报告的
# 窗口切到 B 并在活视图上 detach/attach 到 B -> 切回 A -> 尺寸往返 -> 卸载；
# 每一步都断言"插件窗口是**指定容器**的子窗口、可见、尺寸与容器客户区一致"，
# 以及 detach 之后旧窗口确实已经没了（detach 是同步的）。退出码判定。
# imgui 插件那条不依赖 Qt（CI 上 Qt 是关的），Qt 插件那条额外覆盖套件的控件重建。
if(TARGET pi_test_plugin_imgui)
    add_test(NAME container_switch_runtime
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
                "--container-switch" "pi_test_plugin_imgui.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(container_switch_runtime PROPERTIES TIMEOUT 180)
endif()

if(TARGET pi_test_plugin_qt)
    add_test(NAME container_switch_runtime_qt
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
                "--container-switch" "pi_test_plugin_qt.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(container_switch_runtime_qt PROPERTIES TIMEOUT 180)
endif()

# ---- W-05：同进程两个 imgui 插件（套件是 STATIC，每个模块各带一份状态）----
# 两个**不同的** imgui 插件模块同时加载、各自嵌进自己的容器、各自被驱动渲染
# 若干帧（插件在自己的 draw 回调里把帧计数报给宿主）、一起干净卸载。断言：
# 各自的窗口在**自己的**容器里且可见、两套心跳都在推进（不是画了一帧就冻住）、
# 帧循环之后两个窗口都还活着且没有串容器、卸载后两个窗口都消失。
# 缺任何一个插件变体就不注册（imgui 由 conan 提供，CI 有）。
if(TARGET pi_test_plugin_imgui AND TARGET pi_test_plugin_imgui2)
    add_test(NAME multi_plugin_imgui_in_one_process
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
                "--imgui-pair" "pi_test_plugin_imgui.dll" "pi_test_plugin_imgui2.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(multi_plugin_imgui_in_one_process PROPERTIES TIMEOUT 180)
endif()
