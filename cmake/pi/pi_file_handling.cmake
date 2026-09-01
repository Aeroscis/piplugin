# pi_file_handling.cmake
include_guard(GLOBAL)

# 可配置的文件扩展名集合（默认值；使用者可在 include 后覆盖以扩展支持范围）。
# 两个分类函数共用，保证规则一致。
if(NOT DEFINED PI_HEADER_EXTS)
    set(PI_HEADER_EXTS .h .hpp .hh .hxx)
endif()
if(NOT DEFINED PI_SOURCE_EXTS)
    set(PI_SOURCE_EXTS .cpp .cc .cxx .c++)
endif()

#include(${CMAKE_CURRENT_LIST_DIR}/pi_debug.cmake)

function(pi_conv_abs_paths_to_rel_paths ABS_FILE_PATH_LIST REL_FILE_PATH_LIST)
    pi_exam_var(${ABS_FILE_PATH_LIST})

    # check input list empty
    if (NOT ABS_FILE_PATH_LIST)
        message(WARNING "Input list is empty, nothing to convert.")
        set(${REL_FILE_PATH_LIST} "" PARENT_SCOPE)
        return()
    endif ()

    # init output list
    set(OUTPUT_LIST "")

    foreach (ABS_FILE IN LISTS ${ABS_FILE_PATH_LIST})
        file(RELATIVE_PATH REL_FILE "${CMAKE_CURRENT_SOURCE_DIR}" "${ABS_FILE}")
        list(APPEND OUTPUT_LIST "${REL_FILE}")
    endforeach ()

    set(${REL_FILE_PATH_LIST} ${OUTPUT_LIST} PARENT_SCOPE)

    pi_exam_var(OUTPUT_LIST)
endfunction()


function(pi_extract_public_headers ALL_FILES PUBLIC_HEADERS)
    # args:
    # ALL_FILES - input arg，所有文件列表（约定：传【变量名】,
    #            与 pi_classify_cpp_files / pi_conv_abs_paths_to_rel_paths 一致；
    #            函数内部用 ${} 再解引用一层）
    # PUBLIC_HEADERS - output arg，extracted public headers

    set(extracted_headers "")

    foreach (file IN LISTS ${ALL_FILES})
        # get file ext name
        get_filename_component(ext ${file} EXT)

        # check if file is header（统一用 PI_HEADER_EXTS）
        if (ext IN_LIST PI_HEADER_EXTS)
            # get file name without path
            get_filename_component(fileName ${file} NAME)

            # check if file name contains "_p." sub string（私有头约定）
            string(FIND "${fileName}" "_p." pos)
            if (pos EQUAL -1)  # fail to find "_p" sub string
                list(APPEND extracted_headers "${file}")
            endif ()
        endif ()
    endforeach ()

    # set output arg
    set(${PUBLIC_HEADERS} "${extracted_headers}" PARENT_SCOPE)
endfunction()


function(pi_classify_cpp_files
        CPP_FILE_LIST_VAR_NAME
        PUBLIC_HEADER_VAR_NAME
        PRIVATE_HEADER_VAR_NAME
        SRC_VAR_NAME)

    pi_exam_var(${CPP_FILE_LIST_VAR_NAME})

    # 初始化临时列表
    set(temp_public_headers)
    set(temp_private_headers)
    set(temp_sources)

    # 遍历文件并分类
    foreach (file IN LISTS ${CPP_FILE_LIST_VAR_NAME})
        get_filename_component(ext "${file}" EXT)

        if (ext IN_LIST PI_HEADER_EXTS)
            # 私有头约定：文件名含 "_p."（与 pi_extract_public_headers 规则一致）
            get_filename_component(fileName "${file}" NAME)
            string(FIND "${fileName}" "_p." pos)
            if (pos EQUAL -1)
                list(APPEND temp_public_headers "${file}")
            else ()
                list(APPEND temp_private_headers "${file}")
            endif ()
        elseif (ext IN_LIST PI_SOURCE_EXTS)
            list(APPEND temp_sources "${file}")
        else ()
            message(WARNING "Unknown or unsupported file type: ${file}")
        endif ()
    endforeach ()

    # 将结果通过变量名“引用”返回（设置到父作用域）
    set(${PUBLIC_HEADER_VAR_NAME} ${temp_public_headers} PARENT_SCOPE)
    set(${PRIVATE_HEADER_VAR_NAME} ${temp_private_headers} PARENT_SCOPE)
    set(${SRC_VAR_NAME} ${temp_sources} PARENT_SCOPE)

endfunction()
