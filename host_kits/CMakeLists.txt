# ./host_kits/CMakeLists.txt
#
# 宿主侧 kit：消灭宿主程序里重复的「机制」代码，不占有任何 UI 决策权。
# 三层纪律（见 host_kits/README.md「三层结构」，与 docs/design/architecture.md §7 同源）：
#   L0 core/    纯逻辑库，零 GUI 依赖 —— 加载 / 门禁 / 实例化 / 卸载序列
#   L1 qt/      Qt 嵌入区域（容器包装 + resize 转发 + idle 驱动）
#   L1 dx11/    D3D11 设备 + flip-model 交换链的正确创建参数与 resize 策略（仅 Windows）
#   L2          现成控件糖（opt-in；本期不做）
#
# 开关树：总开关 PI_BUILD_HOST_KITS + 每层分开关；Conan 侧同名选项整批转发
# （见 conanfile.py 的开关树注释）。
option(PI_BUILD_HOST_KITS "build host-side kits (master switch)" ON)
option(PI_BUILD_HOST_KIT_CORE "build host kit L0 session library (piplugin_host)" ON)          # 依赖: 仅框架核心
option(PI_BUILD_HOST_KIT_EVENTS "build host-side event router (piplugin_events)" ON)           # 依赖: 仅框架核心
option(PI_BUILD_HOST_KIT_QT "build host kit L1 qt embed area (piplugin_host_qt)" ON)           # 依赖: Qt5 + L0
option(PI_BUILD_HOST_KIT_DX11 "build host kit L1 dx11 embed glue (piplugin_host_dx11)" ON)     # 依赖: Windows SDK

if (NOT PI_BUILD_HOST_KITS)
    return()   # 总开关关死：所有宿主 kit 一律不编（分开关的取值不再有意义）
endif ()

# L0（L1 的 qt kit 依赖它，故先加）
if (PI_BUILD_HOST_KIT_CORE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/core)
endif ()

# 宿主侧事件路由（APP-06 的可选糖；不依赖 L0，L0 也不依赖它）
if (PI_BUILD_HOST_KIT_EVENTS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/events)
endif ()

# L1 Qt 嵌入胶水
if (PI_BUILD_HOST_KIT_QT)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/qt)
endif ()

# L1 DX11 嵌入胶水
if (PI_BUILD_HOST_KIT_DX11)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/dx11)
endif ()
