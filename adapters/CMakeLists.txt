# ./adapters/CMakeLists.txt

#------------------------- build option-------------------------
# UI adapter kits: 可选的可复用“兼容层”，让基于特定 UI 工具包编写的插件可以在任意 host 中运行
# - qt/   : 针对事件循环归 UI 工具包所有（QApplication）的插件，kit 在私有线程上运行该循环并嵌入控件
# - imgui/: 针对立即模式 UI，无需合并事件循环，kit 直接在 host GUI 线程的 pi_plugin_on_idle() 中渲染一帧
# 开关树：总开关 PI_PLUGIN_BUILD_ADAPTERS + 每框架分开关；Conan 侧同名选项整批转发（见 conanfile.py）
option(PI_PLUGIN_BUILD_ADAPTERS "build ui adapter kits (master switch)" ON)
option(PI_PLUGIN_BUILD_ADAPTER_QT "build qt adapter kit (piplugin_qt)" ON)             # 依赖: Qt5
option(PI_PLUGIN_BUILD_ADAPTER_IMGUI "build imgui adapter kit (piplugin_imgui)" ON)     # 依赖: imgui(conan)

if (NOT PI_PLUGIN_BUILD_ADAPTERS)
    return()   # 总开关关死：所有 kit 一律不编（分开关的取值不再有意义）
endif ()

# qt adapter kit（自检 Qt5）
set(ADAPTER_QT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/qt)
if (PI_PLUGIN_BUILD_ADAPTER_QT)
    add_subdirectory(${ADAPTER_QT_PATH})
endif ()

# imgui adapter kit（自检 imgui）
set(ADAPTER_IMGUI_PATH ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
if (PI_PLUGIN_BUILD_ADAPTER_IMGUI)
    add_subdirectory(${ADAPTER_IMGUI_PATH})
endif ()