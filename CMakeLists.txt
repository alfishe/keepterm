cmake_minimum_required(VERSION 3.28)

project(keepterm
    VERSION 0.1.0
    DESCRIPTION "Cross-platform terminal with projects, persistent sessions and per-session resource accounting"
    LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(KeeptermOptions)
include(KeeptermFlags)
include(KeeptermWarnings)

add_subdirectory(src)
include(KeeptermPackaging)

if(KT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    include(KeeptermChecks)
endif()
