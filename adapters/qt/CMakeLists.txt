# ./adapters/qt/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pipluginframework_qt)
pi_tar_msg(STATUS ${TARGET_NAME} "building qt adapter kit...")

# version infos
set(PRODUCT_VERSION_MAJOR 1)
set(PRODUCT_VERSION_MINOR 0)
set(PRODUCT_VERSION_PATCH 0)
set(PRODUCT_VERSION_TWEAK 0)
set(PRODUCT_VERSION_COMMA "${PRODUCT_VERSION_MAJOR},${PRODUCT_VERSION_MINOR},${PRODUCT_VERSION_PATCH},${PRODUCT_VERSION_TWEAK}")
set(PRODUCT_VERSION_STRING "${PRODUCT_VERSION_MAJOR}.${PRODUCT_VERSION_MINOR}.${PRODUCT_VERSION_PATCH}.${PRODUCT_VERSION_TWEAK}")

set(FILE_VERSION_MAJOR 1)
set(FILE_VERSION_MINOR 0)
set(FILE_VERSION_PATCH 0)
set(FILE_VERSION_TWEAK 0)
set(FILE_VERSION_COMMA "${FILE_VERSION_MAJOR},${FILE_VERSION_MINOR},${FILE_VERSION_PATCH},${FILE_VERSION_TWEAK}")
set(FILE_VERSION_STRING "${FILE_VERSION_MAJOR}.${FILE_VERSION_MINOR}.${FILE_VERSION_PATCH}.${FILE_VERSION_TWEAK}")

set(COMPANY_NAME "pipluginframework")
set(FILE_DESCRIPTION "qt ui adapter kit for pi plugin framework")
set(LEGAL_COPY_RIGHT "Copyright (c) 2025 pipluginframework")
set(PRODUCT_NAME "${TARGET_NAME}")
set(ORIGIN_FILE_NAME "${TARGET_NAME}.dll")#info for windows only

# source files
set(SRC_FILE_LIST
    pi_qt_view.cpp
    pi_qt_view.h
    )
set(TARGET_SRC_FILE_LIST
    ${SRC_FILE_LIST}
    )

set(CMAKE_AUTOMOC ON)

# dependency check（Qt5 本地安装，非 conan 依赖；找不到则禁用本 kit）
find_package(Qt5 COMPONENTS Widgets Core QUIET)
if(NOT Qt5_FOUND)
    message(STATUS "Qt5 not found -> Qt adapter kit (${TARGET_NAME}) disabled")
    return()
endif()

# add library target
add_library(${TARGET_NAME} STATIC)
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}${GLOBAL_PROJECT_BUILD_TYPE_SUFFIX}
    )
target_sources(${TARGET_NAME}
    PRIVATE
        ${TARGET_SRC_FILE_LIST}
)
target_compile_definitions(${TARGET_NAME} PRIVATE "LIBRARY_VERSION=\"${FILE_VERSION_STRING}\"")

# self header
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# first party: framework core
target_link_libraries(${TARGET_NAME} PUBLIC pipluginframework)

# third party: qt
target_link_libraries(${TARGET_NAME} PUBLIC Qt5::Widgets)