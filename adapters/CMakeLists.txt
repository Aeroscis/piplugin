# ./adapters/CMakeLists.txt

#------------------------- build option-------------------------
# UI adapter kits: 可选的可复用“兼容层”，让基于特定 UI 工具包编写的插件可以在任意 host 中运行
# - qt/   : 针对事件循环归 UI 工具包所有（QApplication）的插件，kit 在私有线程上运行该循环并嵌入控件
# - imgui/: 针对立即模式 UI，无需合并事件循环，kit 直接在 host GUI 线程的 pi_on_idle() 中渲染一帧
option(PI_BUILD_ADAPTER_QT "build qt adapter kit (pipluginframework_qt)" ON)             # 依赖: Qt5
option(PI_BUILD_ADAPTER_IMGUI "build imgui adapter kit (pipluginframework_imgui)" ON)     # 依赖: imgui(conan)


# qt adapter kit（自检 Qt5）
set(ADAPTER_QT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/qt)
if (PI_BUILD_ADAPTER_QT)
    add_subdirectory(${ADAPTER_QT_PATH})
endif ()

# imgui adapter kit（自检 imgui）
set(ADAPTER_IMGUI_PATH ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
if (PI_BUILD_ADAPTER_IMGUI)
    add_subdirectory(${ADAPTER_IMGUI_PATH})
endif ()