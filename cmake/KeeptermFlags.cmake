# Per-toolchain flags, OS API baselines and static runtimes.

if(WIN32)
    # Layer 1 of the API baseline: headers of build 14393.
    add_compile_definitions(
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000002
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE)
    # Static CRT.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(MSVC)
        add_compile_options(/permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /GS /Gy /Gw)
        add_link_options(/OPT:REF /OPT:ICF)
    endif()
elseif(APPLE)
    # Layer 5: macOS 13 minimum.
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS" FORCE)
    endif()
    add_compile_options(-Werror=unguarded-availability -Werror=unguarded-availability-new)
    add_link_options(-Wl,-dead_strip)
else()
    # Linux: static libstdc++ and libgcc, glibc 2.28 baseline in release containers.
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,--as-needed)
endif()

if(NOT MSVC)
    add_compile_options(-fvisibility=hidden)
endif()

if(KT_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT kt_ipo_ok OUTPUT kt_ipo_msg)
    if(kt_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "LTO not supported: ${kt_ipo_msg}")
    endif()
endif()

if(KT_SANITIZE)
    if(MSVC)
        # The instrumentation adds sections: large units (main_window.cpp) exceed the COFF limit.
        add_compile_options(/fsanitize=address /bigobj)
        # The ASan runtime DLL sits by cl.exe, found through the PATH of a developer prompt only:
        # processes started apart from it (the session host) fail with STATUS_DLL_NOT_FOUND.
        # Each directory of executables gets a copy.
        get_filename_component(kt_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB kt_asan_dlls "${kt_cl_dir}/clang_rt.asan*_dynamic-x86_64.dll")
        if(NOT kt_asan_dlls)
            message(WARNING "ASan runtime DLL not found next to ${CMAKE_CXX_COMPILER}")
        endif()
        foreach(kt_dir "" src)
            file(COPY ${kt_asan_dlls} DESTINATION "${CMAKE_BINARY_DIR}/${kt_dir}")
        endforeach()
    else()
        string(REPLACE ";" "," kt_san "${KT_SANITIZE}")
        add_compile_options(-fsanitize=${kt_san} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${kt_san})
    endif()
endif()

if(KT_FAULTS)
    add_compile_definitions(KT_FAULTS=1)
endif()
