# ./tests/test_headless_host/CMakeLists.txt
cmake_minimum_required(VERSION 3.23)

# target name
set(TARGET_NAME pi_test_host_headless)
pi_tar_msg(STATUS ${TARGET_NAME} "building headless (console) test host...")

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
set(FILE_DESCRIPTION "headless console test host for pi plugin framework")
set(LEGAL_COPY_RIGHT "Copyright (c) 2025 pipluginframework")
set(PRODUCT_NAME "${TARGET_NAME}")
set(ORIGIN_FILE_NAME "${TARGET_NAME}.exe")#info for windows only

# source files（纯 C，仅链接框架核心）
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

# first party
target_link_libraries(${TARGET_NAME} PRIVATE pipluginframework)

# ------- 安装到构建目录（<root>/bin/<CONFIG>） -------------
install(TARGETS ${TARGET_NAME}
    RUNTIME DESTINATION ${GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH}
)