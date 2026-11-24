# Build options.

option(KT_WERROR "Treat warnings as errors" OFF)
option(KT_LTO "Enable link-time optimization in release builds" OFF)
option(KT_FAULTS "Compile fault injection points" OFF)
# The tests are not part of every source tree (the published sources leave them out).
if(EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
    option(KT_BUILD_TESTS "Build test targets" ON)
else()
    option(KT_BUILD_TESTS "Build test targets" OFF)
    if(KT_BUILD_TESTS)
        message(FATAL_ERROR "KT_BUILD_TESTS=ON, but this source tree has no tests/")
    endif()
endif()
set(KT_SANITIZE "" CACHE STRING "Sanitizers: address;undefined, thread, fuzzer")

if(WIN32)
    option(KT_EXT_WIN "Build the opt-in extended Windows features (OpenConsole, process cwd, NT process snapshot)" ON)
else()
    set(KT_EXT_WIN OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # headers only, no C++20 modules

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
