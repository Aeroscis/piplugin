# ./tests/test_headless_host/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_headless)
pi_tar_msg(STATUS ${TARGET_NAME} "building headless (console) test host...")

# source files（纯 C，链接框架核心 + 宿主 kit L0）
set(SRC_FILE_LIST
    pi_headless_host.c
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

# add executable target
add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)

# 测试共用头：app 自定义宿主服务（通道 B / APP-01）
target_include_directories(${TARGET_NAME} PRIVATE ${PI_TESTS_COMMON_DIR})

# 宿主 kit L0（加载/门禁/实例化/七步卸载序列）缺失则禁用本宿主
if(NOT TARGET piplugin_host)
    message(WARNING "host kit L0 (piplugin_host) not built -> ${TARGET_NAME} DISABLED")
    message(WARNING "  enable -o PI_BUILD_HOST_KITS=True -o PI_BUILD_HOST_KIT_CORE=True")
    return()
endif()

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)
# 宿主侧 kit L0（宿主 kit；STATIC）
target_link_libraries(${TARGET_NAME} PRIVATE piplugin_host)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪，不依赖 install 阶段） -------------
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- ctest 注册：headless 冒烟测试（严格按退出码判定） -------------
# 用部署目录里的插件文件名（与可执行文件同目录，Qt 运行时 DLL 也在那里），
# 所以工作目录必须是部署目录。插件关掉时该用例不注册。
if(TARGET pi_test_plugin_qt)
    add_test(NAME headless_host_smoke
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_qt.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(headless_host_smoke PROPERTIES TIMEOUT 120)
endif()

# 同一台 headless 宿主加载 imgui 插件：CI 上 Qt 是关掉的，这个用例保证
# "headless + 任意插件"这条路在 CI 里也有覆盖（imgui 由 conan 提供，CI 有）。
if(TARGET pi_test_plugin_imgui)
    add_test(NAME headless_host_imgui_smoke
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_imgui.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(headless_host_imgui_smoke PROPERTIES TIMEOUT 120)
endif()

# ---- APP-07：headless + service 插件（IPiPluginService 全生命周期）----
# 宿主自己把 start / poll / status / stop（含幂等与错误路径）逐条断言，任何一条
# 不符就以退出码 1 结束，所以这个用例只按退出码判定 —— 比正则更严格。
# 场景矩阵里的"headless 宿主 + service 插件"那一格就是它。
if(TARGET pi_test_plugin_service)
    add_test(NAME headless_host_service_lifecycle
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_service.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(headless_host_service_lifecycle PROPERTIES TIMEOUT 120)
endif()

# ---- APP-04：descriptor 自由元数据（properties）被宿主读出来 ----
# 宿主把插件声明的属性列出来，并按 key 取值打一行（值随插件不同而不同），
# 这里断言"按键取到的那一行"存在且正确。与版本门禁用例同理：设了
# PASS_REGULAR_EXPRESSION 之后 ctest 不再检查退出码，同一组合的退出码由
# 上面的 smoke 用例负责。
if(TARGET pi_test_plugin_imgui)
    add_test(NAME descriptor_properties_imgui_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_imgui.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(descriptor_properties_imgui_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "property com.example.kind = imgui-plugin"
        TIMEOUT 120)
endif()

if(TARGET pi_test_plugin_service)
    add_test(NAME descriptor_properties_service_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_service.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(descriptor_properties_service_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "property com.example.kind = service-plugin"
        TIMEOUT 120)
endif()

if(TARGET pi_test_plugin_qt)
    add_test(NAME descriptor_properties_qt_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_qt.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(descriptor_properties_qt_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "property com.example.kind = qt-plugin"
        TIMEOUT 120)
endif()

# ---- ECO-08 负向用例：headless 宿主必须拒绝 HOST_UI REQUIRED 的插件 ----
# 与版本门禁用例同理：宿主以非 0 退出（拒绝 = 期望结果），所以断言的是
# "拒绝理由出现在输出里"。能力门禁在 create_instance 之前跑，插件自己的
# CreateInstance 返回 NOTIMPL —— 一旦门禁失效，日志里会出现 NOTIMPL 而不是拒绝理由。
if(TARGET pi_test_plugin_guirequired)
    add_test(NAME capability_gate_rejects_gui_required_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_guirequired.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(capability_gate_rejects_gui_required_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "requires capability"
        TIMEOUT 120)
endif()
# ---- APP-01（通道 B：app 自定义宿主服务）的端到端断言 ----
# 宿主用 pi_plugin_host_services_create_ex() 的 extra_qi 钩子暴露自己的服务；插件对
# 宿主对象 QI 到它并调用。宿主自己只做 QI 探测（不调方法），因此那行计数里的
# 非 0 值只可能来自插件 —— 断言它，就是断言"QI + 调用"真的跑通了。
#
# 格式刻意不含括号，这样这里的正则不需要转义。与版本门禁用例同理：设了
# PASS_REGULAR_EXPRESSION 之后 ctest 不再检查退出码，本用例断言的是"这一行
# 出现在输出里"（同一组合的退出码由上面的 smoke 用例负责）。
set(PI_APP_DEFINED_SERVICE_REGEX "app-defined host service: [1-9]")

if(TARGET pi_test_plugin_qt)
    add_test(NAME app_defined_host_service_qt_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_qt.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(app_defined_host_service_qt_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "${PI_APP_DEFINED_SERVICE_REGEX}"
        TIMEOUT 120)
endif()

if(TARGET pi_test_plugin_imgui)
    add_test(NAME app_defined_host_service_imgui_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_imgui.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(app_defined_host_service_imgui_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "${PI_APP_DEFINED_SERVICE_REGEX}"
        TIMEOUT 120)
endif()

# BLK-03 负向用例：声明不兼容 api_version 的插件必须在 create_instance 之前被拒绝。
# 这里**故意**使用 PASS_REGULAR_EXPRESSION —— 设了它 ctest 就不再检查退出码，
# 而本用例的宿主本来就以非 0 退出（"拒绝"正是期望结果），所以要断言的是
# "拒绝理由出现在宿主输出里"。上半句由 unit 用例负责验证纯函数边界。
if(TARGET pi_test_plugin_badversion)
    add_test(NAME version_gate_rejects_incompatible_plugin
        COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe" "pi_test_plugin_badversion.dll"
        WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
        )
    set_tests_properties(version_gate_rejects_incompatible_plugin PROPERTIES
        PASS_REGULAR_EXPRESSION "incompatible with host"
        TIMEOUT 120)
endif()