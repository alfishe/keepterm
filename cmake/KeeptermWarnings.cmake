# kt_set_warnings(target): the project warning set.

function(kt_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(KT_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wimplicit-fallthrough)
        # GCC reports false positives of -Wnull-dereference inside libstdc++ (std::function) at -O2+.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wnull-dereference)
        endif()
        if(KT_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
