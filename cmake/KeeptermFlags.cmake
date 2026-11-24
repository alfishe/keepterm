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
        add_compile_options(/fsanitize=address)
    else()
        string(REPLACE ";" "," kt_san "${KT_SANITIZE}")
        add_compile_options(-fsanitize=${kt_san} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${kt_san})
    endif()
endif()

if(KT_FAULTS)
    add_compile_definitions(KT_FAULTS=1)
endif()
