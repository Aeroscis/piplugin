# pi_message.cmake
include_guard(GLOBAL)

function(pi_glob_proj_msg type msg)
    message(${type} "project[${CMAKE_PROJECT_NAME}]:${msg}")
endfunction()

function(pi_proj_msg type msg)
    message(${type} "project[${PROJECT_NAME}]:${msg}")
endfunction()

function(pi_proj_err msg)
    message(FATAL_ERROR "project[${PROJECT_NAME}]:${msg}")
endfunction()

function(pi_tar_msg type target msg)
    message(${type} "target[${target}]:${msg}")
endfunction()
