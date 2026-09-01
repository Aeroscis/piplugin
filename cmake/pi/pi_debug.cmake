# pi_debug.cmake
include_guard(GLOBAL)

# e.g. pi_exam_var(CMAKE_CXX_STANDARD) -> input variable instead of variable value
# console output:
# [cmake] -- CMAKE_CXX_STANDARD = 17
function(pi_exam_var var_name)
    # cmake 形参的形式为 外部再套一层。
    # 在外部调用pi_exam_var(SRC_FILE_LIST)时，假设SRC_FILE_LIST的值为main.cpp
    # 则有：
    # ${var_name} = SRC_FILE_LIST
    # ${${var_name}} = main.cpp

    # empty variable check
    if (NOT ${var_name})
        message(STATUS "${var_name} = (no value)")
		return()
    endif ()
	
    list(LENGTH ${var_name} var_count)
    
	if (${var_count} GREATER 1)# variable is a list
        # init a final output string
        set(formatted_values "")

        # loop each item in list and format it
        foreach (item IN LISTS ${var_name})
            #add semi column and add line feed if item is not empty
            if (NOT formatted_values STREQUAL "")
                set(formatted_values "${formatted_values};\n")
            endif ()
            #append 4 spaces and item value to final output
            set(formatted_values "${formatted_values}    ${item}")
        endforeach ()

        # add a semi column if final output is not empty
        if (NOT formatted_values STREQUAL "")
            set(formatted_values "${formatted_values};")
        endif ()

        # print list variable
        message(STATUS "${var_name} = {\n${formatted_values}\n   }")
    else ()

        # print directly if variable is not list
        message(STATUS "${var_name} = ${${var_name}}")
		
    endif ()
endfunction()
