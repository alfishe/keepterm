// IFontSource for macOS: CoreText.
//
// Matching uses CTFontDescriptorCreateMatchingFontDescriptors with the family as a mandatory
// attribute; fallback uses CTFontCreateForString(WithLanguage). File paths come from
// kCTFontURLAttribute; the face index inside a collection (Menlo.ttc) from the PostScript name.
// Fonts without a file URL (activated in memory by other apps) are skipped, and so is
// LastResort: keepterm draws its own tofu box.
#include "text/font_source.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <cmath>
#include <limits>

namespace kt::text {

namespace {

constexpr size_t kMaxCandidates = 3;

// Owns one CoreFoundation reference.
template <class T>
class Cf {
public:
    Cf() = default;
    explicit Cf(T ref) : ref_(ref) {}
    ~Cf() {
        if (ref_) CFRelease(ref_);
    }
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;
    Cf(Cf&& o) noexcept : ref_(o.ref_) { o.ref_ = nullptr; }
    Cf& operator=(Cf&& o) noexcept {
        if (this != &o) {
            if (ref_) CFRelease(ref_);
            ref_ = o.ref_;
            o.ref_ = nullptr;
        }
        return *this;
    }
    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    T ref_ = nullptr;
};

Cf<CFStringRef> cf_string(std::string_view s) {
    return Cf<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()),
                                                   static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false));
}

std::string to_utf8(CFStringRef s) {
    if (!s) return {};
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

bool is_collection(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return ext == ".ttc" || ext == ".otc";
}

std::optional<FontFileRef> ref_from_descriptor(CTFontDescriptorRef d) {
    Cf<CFURLRef> url(static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(d, kCTFontURLAttribute)));
    if (!url) return std::nullopt;
    UInt8 buf[4096];
    if (!CFURLGetFileSystemRepresentation(url.get(), true, buf, sizeof buf)) return std::nullopt;
    FontFileRef ref;
    ref.path = reinterpret_cast<const char*>(buf);
    if (is_collection(ref.path)) {
        Cf<CFStringRef> ps(static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute)));
        ref.face_index = face_index_for_postscript(ref.path, to_utf8(ps.get()));
    }
    return ref;
}

struct Traits {
    uint32_t symbolic = 0;
    double weight = 0, width = 0;
};

Traits traits_of(CTFontDescriptorRef d) {
    Traits t;
    Cf<CFDictionaryRef> dict(static_cast<CFDictionaryRef>(CTFontDescriptorCopyAttribute(d, kCTFontTraitsAttribute)));
    if (!dict) return t;
    if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(dict.get(), kCTFontSymbolicTrait))) {
        int64_t v = 0;
        CFNumberGetValue(n, kCFNumberSInt64Type, &v);
        t.symbolic = static_cast<uint32_t>(v);
    }
    if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(dict.get(), kCTFontWeightTrait)))
        CFNumberGetValue(n, kCFNumberDoubleType, &t.weight);
    if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(dict.get(), kCTFontWidthTrait)))
        CFNumberGetValue(n, kCFNumberDoubleType, &t.width);
    return t;
}

// kCTFontWeightTrait (-1..1) to the CSS scale, through the values of the NSFontWeight constants.
double css_from_ct_weight(double w) {
    static constexpr double kCt[] = {-0.8, -0.6, -0.4, 0.0, 0.23, 0.3, 0.4, 0.56, 0.62};
    if (w <= kCt[0]) return 100;
    for (int i = 1; i < 9; ++i) {
        if (w <= kCt[i]) return 100.0 * i + 100.0 * (w - kCt[i - 1]) / (kCt[i] - kCt[i - 1]);
    }
    return 900;
}

Cf<CTFontDescriptorRef> family_descriptor(std::string_view family) {
    Cf<CFStringRef> name = cf_string(family);
    if (!name) return Cf<CTFontDescriptorRef>();
    const void* keys[] = {kCTFontFamilyNameAttribute};
    const void* values[] = {name.get()};
    Cf<CFDictionaryRef> attrs(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks));
    return Cf<CTFontDescriptorRef>(CTFontDescriptorCreateWithAttributes(attrs.get()));
}

// Every face of a family, or null when the family is not registered.
Cf<CFArrayRef> family_faces(std::string_view family) {
    Cf<CTFontDescriptorRef> desc = family_descriptor(family);
    if (!desc) return Cf<CFArrayRef>();
    const void* mandatory_keys[] = {kCTFontFamilyNameAttribute};
    Cf<CFSetRef> mandatory(CFSetCreate(kCFAllocatorDefault, mandatory_keys, 1, &kCFTypeSetCallBacks));
    return Cf<CFArrayRef>(CTFontDescriptorCreateMatchingFontDescriptors(desc.get(), mandatory.get()));
}

class CoreTextFontSource final : public IFontSource {
public:
    std::vector<FontFamilyInfo> families() override {
        std::vector<FontFamilyInfo> out;
        Cf<CFArrayRef> names(CTFontManagerCopyAvailableFontFamilyNames());
        if (!names) return out;
        const CFIndex n = CFArrayGetCount(names.get());
        for (CFIndex i = 0; i < n; ++i) {
            auto name = static_cast<CFStringRef>(CFArrayGetValueAtIndex(names.get(), i));
            FontFamilyInfo info;
            info.name = to_utf8(name);
            if (info.name.empty() || info.name[0] == '.') continue;   // hidden system families
            Cf<CFArrayRef> faces = family_faces(info.name);
            if (faces && CFArrayGetCount(faces.get()) > 0) {
                auto d = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(faces.get(), 0));
                info.monospace = (traits_of(d).symbolic & kCTFontMonoSpaceTrait) != 0;
            }
            out.push_back(std::move(info));
        }
        return out;
    }

    std::optional<FontFileRef> match(const FontQuery& q) override {
        const std::string family = q.family.empty() ? default_monospace() : q.family;
        Cf<CFArrayRef> faces = family_faces(family);
        if (!faces || CFArrayGetCount(faces.get()) == 0) {
            // CoreText compares family names exactly; accept any letter case like the other OSes.
            const std::string canonical = canonical_family(family);
            if (canonical.empty() || canonical == family) return std::nullopt;
            return match(FontQuery{canonical, q.style, q.weight});
        }
        const bool want_bold = q.style == Style::bold || q.style == Style::bold_italic;
        const bool want_italic = q.style == Style::italic || q.style == Style::bold_italic;
        const double target_weight = want_bold ? 0.4 : 0.0;
        CTFontDescriptorRef best = nullptr;
        double best_score = std::numeric_limits<double>::max();
        const CFIndex n = CFArrayGetCount(faces.get());
        for (CFIndex i = 0; i < n; ++i) {
            auto d = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(faces.get(), i));
            const Traits t = traits_of(d);
            const bool bold = (t.symbolic & kCTFontBoldTrait) != 0;
            const bool italic = (t.symbolic & kCTFontItalicTrait) != 0;
            if (q.weight != 0) {
                // font.weight: the nearest face by CSS weight with the requested slant.
                if (italic != want_italic) continue;
                const double dist = std::fabs(css_from_ct_weight(t.weight) - q.weight);
                if (dist > 150) continue;
                const double score = dist + 100 * std::fabs(t.width);
                if (score < best_score) {
                    best_score = score;
                    best = d;
                }
                continue;
            }
            if (bold != want_bold || italic != want_italic) continue;
            const double score = std::fabs(t.weight - target_weight) + std::fabs(t.width);
            if (score < best_score) {
                best_score = score;
                best = d;
            }
        }
        if (!best) return std::nullopt;
        return ref_from_descriptor(best);
    }

    std::vector<FontFileRef> fallback_for(char32_t cp, const FontQuery& base, std::string_view lang) override {
        std::vector<FontFileRef> out;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return out;
        const std::string family = base.family.empty() ? default_monospace() : base.family;
        Cf<CTFontDescriptorRef> desc = family_descriptor(family);
        if (!desc) return out;
        Cf<CTFontRef> font(CTFontCreateWithFontDescriptor(desc.get(), 12.0, nullptr));
        if (!font) return out;

        UniChar chars[2];
        CFIndex len = 1;
        if (cp >= 0x10000) {
            const uint32_t v = static_cast<uint32_t>(cp) - 0x10000u;
            chars[0] = static_cast<UniChar>(0xD800u + (v >> 10));
            chars[1] = static_cast<UniChar>(0xDC00u + (v & 0x3FFu));
            len = 2;
        } else {
            chars[0] = static_cast<UniChar>(cp);
        }
        Cf<CFStringRef> str(CFStringCreateWithCharacters(kCFAllocatorDefault, chars, len));
        if (!str) return out;
        Cf<CFStringRef> cflang = cf_string(lang);
        const CFRange range = CFRangeMake(0, len);
        Cf<CTFontRef> fb(lang.empty() || !cflang
                             ? CTFontCreateForString(font.get(), str.get(), range)
                             : CTFontCreateForStringWithLanguage(font.get(), str.get(), range, cflang.get()));
        // The best font first, then the cascade list: the best one may be a file FreeType cannot
        // read (PingFangUI.ttc) or a bitmap-only color font, so a few candidates are returned.
        auto add = [&](CTFontRef f) {
            if (!f || out.size() >= kMaxCandidates) return;
            Cf<CFStringRef> fam(CTFontCopyFamilyName(f));
            const std::string fam_name = to_utf8(fam.get());
            if (fam_name == "LastResort" || fam_name == ".LastResort") return;
            CGGlyph glyphs[2] = {0, 0};
            if (!CTFontGetGlyphsForCharacters(f, chars, glyphs, len)) return;
            Cf<CTFontDescriptorRef> fd(CTFontCopyFontDescriptor(f));
            if (!fd) return;
            auto ref = ref_from_descriptor(fd.get());
            if (!ref) return;
            for (const FontFileRef& r : out) {
                if (r.path == ref->path && r.face_index == ref->face_index) return;
            }
            out.push_back(std::move(*ref));
        };
        add(fb.get());
        Cf<CFArrayRef> langs;
        if (cflang && !lang.empty()) {
            const void* values[] = {cflang.get()};
            langs = Cf<CFArrayRef>(CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks));
        }
        Cf<CFArrayRef> cascade(CTFontCopyDefaultCascadeListForLanguages(font.get(), langs.get()));
        const CFIndex n = cascade ? CFArrayGetCount(cascade.get()) : 0;
        for (CFIndex i = 0; i < n && out.size() < kMaxCandidates; ++i) {
            auto d = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(cascade.get(), i));
            Cf<CTFontRef> f(CTFontCreateWithFontDescriptor(d, 12.0, nullptr));
            add(f.get());
        }
        return out;
    }

    std::string default_monospace() override {
        // SF Mono only when the user registered it; the copy inside Terminal.app is not used.
        if (!cached_default_.empty()) return cached_default_;
        Cf<CFArrayRef> sf = family_faces("SF Mono");
        cached_default_ = (sf && CFArrayGetCount(sf.get()) > 0) ? "SF Mono" : "Menlo";
        return cached_default_;
    }

private:
    std::string canonical_family(const std::string& family) {
        Cf<CFArrayRef> names(CTFontManagerCopyAvailableFontFamilyNames());
        if (!names) return {};
        const CFIndex n = CFArrayGetCount(names.get());
        for (CFIndex i = 0; i < n; ++i) {
            const std::string name = to_utf8(static_cast<CFStringRef>(CFArrayGetValueAtIndex(names.get(), i)));
            if (family_equal(name, family)) return name;
        }
        return {};
    }

    std::string cached_default_;
};

}  // namespace

std::unique_ptr<IFontSource> create_font_source() { return std::make_unique<CoreTextFontSource>(); }

}  // namespace kt::text
