/*
 * keepterm build configuration for FreeType (not part of upstream).
 *
 * Shadows <freetype/config/ftoption.h>: includes the upstream file unchanged and
 * switches off what keepterm does not build (tdd/03 §3.2.3, tdd/12 §3.3):
 * no zlib/LZW (no gzip module, no WOFF), no bzip2, no PNG, no Brotli, no HarfBuzz
 * (avoids the FreeType <-> HarfBuzz cycle), no OT-SVG, no Mac resource forks, and no
 * FREETYPE_PROPERTIES environment variable (rendering must not depend on the environment).
 */
#ifndef KT_FTOPTION_H_
#define KT_FTOPTION_H_

#include "../../../include/freetype/config/ftoption.h"

#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_SYSTEM_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_SVG
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK

#endif /* KT_FTOPTION_H_ */
