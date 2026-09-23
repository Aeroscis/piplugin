# ./examples/CMakeLists.txt
#
# roadmap ECO-03：把 docs/tutorial/ 里的代码段变成**可构建、可运行**的例子。
# 每个例子自带 CMakeLists 与 README（三步跑通），并且都只依赖**公开 API**
# （核心库 + 可选的套件/宿主 kit），不引用 tests/ 里的任何东西 —— 这一点是刻意的：
# 读者可以把整个目录拷出去当起点。
#
# 与 tests/ 的分工：tests/ 是回归与验收（断言、退出码、负向用例），
# examples/ 是"怎么用"的最小示范（尽量少代码、尽量多注释、跑起来能看见输出）。
#
# 开关 PI_BUILD_EXAMPLES 由根 CMakeLists.txt 声明（总开关），这里不再重复声明。
pi_tar_msg(STATUS "examples" "building examples...")

# 服务插件先加：minimal_host 注册的 ctest 用例要用它（TARGET 守卫需要它已存在）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/service_plugin)

# 最小宿主：一个窗口 + 一个容器 + 一个插件（view 或 service 都能驱）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/minimal_host)

# 最小插件：imgui 套件（无 Qt 依赖，CI 里也能构建）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/minimal_plugin_imgui)

# 最小插件：Qt 套件（找不到 Qt5 时自动禁用）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/minimal_plugin_qt)

# 特化 app：app 自定义协议（通道 A）+ 宿主自定义服务（通道 B）+ 能力门禁
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/specialized_app)
