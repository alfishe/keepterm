# Shader outputs. The build always compiles against the committed headers in
# shaders/gen/ (SPIR-V, DXBC, metallib, each with a content hash); tools/gen_shaders.py
# regenerates them from quad.hlsl. KT_SHADER_TOOLS selects how the build treats the tools:
#   auto      a `shaders_up_to_date` check test compares the committed outputs with what the
#             tools found on this machine produce; steps without a tool are skipped.
#   required  the same test, but a missing tool for this OS fails it (CI).
#   prebuilt  no tools are looked for; the committed outputs are used as they are.
set(KT_SHADER_TOOLS "auto" CACHE STRING "Shader compilers: auto, required or prebuilt")
set_property(CACHE KT_SHADER_TOOLS PROPERTY STRINGS auto required prebuilt)

if(NOT KT_SHADER_TOOLS STREQUAL "prebuilt")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_custom_target(kt_regen_shaders
            COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/gen_shaders.py
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Regenerating src/render/shaders/gen"
            VERBATIM)
        set(KT_SHADER_CHECK_COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/gen_shaders.py --check)
        if(KT_SHADER_TOOLS STREQUAL "required")
            list(APPEND KT_SHADER_CHECK_COMMAND --require)
        endif()
        set(KT_SHADER_CHECK_COMMAND "${KT_SHADER_CHECK_COMMAND}" CACHE INTERNAL "")
    elseif(KT_SHADER_TOOLS STREQUAL "required")
        message(FATAL_ERROR "KT_SHADER_TOOLS=required needs Python 3 for tools/gen_shaders.py")
    endif()
endif()
