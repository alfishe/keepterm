/*
 * keepterm build configuration for FreeType (not part of upstream).
 *
 * Shadows <freetype/config/mac-support.h>, which defines FT_MACINTOSH on Apple
 * platforms and pulls in the Carbon resource-fork font support of src/base/ftmac.c.
 * keepterm opens fonts from memory only, so FT_MACINTOSH stays undefined and the
 * macOS build does not depend on CoreServices. The include guard matches upstream.
 */
#ifndef FREETYPE_CONFIG_MAC_SUPPORT_H_
#define FREETYPE_CONFIG_MAC_SUPPORT_H_
#endif
