# kt_config and the vendored toml++ it parses with.
#
# toml++ is compiled once, with warnings off, in kt_tomlpp (TOML_HEADER_ONLY=0); its header
# is a SYSTEM include for our targets, so KT_WERROR never sees its warnings.

set(kt_toml_dir "${PROJECT_SOURCE_DIR}/third_party/tomlplusplus")
add_library(kt_tomlpp STATIC ${CMAKE_CURRENT_LIST_DIR}/tomlpp_impl.cpp)
target_include_directories(kt_tomlpp SYSTEM PUBLIC ${kt_toml_dir})
target_compile_definitions(kt_tomlpp PUBLIC TOML_HEADER_ONLY=0 TOML_EXCEPTIONS=0 TOML_ENABLE_FORMATTERS=1)
if(MSVC)
    target_compile_options(kt_tomlpp PRIVATE /w)
else()
    target_compile_options(kt_tomlpp PRIVATE -w)
endif()

kt_module(kt_config
    config/schema.cpp
    config/config.cpp
    config/dump.cpp
    config/toml_reader.cpp
    config/service.cpp
    config/cli.cpp)
# Settings writes: the format-preserving editor, the writer and its atomic file replacement.
target_sources(kt_config PRIVATE config/toml_edit.cpp config/writer.cpp config/write_file.cpp)
target_link_libraries(kt_config PUBLIC kt_base kt_platform_core PRIVATE kt_tomlpp)

target_link_libraries(keepterm PRIVATE kt_config)
