# pi_project.cmake
include_guard(GLOBAL)

# 依赖 pi_debug（pi_exam_var）与 pi_message（pi_proj_msg / pi_proj_err / pi_tar_msg）。
# include_guard(GLOBAL) 保证无论经 pi.cmake 入口还是单独 include，都不会重复定义。
include(${CMAKE_CURRENT_LIST_DIR}/pi_debug.cmake)

# 函数：配置语言及版本要求
# 调用形式：pi_cfg_lang_ver(<LANG> <VER> [REQUIRED])，如 "CXX 17 REQUIRED" 或 "C 11"；
# 不带参数时默认 C++17 REQUIRED。
# 用 cmake_parse_arguments 以声明式解析，取代手写 ARGV 索引循环（原 mini-DSL）。
function(pi_cfg_lang_ver)
    if(ARGC EQUAL 0)
        # 未传参数，默认使用 C++17（REQUIRED）
        message(STATUS "pi_cfg_lang_ver: no args, defaulting to CXX 17 REQUIRED")
        set(CMAKE_CXX_STANDARD 17 PARENT_SCOPE)
        set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)
        return()
    endif()

    # REQUIRED -> 布尔选项；CXX / C -> 单值关键字（值为版本号）
    cmake_parse_arguments(_pi "REQUIRED" "CXX;C" "" ${ARGN})

    # 未匹配到任何关键字的参数视为非法（含拼错的语言名）
    if(DEFINED _pi_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pi_cfg_lang_ver: unexpected arguments: ${_pi_UNPARSED_ARGUMENTS}")
    endif()

    foreach(_lang C CXX)
        if(_pi_${_lang})
            set(CMAKE_${_lang}_STANDARD ${_pi_${_lang}} PARENT_SCOPE)
            if(_pi_REQUIRED)
                set(CMAKE_${_lang}_STANDARD_REQUIRED ON PARENT_SCOPE)
            endif()
        endif()
    endforeach()
endfunction()

function(pi_init_glob_proj)
    
    #------------------------- configure language version -------------------------
    # 注意：pi_cfg_lang_ver 的 PARENT_SCOPE 只能上浮到本函数作用域，
    # 本函数返回后该作用域即销毁，必须在此处把标准变量再转发一级回到真正的调用者。
    pi_cfg_lang_ver(${ARGV})
    set(_pi_cfg_langs C CXX)
    foreach(_lang IN LISTS _pi_cfg_langs)
        if(DEFINED CMAKE_${_lang}_STANDARD)
            set(CMAKE_${_lang}_STANDARD ${CMAKE_${_lang}_STANDARD} PARENT_SCOPE)
        endif()
        if(DEFINED CMAKE_${_lang}_STANDARD_REQUIRED)
            set(CMAKE_${_lang}_STANDARD_REQUIRED ${CMAKE_${_lang}_STANDARD_REQUIRED} PARENT_SCOPE)
        endif()
    endforeach()
    if(CMAKE_CXX_COMPILER_ID)
        set(CMAKE_CXX_EXTENSIONS OFF PARENT_SCOPE) # disable non-iso。开启 ISO 严格模式应设为 OFF
    endif()
    if(CMAKE_C_COMPILER_ID)
        set(CMAKE_C_EXTENSIONS OFF PARENT_SCOPE)
    endif()
	
    #------------------------- global project constant -------------------------
	
    # global project path
    set(GLOBAL_PROJECT_PATH ${PROJECT_SOURCE_DIR})
    set(GLOBAL_PROJECT_PATH ${PROJECT_SOURCE_DIR} PARENT_SCOPE)

    # bin path
    set(GLOBAL_PROJECT_BIN_PATH ${GLOBAL_PROJECT_PATH}/bin)
    set(GLOBAL_PROJECT_BIN_PATH ${GLOBAL_PROJECT_PATH}/bin PARENT_SCOPE)

    # bin build type path ->注意本路径含有生成器表达式，在configure阶段会保留字面量，直到build阶段才展开
    set(GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH ${GLOBAL_PROJECT_PATH}/bin/$<CONFIG>)
    set(GLOBAL_PROJECT_BIN_BUILD_TYPE_PATH ${GLOBAL_PROJECT_PATH}/bin/$<CONFIG> PARENT_SCOPE)

    # src path
    set(GLOBAL_PROJECT_SRC_PATH ${GLOBAL_PROJECT_PATH}/src)
    set(GLOBAL_PROJECT_SRC_PATH ${GLOBAL_PROJECT_PATH}/src PARENT_SCOPE)

    # tests path
    set(GLOBAL_PROJECT_TESTS_PATH ${GLOBAL_PROJECT_PATH}/tests)
    set(GLOBAL_PROJECT_TESTS_PATH ${GLOBAL_PROJECT_PATH}/tests PARENT_SCOPE)

    # cmake prefix and module path
    pi_proj_msg(STATUS "CMAKE_PREFIX_PATH = ${CMAKE_PREFIX_PATH}")
    pi_proj_msg(STATUS "CMAKE_MODULE_PATH = ${CMAKE_MODULE_PATH}")
	
	# ------------------------- windows best practice -------------------------
	
	# WINDOWS LEAN AND MEAN 宏,精简 windows.h 文件的覆盖范围，微软标准实践，全局应用
    if(WIN32)
        add_compile_definitions(WIN32_LEAN_AND_MEAN)
    endif()

    # 在Windows下需要配置的 符号位置，使得 动态库 也能同时生成 .lib 的符号表
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON PARENT_SCOPE)
	
    # -------------------------Build Arguments -------------------------

	# build type suffix
    set(GLOBAL_PROJECT_BUILD_TYPE_SUFFIX $<$<CONFIG:Debug>:d>)
    set(GLOBAL_PROJECT_BUILD_TYPE_SUFFIX $<$<CONFIG:Debug>:d> PARENT_SCOPE)
	

    # compiler - CXX
    if(CMAKE_CXX_COMPILER_ID)
        if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
            pi_proj_msg(STATUS "Building with CXX compiler \"Clang\"")
            add_compile_options(
                    -Wall          # enable all warnings
                    -Wextra        # extra warnings
                    -Wpedantic     # follow iso strictly
            )
        elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
            pi_proj_msg(STATUS "Building with CXX compiler \"GNU\"")
            add_compile_options(
                    -Wall          # enable all warnings
                    -Wextra        # extra warnings
                    --pedantic-errors     # follow iso strictly, generate error when using gcc extension
            )
        elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
            pi_proj_msg(STATUS "Building with CXX compiler \"MSVC\"")
            add_compile_options(
                    /W4  # highest warning level
                    /permissive-          # follow iso strictly, generate error when using msvc extension
                    /Zc:__cplusplus        # use iso cpp version. e.g. 201703L for C++17
                    /utf-8     # use utf-8 with both source code and runtime string
            )
        else ()
            pi_exam_var(CMAKE_CXX_COMPILER_ID)
            pi_proj_err("Unknown Cpp compiler")
        endif()
    endif()

    # compiler - C
    if(CMAKE_C_COMPILER_ID)
        if ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
            pi_proj_msg(STATUS "Building with C compiler \"Clang\"")
            add_compile_options(
                    -Wall          # enable all warnings
                    -Wextra        # extra warnings
                    -Wpedantic     # follow iso strictly
            )
        elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
            pi_proj_msg(STATUS "Building with C compiler \"GNU\"")
            add_compile_options(
                    -Wall          # enable all warnings
                    -Wextra        # extra warnings
                    --pedantic-errors     # follow iso strictly, generate error when using gcc extension
            )
        elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
            pi_proj_msg(STATUS "Building with C compiler \"MSVC\"")
            add_compile_options(
                    /W4  # highest warning level
                    /permissive-          # follow iso strictly, generate error when using msvc extension
                    /utf-8     # use utf-8 with both source code and runtime string
            )
        else ()
            pi_exam_var(CMAKE_C_COMPILER_ID)
            pi_proj_err("Unknown C compiler")
        endif()
    endif()
endfunction()