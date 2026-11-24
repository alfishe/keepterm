# kt_text and the vendored libraries it needs.
#
# FreeType and HarfBuzz are built from the pinned sources in third_party/ as static
# libraries. They do not get kt_set_warnings: their warnings must never fail a
# KT_WERROR build, so they are compiled with warnings off, and their headers are
# SYSTEM includes for our targets.

set(kt_tp "${PROJECT_SOURCE_DIR}/third_party")

function(kt_vendored_quiet target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /w)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
endfunction()

# ---------------------------------------------------------------- FreeType
# Module list and options: third_party/freetype/kt_config (shadows the upstream
# ftoption.h, ftmodule.h and mac-support.h). No zlib, bzip2, PNG, Brotli, HarfBuzz.
set(kt_ft "${kt_tp}/freetype")
add_library(kt_freetype STATIC
    ${kt_ft}/src/base/ftsystem.c
    ${kt_ft}/src/base/ftinit.c
    ${kt_ft}/src/base/ftdebug.c
    ${kt_ft}/src/base/ftbase.c
    ${kt_ft}/src/base/ftbbox.c
    ${kt_ft}/src/base/ftbitmap.c
    ${kt_ft}/src/base/ftglyph.c
    ${kt_ft}/src/base/ftmm.c
    ${kt_ft}/src/base/ftsynth.c
    ${kt_ft}/src/base/ftfstype.c
    ${kt_ft}/src/base/ftgasp.c
    ${kt_ft}/src/autofit/autofit.c
    ${kt_ft}/src/truetype/truetype.c
    ${kt_ft}/src/sfnt/sfnt.c
    ${kt_ft}/src/cff/cff.c
    ${kt_ft}/src/psaux/psaux.c
    ${kt_ft}/src/pshinter/pshinter.c
    ${kt_ft}/src/psnames/psnames.c
    ${kt_ft}/src/smooth/smooth.c)
target_include_directories(kt_freetype SYSTEM PUBLIC ${kt_ft}/kt_config ${kt_ft}/include)
target_compile_definitions(kt_freetype PRIVATE FT2_BUILD_LIBRARY)
set_target_properties(kt_freetype PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
kt_vendored_quiet(kt_freetype)

# ---------------------------------------------------------------- HarfBuzz
# One amalgamated translation unit, FreeType integration (hb-ft), no ICU, no glib,
# no platform shapers. Feature set: the HB_LEAN list of hb-config.hh minus HB_NO_VAR,
# so variable fonts keep their feature variations, and minus HB_NO_OPEN: hb-face.cc in
# 10.1.0 calls hb_blob_create_from_file_or_fail unguarded, which MSVC cannot leave unresolved.
set(kt_hb "${kt_tp}/harfbuzz")
add_library(kt_harfbuzz STATIC ${kt_hb}/src/harfbuzz.cc)
target_include_directories(kt_harfbuzz SYSTEM PUBLIC ${kt_hb}/src)
target_link_libraries(kt_harfbuzz PUBLIC kt_freetype)
target_compile_definitions(kt_harfbuzz PRIVATE
    HAVE_FREETYPE=1
    HAVE_FT_GET_VAR_BLEND_COORDINATES=1
    HAVE_FT_SET_VAR_BLEND_COORDINATES=1
    HAVE_FT_DONE_MM_VAR=1
    HAVE_FT_GET_TRANSFORM=1
    HB_DISABLE_DEPRECATED
    HB_NO_ATEXIT
    HB_NO_BUFFER_MESSAGE
    HB_NO_BUFFER_SERIALIZE
    HB_NO_BUFFER_VERIFY
    HB_NO_BITMAP
    HB_NO_CFF
    HB_NO_COLOR
    HB_NO_DRAW
    HB_NO_ERRNO
    HB_NO_FACE_COLLECT_UNICODES
    HB_NO_GETENV
    HB_NO_HINTING
    HB_NO_LANGUAGE_LONG
    HB_NO_LANGUAGE_PRIVATE_SUBTAG
    HB_NO_LAYOUT_FEATURE_PARAMS
    HB_NO_LAYOUT_COLLECT_GLYPHS
    HB_NO_LAYOUT_RARELY_USED
    HB_NO_LAYOUT_UNUSED
    HB_NO_MATH
    HB_NO_META
    HB_NO_METRICS
    HB_NO_MMAP
    HB_NO_NAME
    HB_NO_OT_FONT_GLYPH_NAMES
    HB_NO_OT_SHAPE_FRACTIONS
    HB_NO_PAINT
    HB_NO_SETLOCALE
    HB_NO_STYLE
    HB_NO_SUBSET_LAYOUT
    HB_NO_VERTICAL)
kt_vendored_quiet(kt_harfbuzz)

# ---------------------------------------------------------------- kt_text
kt_module(kt_text
    ${CMAKE_CURRENT_LIST_DIR}/atlas.cpp
    ${CMAKE_CURRENT_LIST_DIR}/face.cpp
    ${CMAKE_CURRENT_LIST_DIR}/font.cpp
    ${CMAKE_CURRENT_LIST_DIR}/font_source.cpp
    ${CMAKE_CURRENT_LIST_DIR}/procedural.cpp
    ${CMAKE_CURRENT_LIST_DIR}/raster.cpp
    ${CMAKE_CURRENT_LIST_DIR}/terminal_shaper.cpp)
target_link_libraries(kt_text PUBLIC kt_base PRIVATE kt_harfbuzz kt_freetype)
set_target_properties(kt_text PROPERTIES KT_CORE ON)

# ---------------------------------------------------------------- kt_text_platform
# Font discovery per OS: the IFontSource implementations that call OS APIs.
# Not a core target. fontconfig and DirectWrite are loaded at run time, never linked.
if(APPLE)
    kt_module(kt_text_platform ${CMAKE_CURRENT_LIST_DIR}/font_source_macos.cpp)
    target_link_libraries(kt_text_platform PUBLIC kt_text PRIVATE "-framework CoreText" "-framework CoreFoundation")
elseif(WIN32)
    kt_module(kt_text_platform ${CMAKE_CURRENT_LIST_DIR}/font_source_win.cpp)
    target_link_libraries(kt_text_platform PUBLIC kt_text)
else()
    kt_module(kt_text_platform ${CMAKE_CURRENT_LIST_DIR}/font_source_linux.cpp)
    target_link_libraries(kt_text_platform PUBLIC kt_text PRIVATE ${CMAKE_DL_LIBS})
endif()
