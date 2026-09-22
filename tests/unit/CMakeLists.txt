# ./tests/unit/CMakeLists.txt
#
# 核心回归单测（roadmap BLK-06）：裸 C，无第三方测试框架，由 ctest 注册。
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_unit_tests)
pi_tar_msg(STATUS ${TARGET_NAME} "building unit tests...")

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

set(COMPANY_NAME "piplugin")
set(FILE_DESCRIPTION "unit tests for the pi plugin framework core")
set(LEGAL_COPY_RIGHT "Copyright (c) 2025 piplugin")
set(PRODUCT_NAME "${TARGET_NAME}")
set(ORIGIN_FILE_NAME "${TARGET_NAME}.exe")#info for windows only

# source files（纯 C，仅依赖框架核心）
set(SRC_FILE_LIST
    pi_unit_tests.c
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

# first party
target_link_libraries(${TARGET_NAME} PRIVATE piplugin)

# ------- 部署到开发目录 bin/<CONFIG>（POST_BUILD：构建即就绪） -------------
# 核心是 SHARED，可执行文件必须与 piplugin*.dll 同目录才能启动；
# ctest 运行的是部署后的副本（见下面的 add_test）。
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${TARGET_NAME}>"
        "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    COMMENT "Deploy ${TARGET_NAME} -> ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
)

# ------- ctest 注册 -------------
# 只按退出码判定（0 = 全过），刻意不加 PASS_REGULAR_EXPRESSION —— 设了它
# ctest 就不再检查退出码，反而更松。
add_test(NAME unit
    COMMAND "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}/${TARGET_NAME}.exe"
    WORKING_DIRECTORY "${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}"
    )
set_tests_properties(unit PROPERTIES TIMEOUT 120)
