# kt_store and the vendored libraries it needs: zstd, SQLite, PCRE2.
#
# The vendored code is compiled with warnings off and its headers are SYSTEM includes, so
# KT_WERROR never sees their warnings.

set(kt_store_tp "${PROJECT_SOURCE_DIR}/third_party")

function(kt_store_quiet target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /w)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
endfunction()

# ---------------------------------------------------------------- zstd (lib only)
# Single-threaded, no assembly (the x86-64 Huffman decoder in .S is not built), no legacy
# formats, no dictionary builder.
set(kt_zstd "${kt_store_tp}/zstd/lib")
file(GLOB kt_zstd_sources CONFIGURE_DEPENDS
    ${kt_zstd}/common/*.c ${kt_zstd}/compress/*.c ${kt_zstd}/decompress/*.c)
add_library(kt_zstd STATIC ${kt_zstd_sources})
target_include_directories(kt_zstd SYSTEM PUBLIC ${kt_zstd})
target_compile_definitions(kt_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 ZSTD_MULTITHREAD_SUPPORT_DEFAULT=0)
kt_store_quiet(kt_zstd)

# ---------------------------------------------------------------- SQLite (amalgamation)
add_library(kt_sqlite STATIC ${kt_store_tp}/sqlite/sqlite3.c)
target_include_directories(kt_sqlite SYSTEM PUBLIC ${kt_store_tp}/sqlite)
target_compile_definitions(kt_sqlite PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_OMIT_SHARED_CACHE=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS=1)
kt_store_quiet(kt_sqlite)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(kt_sqlite PRIVATE Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(kt_sqlite PRIVATE m)
    endif()
endif()

# ---------------------------------------------------------------- PCRE2 (8-bit, no JIT)
set(kt_pcre2 "${kt_store_tp}/pcre2/src")
add_library(kt_pcre2 STATIC
    ${kt_pcre2}/pcre2_auto_possess.c ${kt_pcre2}/pcre2_chartables.c ${kt_pcre2}/pcre2_chkdint.c
    ${kt_pcre2}/pcre2_compile.c ${kt_pcre2}/pcre2_compile_cgroup.c ${kt_pcre2}/pcre2_compile_class.c
    ${kt_pcre2}/pcre2_config.c ${kt_pcre2}/pcre2_context.c ${kt_pcre2}/pcre2_convert.c
    ${kt_pcre2}/pcre2_dfa_match.c ${kt_pcre2}/pcre2_error.c ${kt_pcre2}/pcre2_extuni.c
    ${kt_pcre2}/pcre2_find_bracket.c ${kt_pcre2}/pcre2_jit_compile.c ${kt_pcre2}/pcre2_maketables.c
    ${kt_pcre2}/pcre2_match.c ${kt_pcre2}/pcre2_match_data.c ${kt_pcre2}/pcre2_match_next.c
    ${kt_pcre2}/pcre2_newline.c ${kt_pcre2}/pcre2_ord2utf.c ${kt_pcre2}/pcre2_pattern_info.c
    ${kt_pcre2}/pcre2_script_run.c ${kt_pcre2}/pcre2_serialize.c ${kt_pcre2}/pcre2_string_utils.c
    ${kt_pcre2}/pcre2_study.c ${kt_pcre2}/pcre2_substitute.c ${kt_pcre2}/pcre2_substring.c
    ${kt_pcre2}/pcre2_tables.c ${kt_pcre2}/pcre2_ucd.c ${kt_pcre2}/pcre2_valid_utf.c
    ${kt_pcre2}/pcre2_xclass.c)
target_include_directories(kt_pcre2 SYSTEM PUBLIC ${kt_pcre2})
target_compile_definitions(kt_pcre2 PUBLIC PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC)
target_compile_definitions(kt_pcre2 PRIVATE HAVE_CONFIG_H SUPPORT_UNICODE SUPPORT_PCRE2_8
    HAVE_STDINT_H=1 HAVE_INTTYPES_H=1 HAVE_STDLIB_H=1 HAVE_STRING_H=1 HAVE_LIMITS_H=1)
kt_store_quiet(kt_pcre2)

# ---------------------------------------------------------------- LZMA SDK (optional codec)
# Off by default: the live-data measurement decides whether
# the cold tier needs it. With it on, `lzma` is a valid storage.cold_codec.
option(KT_STORE_LZMA "Build the LZMA codec of the history store (LZMA SDK, public domain)" OFF)
if(KT_STORE_LZMA)
    set(kt_lzma "${kt_store_tp}/lzma-sdk/C")
    add_library(kt_lzma STATIC ${kt_lzma}/LzmaEnc.c ${kt_lzma}/LzmaDec.c ${kt_lzma}/LzFind.c ${kt_lzma}/LzFindOpt.c
        ${kt_lzma}/CpuArch.c)
    target_include_directories(kt_lzma SYSTEM PUBLIC ${kt_lzma})
    target_compile_definitions(kt_lzma PUBLIC Z7_ST)  # single-threaded: no LzFindMt / Threads
    kt_store_quiet(kt_lzma)
endif()

# ---------------------------------------------------------------- kt_store
if(WIN32)
    set(kt_store_os store/file_io_win.cpp)
else()
    set(kt_store_os store/file_io_posix.cpp)
endif()
kt_module(kt_store
    ${kt_store_os}
    store/file_io.cpp
    store/format.cpp
    store/fold.cpp
    store/trigram.cpp
    store/segment.cpp
    store/catalog.cpp
    store/session_store.cpp
    store/projection.cpp
    store/regex_query.cpp
    store/search.cpp
    store/retention.cpp
    store/usage.cpp
    store/cli.cpp)
target_link_libraries(kt_store PUBLIC kt_base kt_vt PRIVATE kt_session_proto kt_platform_core kt_config
    kt_zstd kt_sqlite kt_pcre2)
target_compile_definitions(kt_store PUBLIC KT_STORE_HAVE_PCRE2=1)
if(KT_STORE_LZMA)
    target_link_libraries(kt_store PRIVATE kt_lzma)
    target_compile_definitions(kt_store PUBLIC KT_STORE_HAVE_LZMA=1)
else()
    target_compile_definitions(kt_store PUBLIC KT_STORE_HAVE_LZMA=0)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(kt_store PUBLIC Threads::Threads)
endif()

target_link_libraries(keepterm PRIVATE kt_store)
target_compile_definitions(keepterm PRIVATE KT_HAVE_STORE=1)
# The session host writes the warm store on its storage thread (session/history_writer.h).
target_sources(kt_session PRIVATE session/history_writer.cpp)
target_link_libraries(kt_session PRIVATE kt_store)
target_compile_definitions(kt_session PRIVATE KT_HAVE_STORE=1)
