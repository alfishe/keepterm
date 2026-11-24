// Font discovery (`IFontSource`).
//
// The interface is OS-free and part of the text core. The implementations live in the
// `kt_text_platform` target (font_source_{macos,linux,win}.cpp), which may call OS APIs:
//   - macOS: CoreText (CTFontDescriptor matching, CTFontCreateForString for fallback);
//   - Linux: fontconfig loaded with dlopen (never linked), else a scan of the XDG font
//     directories (DirectoryFontIndex below);
//   - Windows: DirectWrite loaded at run time (system collection, IDWriteFontFallback), else a
//     scan of the Windows font folders.
// IFontSource belongs in platform_core; until that module exists it lives here.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "text/font.h"

namespace kt::text {

struct FontFamilyInfo {
    std::string name;
    bool monospace = false;
};

struct FontQuery {
    std::string family;             // empty = the OS default monospace family
    Style style = Style::regular;
    // CSS weight 100-900 (font.weight, font.bold_weight). 0: 400, or 700 for bold styles, with
    // the strict style check below. Set: the face of the family nearest to it with the
    // requested slant; nothing when the nearest face is more than 150 away.
    uint16_t weight = 0;
};

// Monospace families of a source, sorted case-insensitively, without duplicates (Settings →
// Appearance, `keepterm --font list`).
std::vector<std::string> monospace_families(IFontSource&);

// The CSS weight of a font.weight value ("thin" 100 ... "bold" 700); 400 for unknown names.
uint16_t css_weight(std::string_view name);

class IFontSource {
public:
    virtual ~IFontSource() = default;
    virtual std::vector<FontFamilyInfo> families() = 0;
    // The face of `family` in `style`. Returns nothing when the family is not installed, and
    // nothing for bold / italic when the family has no such face (the caller synthesizes).
    virtual std::optional<FontFileRef> match(const FontQuery&) = 0;
    // Faces that have a glyph for `cp`, best first. `base` is the primary font of the chain;
    // `lang` a BCP 47 tag for Han unification ("ja", "zh-Hans", ...) or empty.
    virtual std::vector<FontFileRef> fallback_for(char32_t cp, const FontQuery& base, std::string_view lang) = 0;
    // The OS default monospace family: Cascadia Mono or Consolas, SF Mono when
    // registered or Menlo, fontconfig `monospace`.
    virtual std::string default_monospace() = 0;
};

// The source of this OS (kt_text_platform). Never null: without any system service it returns
// a source backed by DirectoryFontIndex.
std::unique_ptr<IFontSource> create_font_source();

// Resolves a spec to files: the first installed family of spec.families, else the OS default
// monospace; its bold / italic faces when installed; spec.fallback families as the user
// fallback list. Families that were not found are appended to `missing` (for the missing-font
// notice). Fails only when not even the default monospace family resolves.
Result<FontFiles> resolve_font_files(IFontSource&, const FontSpec&, std::vector<std::string>* missing = nullptr);

// ---------------------------------------------------------------- portable helpers

// Index of the face whose PostScript name is `postscript` inside a font file (collections such
// as Menlo.ttc), or 0 when not found or the file is not a collection.
uint32_t face_index_for_postscript(const std::string& path, std::string_view postscript);

// Case-insensitive ASCII comparison of family names.
bool family_equal(std::string_view a, std::string_view b);

// A font index built by scanning directories for .ttf, .otf, .ttc and .otc files with FreeType
// (the fallback without fontconfig / DirectWrite). Coverage is kept per face as
// one bit per 256-code-point block; fallback queries confirm the exact code point.
// Not cached on disk yet: the scan runs once per process, on first use.
class DirectoryFontIndex final : public IFontSource {
public:
    struct Entry {
        std::string path;
        uint32_t index = 0;
        std::string family;
        std::string style_name;
        bool bold = false, italic = false, monospace = false, scalable = true;
        uint16_t weight = 400;          // OS/2 usWeightClass (CSS scale)
        std::vector<uint64_t> blocks;   // 0x110000 / 256 bits
    };

    // `dirs` are scanned recursively. `default_families` are tried in order by
    // default_monospace() before any monospace family found in the scan.
    DirectoryFontIndex(std::vector<std::string> dirs, std::vector<std::string> default_families);
    ~DirectoryFontIndex() override;

    std::vector<FontFamilyInfo> families() override;
    std::optional<FontFileRef> match(const FontQuery&) override;
    std::vector<FontFileRef> fallback_for(char32_t cp, const FontQuery& base, std::string_view lang) override;
    std::string default_monospace() override;

    const std::vector<Entry>& entries();

    // The standard font directories of this OS: XDG data dirs + /usr/share/fonts on Linux,
    // the system and user Fonts folders on macOS; Windows passes its own.
    static std::vector<std::string> standard_dirs();

private:
    void scan();
    bool scanned_ = false;
    std::vector<std::string> dirs_;
    std::vector<std::string> defaults_;
    std::vector<Entry> entries_;
};

}  // namespace kt::text
