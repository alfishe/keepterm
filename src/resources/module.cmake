# kt_resources: the resource monitor core: process snapshot per OS, attribution of
# processes to sessions, aggregation, the sampler and the headless `keepterm --resources` report.
# containment.h stays header-only for kt_pty and kt_session.

set(kt_res_dir ${CMAKE_CURRENT_LIST_DIR})
set(kt_res_sources
    ${kt_res_dir}/attribution.cpp
    ${kt_res_dir}/collector.cpp
    ${kt_res_dir}/procfs_parse.cpp
    ${kt_res_dir}/report.cpp
    ${kt_res_dir}/cli.cpp)
if(WIN32)
    list(APPEND kt_res_sources ${kt_res_dir}/source_win.cpp)
elseif(APPLE)
    list(APPEND kt_res_sources ${kt_res_dir}/source_macos.cpp ${kt_res_dir}/platform_posix.cpp)
else()
    list(APPEND kt_res_sources ${kt_res_dir}/source_linux.cpp ${kt_res_dir}/platform_posix.cpp)
endif()
kt_module(kt_resources ${kt_res_sources})
target_link_libraries(kt_resources PUBLIC kt_base)
if(NOT WIN32 AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(kt_resources PUBLIC Threads::Threads)
endif()

# `keepterm --resources` (src/app/main.cpp).
target_link_libraries(keepterm PRIVATE kt_resources)
target_compile_definitions(keepterm PRIVATE KT_HAVE_RESOURCES=1)
