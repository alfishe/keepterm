// IFontSource for Linux: fontconfig through dlopen (never linked), else the
// DirectoryFontIndex over the XDG font directories.
//
// fontconfig's types are declared here (opaque pointers and the stable public structs), so the
// build needs no fontconfig headers. FcFontSort results are cached per base pattern (family,
// style, language) and walked until a face has the code point.
#include "text/font_source.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

#ifndef KT_FONTCONFIG_LIBRARY
#define KT_FONTCONFIG_LIBRARY "libfontconfig.so.1"
#endif

namespace kt::text {

namespace {

// ---------------------------------------------------------------- fontconfig ABI subset
using FcChar8 = unsigned char;
using FcChar32 = unsigned int;
using FcBool = int;
struct FcConfig;
struct FcPattern;
struct FcCharSet;
struct FcObjectSet;
struct FcFontSet {
    int nfont;
    int sfont;
    FcPattern** fonts;
};
enum FcResult : int { FcResultMatch = 0, FcResultNoMatch, FcResultTypeMismatch, FcResultNoId, FcResultOutOfMemory };
enum FcMatchKind : int { FcMatchPattern = 0, FcMatchFont, FcMatchScan };

constexpr const char* FC_FAMILY = "family";
constexpr const char* FC_SLANT = "slant";
constexpr const char* FC_WEIGHT = "weight";
constexpr const char* FC_FILE = "file";
constexpr const char* FC_INDEX = "index";
constexpr const char* FC_SPACING = "spacing";
constexpr const char* FC_CHARSET = "charset";
constexpr const char* FC_LANG = "lang";
constexpr const char* FC_OUTLINE = "outline";
constexpr int FC_SLANT_ROMAN = 0;
constexpr int FC_SLANT_ITALIC = 100;
constexpr int FC_WEIGHT_REGULAR = 80;
constexpr int FC_WEIGHT_SEMIBOLD = 180;
constexpr int FC_WEIGHT_BOLD = 200;
constexpr int FC_MONO = 100;

// fontconfig weights against the CSS scale (FcWeightFromOpenType's table), linear in between.
constexpr int kCssW[] = {100, 200, 300, 350, 400, 500, 600, 700, 800, 900};
constexpr int kFcW[] = {0, 40, 50, 75, 80, 100, 180, 200, 205, 210};
int map_weight(int v, const int* from, const int* to) {
    if (v <= from[0]) return to[0];
    for (int i = 1; i < 10; ++i) {
        if (v <= from[i]) return to[i - 1] + (to[i] - to[i - 1]) * (v - from[i - 1]) / (from[i] - from[i - 1]);
    }
    return to[9];
}
int fc_from_css_weight(int css) { return map_weight(css, kCssW, kFcW); }
int css_from_fc_weight(int fc) { return map_weight(fc, kFcW, kCssW); }

struct Fc {
    void* lib = nullptr;
    // Filled by load(); every pointer is non-null once load() returned true.
    FcBool (*Init)() = nullptr;
    FcConfig* (*ConfigGetCurrent)() = nullptr;
    FcPattern* (*PatternCreate)() = nullptr;
    void (*PatternDestroy)(FcPattern*) = nullptr;
    FcBool (*PatternAddString)(FcPattern*, const char*, const FcChar8*) = nullptr;
    FcBool (*PatternAddInteger)(FcPattern*, const char*, int) = nullptr;
    FcBool (*PatternAddBool)(FcPattern*, const char*, FcBool) = nullptr;
    FcResult (*PatternGetString)(const FcPattern*, const char*, int, FcChar8**) = nullptr;
    FcResult (*PatternGetInteger)(const FcPattern*, const char*, int, int*) = nullptr;
    FcResult (*PatternGetBool)(const FcPattern*, const char*, int, FcBool*) = nullptr;
    FcResult (*PatternGetCharSet)(const FcPattern*, const char*, int, FcCharSet**) = nullptr;
    FcBool (*ConfigSubstitute)(FcConfig*, FcPattern*, FcMatchKind) = nullptr;
    void (*DefaultSubstitute)(FcPattern*) = nullptr;
    FcPattern* (*FontMatch)(FcConfig*, FcPattern*, FcResult*) = nullptr;
    FcFontSet* (*FontSort)(FcConfig*, FcPattern*, FcBool, FcCharSet**, FcResult*) = nullptr;
    FcFontSet* (*FontList)(FcConfig*, FcPattern*, FcObjectSet*) = nullptr;
    void (*FontSetDestroy)(FcFontSet*) = nullptr;
    FcBool (*CharSetHasChar)(const FcCharSet*, FcChar32) = nullptr;
    FcObjectSet* (*ObjectSetCreate)() = nullptr;
    FcBool (*ObjectSetAdd)(FcObjectSet*, const char*) = nullptr;
    void (*ObjectSetDestroy)(FcObjectSet*) = nullptr;

    bool load() {
        lib = dlopen(KT_FONTCONFIG_LIBRARY, RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        bool ok = true;
        auto sym = [&](auto& fn, const char* name) {
            void* p = dlsym(lib, name);
            if (!p) ok = false;
            // POSIX guarantees that a pointer from dlsym converts to a function pointer.
            static_assert(sizeof(fn) == sizeof(p));
            std::memcpy(&fn, &p, sizeof p);
        };
        sym(Init, "FcInit");
        sym(ConfigGetCurrent, "FcConfigGetCurrent");
        sym(PatternCreate, "FcPatternCreate");
        sym(PatternDestroy, "FcPatternDestroy");
        sym(PatternAddString, "FcPatternAddString");
        sym(PatternAddInteger, "FcPatternAddInteger");
        sym(PatternAddBool, "FcPatternAddBool");
        sym(PatternGetString, "FcPatternGetString");
        sym(PatternGetInteger, "FcPatternGetInteger");
        sym(PatternGetBool, "FcPatternGetBool");
        sym(PatternGetCharSet, "FcPatternGetCharSet");
        sym(ConfigSubstitute, "FcConfigSubstitute");
        sym(DefaultSubstitute, "FcDefaultSubstitute");
        sym(FontMatch, "FcFontMatch");
        sym(FontSort, "FcFontSort");
        sym(FontList, "FcFontList");
        sym(FontSetDestroy, "FcFontSetDestroy");
        sym(CharSetHasChar, "FcCharSetHasChar");
        sym(ObjectSetCreate, "FcObjectSetCreate");
        sym(ObjectSetAdd, "FcObjectSetAdd");
        sym(ObjectSetDestroy, "FcObjectSetDestroy");
        if (!ok || !Init()) {
            dlclose(lib);
            lib = nullptr;
            return false;
        }
        return true;
    }
};

const FcChar8* u8(const std::string& s) { return reinterpret_cast<const FcChar8*>(s.c_str()); }

class FontconfigSource final : public IFontSource {
public:
    explicit FontconfigSource(Fc fc) : fc_(fc) {}
    ~FontconfigSource() override {
        for (auto& [key, set] : sorts_) {
            if (set) fc_.FontSetDestroy(set);
        }
        // The library stays loaded: FcFini is not safe while other users of fontconfig exist.
    }

    std::vector<FontFamilyInfo> families() override {
        std::vector<FontFamilyInfo> out;
        FcPattern* pat = fc_.PatternCreate();
        FcObjectSet* os = fc_.ObjectSetCreate();
        fc_.ObjectSetAdd(os, FC_FAMILY);
        fc_.ObjectSetAdd(os, FC_SPACING);
        FcFontSet* set = fc_.FontList(nullptr, pat, os);
        fc_.ObjectSetDestroy(os);
        fc_.PatternDestroy(pat);
        if (!set) return out;
        for (int i = 0; i < set->nfont; ++i) {
            FcChar8* fam = nullptr;
            if (fc_.PatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch || !fam) continue;
            int spacing = 0;
            const bool mono = fc_.PatternGetInteger(set->fonts[i], FC_SPACING, 0, &spacing) == FcResultMatch && spacing >= FC_MONO;
            const std::string name = reinterpret_cast<const char*>(fam);
            auto it = std::find_if(out.begin(), out.end(), [&](const FontFamilyInfo& f) { return f.name == name; });
            if (it == out.end()) out.push_back(FontFamilyInfo{name, mono});
            else it->monospace = it->monospace || mono;
        }
        fc_.FontSetDestroy(set);
        std::sort(out.begin(), out.end(), [](const FontFamilyInfo& a, const FontFamilyInfo& b) { return a.name < b.name; });
        return out;
    }

    std::optional<FontFileRef> match(const FontQuery& q) override {
        const std::string family = q.family.empty() ? std::string("monospace") : q.family;
        const bool generic = family_equal(family, "monospace");
        FcPattern* pat = base_pattern(family, q.style, {}, q.weight);
        fc_.ConfigSubstitute(nullptr, pat, FcMatchPattern);
        fc_.DefaultSubstitute(pat);
        FcResult res = FcResultNoMatch;
        FcPattern* m = fc_.FontMatch(nullptr, pat, &res);
        fc_.PatternDestroy(pat);
        if (!m) return std::nullopt;
        std::optional<FontFileRef> out;
        // FcFontMatch always returns the closest font: accept it only for the requested family
        // (any of its names) and, for bold / italic, only when the face really has that style.
        const bool want_bold = q.style == Style::bold || q.style == Style::bold_italic;
        const bool want_italic = q.style == Style::italic || q.style == Style::bold_italic;
        int weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN;
        fc_.PatternGetInteger(m, FC_WEIGHT, 0, &weight);
        fc_.PatternGetInteger(m, FC_SLANT, 0, &slant);
        bool style_ok = (want_bold == (weight >= FC_WEIGHT_SEMIBOLD)) && (want_italic == (slant != FC_SLANT_ROMAN));
        if (q.weight != 0)   // font.weight: the nearest face within 150 on the CSS scale
            style_ok = std::abs(css_from_fc_weight(weight) - int{q.weight}) <= 150 && (want_italic == (slant != FC_SLANT_ROMAN));
        if ((generic || has_family(m, family)) && style_ok && scalable(m)) out = file_of(m);
        fc_.PatternDestroy(m);
        return out;
    }

    std::vector<FontFileRef> fallback_for(char32_t cp, const FontQuery& base, std::string_view lang) override {
        std::vector<FontFileRef> out;
        FcFontSet* set = sorted(base, lang);
        if (!set) return out;
        for (int i = 0; i < set->nfont && out.size() < 3; ++i) {
            FcPattern* p = set->fonts[i];
            FcCharSet* cs = nullptr;
            if (fc_.PatternGetCharSet(p, FC_CHARSET, 0, &cs) != FcResultMatch || !cs) continue;
            if (!fc_.CharSetHasChar(cs, static_cast<FcChar32>(cp)) || !scalable(p)) continue;
            if (has_family(p, "LastResort")) continue;   // keepterm draws its own tofu
            if (auto f = file_of(p)) out.push_back(std::move(*f));
        }
        return out;
    }

    std::string default_monospace() override {
        if (!default_.empty()) return default_;
        FcPattern* pat = base_pattern("monospace", Style::regular, {});
        fc_.ConfigSubstitute(nullptr, pat, FcMatchPattern);
        fc_.DefaultSubstitute(pat);
        FcResult res = FcResultNoMatch;
        FcPattern* m = fc_.FontMatch(nullptr, pat, &res);
        fc_.PatternDestroy(pat);
        if (m) {
            FcChar8* fam = nullptr;
            if (fc_.PatternGetString(m, FC_FAMILY, 0, &fam) == FcResultMatch && fam) default_ = reinterpret_cast<const char*>(fam);
            fc_.PatternDestroy(m);
        }
        if (default_.empty()) default_ = "monospace";
        return default_;
    }

private:
    FcPattern* base_pattern(const std::string& family, Style style, std::string_view lang, uint16_t css_weight = 0) {
        FcPattern* pat = fc_.PatternCreate();
        fc_.PatternAddString(pat, FC_FAMILY, u8(family));
        const bool bold = style == Style::bold || style == Style::bold_italic;
        const bool italic = style == Style::italic || style == Style::bold_italic;
        fc_.PatternAddInteger(pat, FC_WEIGHT,
                              css_weight ? fc_from_css_weight(css_weight) : bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
        fc_.PatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        if (!lang.empty()) fc_.PatternAddString(pat, FC_LANG, u8(std::string(lang)));
        return pat;
    }

    FcFontSet* sorted(const FontQuery& base, std::string_view lang) {
        auto key = std::make_tuple(base.family, static_cast<int>(base.style), std::string(lang));
        if (auto it = sorts_.find(key); it != sorts_.end()) return it->second;
        const std::string family = base.family.empty() ? std::string("monospace") : base.family;
        FcPattern* pat = base_pattern(family, base.style, lang);
        fc_.ConfigSubstitute(nullptr, pat, FcMatchPattern);
        fc_.DefaultSubstitute(pat);
        FcResult res = FcResultNoMatch;
        FcFontSet* set = fc_.FontSort(nullptr, pat, /*trim=*/1, nullptr, &res);
        fc_.PatternDestroy(pat);
        sorts_.emplace(std::move(key), set);
        return set;
    }

    bool has_family(FcPattern* p, const std::string& family) {
        for (int i = 0; i < 16; ++i) {
            FcChar8* fam = nullptr;
            if (fc_.PatternGetString(p, FC_FAMILY, i, &fam) != FcResultMatch || !fam) break;
            if (family_equal(reinterpret_cast<const char*>(fam), family)) return true;
        }
        return false;
    }

    // Outline fonts only: FreeType here has no PNG (CBDT emoji) and no bitmap-only fonts.
    bool scalable(FcPattern* p) {
        FcBool outline = 1;
        if (fc_.PatternGetBool(p, FC_OUTLINE, 0, &outline) == FcResultMatch && !outline) return false;
        return true;
    }

    std::optional<FontFileRef> file_of(FcPattern* p) {
        FcChar8* file = nullptr;
        if (fc_.PatternGetString(p, FC_FILE, 0, &file) != FcResultMatch || !file) return std::nullopt;
        int index = 0;
        fc_.PatternGetInteger(p, FC_INDEX, 0, &index);
        FontFileRef ref;
        ref.path = reinterpret_cast<const char*>(file);
        // Named instances of variable fonts carry the instance in the high 16 bits.
        ref.face_index = static_cast<uint32_t>(index) & 0xFFFFu;
        return ref;
    }

    Fc fc_;
    std::map<std::tuple<std::string, int, std::string>, FcFontSet*> sorts_;
    std::string default_;
};

}  // namespace

std::unique_ptr<IFontSource> create_font_source() {
    Fc fc;
    if (fc.load()) return std::make_unique<FontconfigSource>(fc);
    // No fontconfig: scan the standard directories; common monospace families first.
    return std::make_unique<DirectoryFontIndex>(
        DirectoryFontIndex::standard_dirs(),
        std::vector<std::string>{"DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "Ubuntu Mono",
                                 "Cascadia Mono", "JetBrains Mono", "Source Code Pro", "Fira Mono", "Hack"});
}

}  // namespace kt::text
