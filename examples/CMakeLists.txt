if(NOT CMQ_BUILD_EXAMPLES)
    return()
endif()

function(cmq_add_example name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE cmsgqueue)
    target_compile_options(${name} PRIVATE ${CMQ_WARNING_FLAGS})
endfunction()
