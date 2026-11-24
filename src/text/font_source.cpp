// Portable parts of font discovery: spec resolution, collection face lookup and the
// directory index. No OS headers: only FreeType and the standard library.
#include "text/font_source.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

namespace kt::text {

namespace {

constexpr std::size_t kBlocks = 0x110000 / 256;
constexpr std::size_t kMaxScannedFiles = 20000;
constexpr std::size_t kMaxFallbackResults = 3;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string env(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) return {};
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

bool font_extension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = lower(c);
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

bool same_ref(const FontFileRef& a, const FontFileRef& b) {
    return a.path == b.path && a.face_index == b.face_index && a.memory == b.memory;
}

struct FtLib {
    FT_Library lib = nullptr;
    FtLib() {
        if (FT_Init_FreeType(&lib)) lib = nullptr;
    }
    ~FtLib() {
        if (lib) FT_Done_FreeType(lib);
    }
    FtLib(const FtLib&) = delete;
    FtLib& operator=(const FtLib&) = delete;
};

struct FtFace {
    FT_Face face = nullptr;
    FtFace(FT_Library lib, const std::string& path, FT_Long index) {
        if (!lib || FT_New_Face(lib, path.c_str(), index, &face)) face = nullptr;
    }
    ~FtFace() {
        if (face) FT_Done_Face(face);
    }
    FtFace(const FtFace&) = delete;
    FtFace& operator=(const FtFace&) = delete;
};

}  // namespace

bool family_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

uint32_t face_index_for_postscript(const std::string& path, std::string_view postscript) {
    if (postscript.empty()) return 0;
    FtLib ft;
    FtFace probe(ft.lib, path, -1);
    if (!probe.face || probe.face->num_faces <= 1) return 0;
    const FT_Long n = std::min<FT_Long>(probe.face->num_faces, 256);
    for (FT_Long i = 0; i < n; ++i) {
        FtFace f(ft.lib, path, i);
        if (!f.face) continue;
        const char* ps = FT_Get_Postscript_Name(f.face);
        if (ps && postscript == ps) return static_cast<uint32_t>(i);
    }
    return 0;
}

Result<FontFiles> resolve_font_files(IFontSource& src, const FontSpec& spec, std::vector<std::string>* missing) {
    FontFiles files;
    // A weight other than the style's own is tried first, then the plain style.
    auto query = [&](const std::string& fam, Style s) -> std::optional<FontFileRef> {
        const bool bold = s == Style::bold || s == Style::bold_italic;
        const uint16_t w = bold ? spec.bold_weight : spec.weight;
        if (w != 0 && w != (bold ? 700 : 400)) {
            if (auto r = src.match(FontQuery{fam, s, w})) return r;
        }
        return src.match(FontQuery{fam, s, 0});
    };
    for (const std::string& fam : spec.families) {
        if (fam.empty()) continue;
        if (auto r = query(fam, Style::regular)) {
            files.family = fam;
            files.regular = *r;
            break;
        }
        if (missing) missing->push_back(fam);
    }
    if (files.family.empty()) {
        const std::string def = src.default_monospace();
        if (def.empty()) return make_error(Errc::unavailable, "no monospace font found");
        auto r = query(def, Style::regular);
        if (!r) return make_error(Errc::unavailable, "default monospace font not found: " + def);
        files.family = def;
        files.regular = *r;
    }
    auto styled = [&](Style s) -> std::optional<FontFileRef> {
        auto r = query(files.family, s);
        if (r && same_ref(*r, files.regular)) return std::nullopt;
        return r;
    };
    files.bold = styled(Style::bold);
    files.italic = styled(Style::italic);
    files.bold_italic = styled(Style::bold_italic);
    if (files.bold_italic && ((files.bold && same_ref(*files.bold_italic, *files.bold)) ||
                              (files.italic && same_ref(*files.bold_italic, *files.italic))))
        files.bold_italic.reset();
    for (const std::string& fam : spec.fallback) {
        if (auto r = src.match(FontQuery{fam, Style::regular, 0})) files.fallback.push_back(*r);
        else if (missing) missing->push_back(fam);
    }
    return files;
}

std::vector<std::string> monospace_families(IFontSource& src) {
    std::vector<std::string> out;
    for (FontFamilyInfo& f : src.families()) {
        if (f.monospace && !f.name.empty()) out.push_back(std::move(f.name));
    }
    auto lower = [](std::string_view s) {
        std::string l(s);
        for (char& c : l) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        return l;
    };
    std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) {
        const std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return family_equal(a, b); }),
              out.end());
    return out;
}

uint16_t css_weight(std::string_view name) {
    if (name == "thin") return 100;
    if (name == "extralight") return 200;
    if (name == "light") return 300;
    if (name == "medium") return 500;
    if (name == "semibold") return 600;
    if (name == "bold") return 700;
    if (name == "extrabold") return 800;
    if (name == "black") return 900;
    return 400;
}

// ---------------------------------------------------------------- DirectoryFontIndex

DirectoryFontIndex::DirectoryFontIndex(std::vector<std::string> dirs, std::vector<std::string> default_families)
    : dirs_(std::move(dirs)), defaults_(std::move(default_families)) {}

DirectoryFontIndex::~DirectoryFontIndex() = default;

std::vector<std::string> DirectoryFontIndex::standard_dirs() {
    std::vector<std::string> dirs;
    const std::string home = env("HOME");
#if defined(_WIN32)
    const std::string windir = env("WINDIR");
    if (!windir.empty()) dirs.push_back(windir + "\\Fonts");
    const std::string local = env("LOCALAPPDATA");
    if (!local.empty()) dirs.push_back(local + "\\Microsoft\\Windows\\Fonts");
#elif defined(__APPLE__)
    dirs = {"/System/Library/Fonts", "/Library/Fonts"};
    if (!home.empty()) dirs.push_back(home + "/Library/Fonts");
#else
    std::string data_home = env("XDG_DATA_HOME");
    if (data_home.empty() && !home.empty()) data_home = home + "/.local/share";
    if (!data_home.empty()) dirs.push_back(data_home + "/fonts");
    if (!home.empty()) dirs.push_back(home + "/.fonts");
    std::string data_dirs = env("XDG_DATA_DIRS");
    if (data_dirs.empty()) data_dirs = "/usr/local/share:/usr/share";
    std::size_t start = 0;
    while (start <= data_dirs.size()) {
        const std::size_t end = std::min(data_dirs.find(':', start), data_dirs.size());
        if (end > start) dirs.push_back(data_dirs.substr(start, end - start) + "/fonts");
        start = end + 1;
    }
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
#endif
    // Drop duplicates, keep the order.
    std::vector<std::string> unique;
    for (std::string& d : dirs) {
        if (std::find(unique.begin(), unique.end(), d) == unique.end()) unique.push_back(std::move(d));
    }
    return unique;
}

void DirectoryFontIndex::scan() {
    if (scanned_) return;
    scanned_ = true;
    FtLib ft;
    if (!ft.lib) return;
    std::vector<std::filesystem::path> files;
    namespace fs = std::filesystem;
    for (const std::string& d : dirs_) {
        std::error_code ec;
        fs::recursive_directory_iterator it(fs::path(d), fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && files.size() < kMaxScannedFiles; it.increment(ec)) {
            std::error_code fec;
            if (it->is_regular_file(fec) && font_extension(it->path())) files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    for (const fs::path& p : files) {
        const std::string path = p.string();
        FtFace probe(ft.lib, path, -1);
        if (!probe.face) continue;
        const FT_Long n = std::min<FT_Long>(probe.face->num_faces, 256);
        for (FT_Long i = 0; i < n; ++i) {
            FtFace f(ft.lib, path, i);
            if (!f.face || !f.face->family_name) continue;
            Entry e;
            e.path = path;
            e.index = static_cast<uint32_t>(i);
            e.family = f.face->family_name;
            e.style_name = f.face->style_name ? f.face->style_name : "";
            e.bold = (f.face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
            e.weight = e.bold ? 700 : 400;
            const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(f.face, FT_SFNT_OS2));
            if (os2 && os2->version != 0xFFFF && os2->usWeightClass >= 100 && os2->usWeightClass <= 1000)
                e.weight = os2->usWeightClass;
            e.italic = (f.face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
            e.monospace = FT_IS_FIXED_WIDTH(f.face) != 0;
            e.scalable = FT_IS_SCALABLE(f.face) != 0;
            e.blocks.assign(kBlocks / 64, 0);
            if (FT_Select_Charmap(f.face, FT_ENCODING_UNICODE) == 0) {
                FT_UInt gid = 0;
                FT_ULong cp = FT_Get_First_Char(f.face, &gid);
                while (gid != 0) {
                    if (cp < 0x110000) {
                        const std::size_t b = cp >> 8;
                        e.blocks[b >> 6] |= uint64_t{1} << (b & 63);
                    }
                    cp = FT_Get_Next_Char(f.face, cp, &gid);
                }
            }
            entries_.push_back(std::move(e));
        }
    }
}

const std::vector<DirectoryFontIndex::Entry>& DirectoryFontIndex::entries() {
    scan();
    return entries_;
}

std::vector<FontFamilyInfo> DirectoryFontIndex::families() {
    scan();
    std::vector<FontFamilyInfo> out;
    for (const Entry& e : entries_) {
        auto it = std::find_if(out.begin(), out.end(), [&](const FontFamilyInfo& f) { return family_equal(f.name, e.family); });
        if (it == out.end()) out.push_back(FontFamilyInfo{e.family, e.monospace});
        else it->monospace = it->monospace || e.monospace;
    }
    std::sort(out.begin(), out.end(), [](const FontFamilyInfo& a, const FontFamilyInfo& b) { return a.name < b.name; });
    return out;
}

std::optional<FontFileRef> DirectoryFontIndex::match(const FontQuery& q) {
    scan();
    const std::string family = q.family.empty() ? default_monospace() : q.family;
    const bool want_bold = q.style == Style::bold || q.style == Style::bold_italic;
    const bool want_italic = q.style == Style::italic || q.style == Style::bold_italic;
    const Entry* best = nullptr;
    int best_score = -1;
    if (q.weight != 0) {
        int best_dist = 151;
        for (const Entry& e : entries_) {
            if (!e.scalable || e.italic != want_italic || !family_equal(e.family, family)) continue;
            const int dist = std::abs(int{e.weight} - int{q.weight});
            if (dist < best_dist) {
                best = &e;
                best_dist = dist;
            }
        }
        if (!best) return std::nullopt;
        return FontFileRef{best->path, nullptr, best->index};
    }
    for (const Entry& e : entries_) {
        if (!e.scalable || e.bold != want_bold || e.italic != want_italic || !family_equal(e.family, family)) continue;
        // Prefer the plain style names over Medium, Light, Condensed, ...
        int score = 1;
        const std::string_view sn = e.style_name;
        if (sn == "Regular" || sn == "Bold" || sn == "Italic" || sn == "Bold Italic" || sn == "Book" ||
            sn == "Oblique" || sn == "Bold Oblique")
            score = 2;
        if (score > best_score) {
            best = &e;
            best_score = score;
        }
    }
    if (!best) return std::nullopt;
    return FontFileRef{best->path, nullptr, best->index};
}

std::vector<FontFileRef> DirectoryFontIndex::fallback_for(char32_t cp, const FontQuery& base, std::string_view lang) {
    (void)lang;   // no language data in the index; fontconfig and DirectWrite use it
    scan();
    std::vector<FontFileRef> out;
    if (cp >= 0x110000) return out;
    const std::size_t b = cp >> 8;
    std::vector<const Entry*> candidates;
    for (const Entry& e : entries_) {
        if (e.scalable && !e.bold && !e.italic && ((e.blocks[b >> 6] >> (b & 63)) & 1u)) candidates.push_back(&e);
    }
    // Same family first, then monospace faces, then the rest (stable: path order).
    std::stable_sort(candidates.begin(), candidates.end(), [&](const Entry* x, const Entry* y) {
        const int rx = family_equal(x->family, base.family) ? 0 : x->monospace ? 1 : 2;
        const int ry = family_equal(y->family, base.family) ? 0 : y->monospace ? 1 : 2;
        return rx < ry;
    });
    FtLib ft;
    for (const Entry* e : candidates) {
        FtFace f(ft.lib, e->path, static_cast<FT_Long>(e->index));
        if (!f.face || FT_Select_Charmap(f.face, FT_ENCODING_UNICODE) != 0) continue;
        if (FT_Get_Char_Index(f.face, cp) == 0) continue;
        out.push_back(FontFileRef{e->path, nullptr, e->index});
        if (out.size() >= kMaxFallbackResults) break;
    }
    return out;
}

std::string DirectoryFontIndex::default_monospace() {
    scan();
    for (const std::string& d : defaults_) {
        for (const Entry& e : entries_) {
            if (family_equal(e.family, d)) return e.family;
        }
    }
    for (const Entry& e : entries_) {
        if (e.monospace && e.scalable && !e.bold && !e.italic) return e.family;
    }
    return {};
}

}  // namespace kt::text
