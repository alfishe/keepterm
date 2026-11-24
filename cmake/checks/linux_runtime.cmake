# Checks that BINARY carries its C++ runtime statically: no GLIBCXX_*, CXXABI_* or GCC_*
# symbol versions, so it runs without a matching libstdc++. glibc is taken from the build host.

execute_process(COMMAND objdump -T "${BINARY}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "objdump failed on ${BINARY}")
endif()
foreach(bad GLIBCXX_ CXXABI_ GCC_)
    if(out MATCHES "${bad}[0-9]")
        message(FATAL_ERROR "${BINARY} imports ${bad}* symbols; the C++ runtime must be static")
    endif()
endforeach()
message(STATUS "static C++ runtime: ok")
