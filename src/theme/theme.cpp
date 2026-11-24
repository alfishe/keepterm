#include "theme/theme.h"

#include <algorithm>
#include <charconv>

#include "config/schema.h"       // edit_distance
#include "config/toml_reader.h"  // the positioned TOML tree of kt_config

namespace kt::theme {

namespace {

using config::Diagnostic;
using Level = config::Diagnostic::Level;
using config::detail::PNode;
using config::detail::PTable;

constexpr std::string_view kUiTokens[] = {
    "bg",        "fg",          "fg_muted",          "fg_dim",  "surface", "surface_raised", "border",
    "hover",     "selection",   "selection_inactive", "accent",  "accent_soft", "success",     "warning",
    "failure",   "scrim",       "shadow",            "dim_overlay",
};

constexpr std::string_view kColorKeys[] = {"background", "foreground", "cursor", "cursor_text", "selection_background",
                                           "selection_foreground", "bold", "link", "accent", "palette"};

// Gathers diagnostics for one file.
struct Diags {
    std::string file;
    std::vector<Diagnostic> out;

    void add(Level lv, int line, int col, std::string key, std::string msg, std::string hint = {}) {
        Diagnostic d;
        d.level = lv;
        d.file = file;
        d.line = line;
        d.column = col;
        d.key = std::move(key);
        d.message = std::move(msg);
        d.hint = std::move(hint);
        out.push_back(std::move(d));
    }
};

std::string closest(std::string_view key, std::span<const std::string_view> known) {
    std::string_view best;
    std::size_t best_d = 3;
    for (std::string_view k : known) {
        const std::size_t d = config::edit_distance(key, k);
        if (d < best_d) {
            best_d = d;
            best = k;
        }
    }
    return best.empty() ? std::string() : "did you mean '" + std::string(best) + "'?";
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// What a file set; the rest is filled by finish().
struct Partial {
    Theme t;
    std::optional<Appearance> appearance;
    std::array<bool, 16> have_palette{};
    std::optional<Rgb> fg, bg, cursor, cursor_text, selection_bg;
    bool any_color = false;
};

Theme finish(Partial p) {
    Theme& t = p.t;
    const Appearance hint = p.appearance ? *p.appearance : (p.bg ? appearance_of(*p.bg) : Appearance::dark);
    t.background = p.bg ? *p.bg : (hint == Appearance::light ? 0xFFFFFFu : 0x302F3Au);
    t.foreground = p.fg ? *p.fg : (hint == Appearance::light ? 0x1F2328u : 0xE0DEF4u);
    t.appearance = p.appearance ? *p.appearance : appearance_of(t.background);
    for (std::size_t i = 0; i < 16; ++i) {
        if (!p.have_palette[i]) t.palette[i] = default_palette()[i];
    }
    t.cursor = p.cursor ? *p.cursor : t.foreground;
    t.cursor_text = p.cursor_text ? *p.cursor_text : t.background;
    t.selection_background = p.selection_bg ? *p.selection_bg : mix(t.background, t.foreground, 0.25);
    return std::move(p.t);
}

// ---------------------------------------------------------------- keepterm TOML

const std::string* string_of(const PNode& n) {
    const config::RawValue* s = n.scalar();
    return s ? std::get_if<std::string>(&s->v) : nullptr;
}

// A color string; "" gives `allow_empty` -> nullopt without a diagnostic.
std::optional<Rgb> color_of(const PNode& n, std::string_view key, Diags& d, bool* empty = nullptr) {
    const std::string* s = string_of(n);
    if (!s) {
        d.add(Level::Warning, n.line, n.column, std::string(key), "'" + std::string(key) + "' must be a color string",
              "use \"#RRGGBB\"");
        return std::nullopt;
    }
    if (s->empty()) {
        if (empty) *empty = true;
        return std::nullopt;
    }
    auto c = parse_rgb(*s);
    if (!c)
        d.add(Level::Warning, n.line, n.column, std::string(key), "\"" + *s + "\" is not a color; using the default",
              "use \"#RRGGBB\"");
    return c;
}

void read_colors(const PTable& tbl, Partial& p, Diags& d) {
    for (const auto& [key, node] : tbl) {
        const std::string path = "colors." + key;
        if (key == "palette") {
            const config::detail::PArray* arr = node.array();
            if (!arr) {
                d.add(Level::Warning, node.line, node.column, path, "'colors.palette' must be a list of 16 colors");
                continue;
            }
            if (arr->size() != 16)
                d.add(Level::Warning, node.line, node.column, path,
                      "'colors.palette' has " + std::to_string(arr->size()) + " colors; 16 expected",
                      "missing entries use the default palette");
            for (std::size_t i = 0; i < std::min<std::size_t>(16, arr->size()); ++i) {
                if (auto c = color_of((*arr)[i], path, d)) {
                    p.t.palette[i] = *c;
                    p.have_palette[i] = true;
                    p.any_color = true;
                }
            }
            continue;
        }
        bool empty = false;
        std::optional<Rgb> c;
        const bool known = std::find(std::begin(kColorKeys), std::end(kColorKeys), key) != std::end(kColorKeys);
        if (!known) {
            d.add(Level::Warning, node.line, node.column, path, "unknown key '" + path + "'", closest(key, kColorKeys));
            continue;
        }
        c = color_of(node, path, d, &empty);
        if (c) p.any_color = true;
        if (key == "background") p.bg = c;
        else if (key == "foreground") p.fg = c;
        else if (key == "cursor") p.cursor = c;
        else if (key == "cursor_text") p.cursor_text = c;
        else if (key == "selection_background") p.selection_bg = c;
        else if (key == "selection_foreground") p.t.selection_foreground = c;
        else if (key == "bold") p.t.bold = c;
        else if (key == "link") p.t.link = c;
        else if (key == "accent") p.t.accent = c;
    }
}

void read_ui(const PTable& tbl, Partial& p, Diags& d) {
    for (const auto& [key, node] : tbl) {
        const std::string path = "ui." + key;
        if (std::find(std::begin(kUiTokens), std::end(kUiTokens), key) == std::end(kUiTokens)) {
            d.add(Level::Warning, node.line, node.column, path, "unknown UI token '" + key + "'", closest(key, kUiTokens));
            continue;
        }
        const std::string* s = string_of(node);
        auto c = s ? parse_rgba(*s) : std::nullopt;
        if (!c) {
            d.add(Level::Warning, node.line, node.column, path, "'" + path + "' is not a color; ignored",
                  "use \"#RRGGBB\" or \"#RRGGBBAA\"");
            continue;
        }
        p.t.ui.push_back(UiToken{key, *c});
    }
    std::sort(p.t.ui.begin(), p.t.ui.end(), [](const UiToken& a, const UiToken& b) { return a.name < b.name; });
}

}  // namespace

std::string_view to_string(Appearance a) { return a == Appearance::light ? "light" : "dark"; }

const std::array<Rgb, 16>& default_palette() {
    static constexpr std::array<Rgb, 16> k = {0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD,
                                              0x00CDCD, 0xE5E5E5, 0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00,
                                              0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
    return k;
}

std::span<const std::string_view> ui_token_names() { return kUiTokens; }

Appearance appearance_of(Rgb background) { return luminance(background) > 0.5 ? Appearance::light : Appearance::dark; }

std::string normalize_name(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || static_cast<unsigned char>(c) >= 0x80) out += c;
    }
    return out;
}

ParseResult parse_keepterm_theme(std::string_view text, std::string_view file) {
    ParseResult res;
    Diags d{std::string(file), {}};
    auto parsed = config::detail::parse_toml(text, file);
    if (auto* err = std::get_if<config::detail::ParseError>(&parsed)) {
        d.add(Level::Error, err->line, err->column, "", err->message);
        res.diagnostics = std::move(d.out);
        return res;
    }
    const PNode& root = std::get<PNode>(parsed);
    Partial p;
    p.t.format = ThemeFormat::keepterm;
    p.t.source = std::string(file);
    if (const PTable* top = root.table()) {
        for (const auto& [key, node] : *top) {
            if (key == "name") {
                if (const std::string* s = string_of(node); s && !trim(*s).empty()) p.t.name = std::string(trim(*s));
                else d.add(Level::Warning, node.line, node.column, "name", "'name' must be a non-empty string");
            } else if (key == "appearance") {
                const std::string* s = string_of(node);
                if (s && *s == "light") p.appearance = Appearance::light;
                else if (s && *s == "dark") p.appearance = Appearance::dark;
                else
                    d.add(Level::Warning, node.line, node.column, "appearance",
                          "'appearance' must be \"light\" or \"dark\"; taken from the background");
            } else if (key == "colors") {
                if (const PTable* t = node.table()) read_colors(*t, p, d);
                else d.add(Level::Warning, node.line, node.column, "colors", "'colors' must be a table");
            } else if (key == "ui") {
                if (const PTable* t = node.table()) read_ui(*t, p, d);
                else d.add(Level::Warning, node.line, node.column, "ui", "'ui' must be a table");
            } else {
                static constexpr std::string_view kTop[] = {"name", "appearance", "colors", "ui"};
                d.add(Level::Warning, node.line, node.column, key, "unknown key '" + key + "'", closest(key, kTop));
            }
        }
    }
    if (p.t.name.empty()) {
        // The file name without its extension.
        std::string_view base = file;
        if (auto s = base.find_last_of("/\\"); s != std::string_view::npos) base.remove_prefix(s + 1);
        if (auto dot = base.rfind('.'); dot != std::string_view::npos && dot > 0) base = base.substr(0, dot);
        p.t.name = std::string(base);
        d.add(Level::Info, 1, 0, "name", "no 'name'; the theme is called \"" + p.t.name + "\"");
    }
    if (!p.any_color) {
        d.add(Level::Error, 1, 0, "", "the theme sets no colors");
        res.diagnostics = std::move(d.out);
        return res;
    }
    res.theme = finish(std::move(p));
    res.diagnostics = std::move(d.out);
    return res;
}

ParseResult parse_ghostty_theme(std::string_view text, std::string_view name, std::string_view file) {
    ParseResult res;
    Diags d{std::string(file), {}};
    Partial p;
    p.t.name = std::string(name);
    p.t.format = ThemeFormat::ghostty;
    p.t.source = std::string(file);
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    int line_no = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view raw = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            d.add(Level::Warning, line_no, 1, "", "expected 'key = value'");
            continue;
        }
        const std::string key(trim(line.substr(0, eq)));
        std::string_view value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        const int col = static_cast<int>(line.data() - raw.data()) + 1;

        auto color = [&](std::string_view v) -> std::optional<Rgb> {
            if (v.empty()) return std::nullopt;
            if (v == "cell-foreground" || v == "cell-background") {
                d.add(Level::Info, line_no, col, key, "'" + std::string(v) + "' is not supported; the default applies");
                return std::nullopt;
            }
            auto c = parse_rgb(v);
            if (!c) d.add(Level::Warning, line_no, col, key, "\"" + std::string(v) + "\" is not a color", "use #RRGGBB");
            return c;
        };

        if (key == "palette") {
            const std::size_t e2 = value.find('=');
            unsigned idx = 0;
            const std::string_view is = trim(value.substr(0, e2));
            const auto r = std::from_chars(is.data(), is.data() + is.size(), idx);
            if (e2 == std::string_view::npos || r.ec != std::errc{} || r.ptr != is.data() + is.size() || idx > 255) {
                d.add(Level::Warning, line_no, col, key, "expected 'palette = N=#RRGGBB'");
                continue;
            }
            if (idx >= 16) continue;   // the 256-color cube and ramp stay standard
            if (auto c = color(trim(value.substr(e2 + 1)))) {
                p.t.palette[idx] = *c;
                p.have_palette[idx] = true;
                p.any_color = true;
            }
            continue;
        }
        std::optional<Rgb>* slot = nullptr;
        if (key == "background") slot = &p.bg;
        else if (key == "foreground") slot = &p.fg;
        else if (key == "cursor-color") slot = &p.cursor;
        else if (key == "cursor-text") slot = &p.cursor_text;
        else if (key == "selection-background") slot = &p.selection_bg;
        else if (key == "selection-foreground") slot = &p.t.selection_foreground;
        else if (key == "bold-color") slot = &p.t.bold;
        if (!slot) {
            d.add(Level::Info, line_no, col, key, "'" + key + "' is not a theme color; ignored");
            continue;
        }
        if (auto c = color(value)) {
            *slot = c;
            p.any_color = true;
        }
    }
    if (!p.any_color) {
        d.add(Level::Error, 1, 0, "", "the theme sets no colors");
        res.diagnostics = std::move(d.out);
        return res;
    }
    res.theme = finish(std::move(p));
    res.diagnostics = std::move(d.out);
    return res;
}

ThemeFormat detect_format(std::string_view filename, std::string_view text) {
    if (filename.size() >= 5) {
        std::string ext(filename.substr(filename.size() - 5));
        for (char& c : ext) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (ext == ".toml") return ThemeFormat::keepterm;
    }
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line = trim(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        if (line == "[colors]") return ThemeFormat::keepterm;
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return ThemeFormat::ghostty;
}

ParseResult parse_theme_file(std::string_view filename, std::string_view text, std::string_view display_path) {
    if (detect_format(filename, text) == ThemeFormat::keepterm) return parse_keepterm_theme(text, display_path);
    return parse_ghostty_theme(text, filename, display_path);
}

std::string to_toml(const Theme& t) {
    auto q = [](std::string_view s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    };
    std::string s;
    s += "name = " + q(t.name) + "\n";
    s += "appearance = \"" + std::string(to_string(t.appearance)) + "\"\n\n[colors]\n";
    s += "background = \"" + format_rgb(t.background) + "\"\n";
    s += "foreground = \"" + format_rgb(t.foreground) + "\"\n";
    s += "cursor = \"" + format_rgb(t.cursor) + "\"\n";
    s += "cursor_text = \"" + format_rgb(t.cursor_text) + "\"\n";
    s += "selection_background = \"" + format_rgb(t.selection_background) + "\"\n";
    s += "selection_foreground = \"" + (t.selection_foreground ? format_rgb(*t.selection_foreground) : "") + "\"\n";
    if (t.bold) s += "bold = \"" + format_rgb(*t.bold) + "\"\n";
    if (t.link) s += "link = \"" + format_rgb(*t.link) + "\"\n";
    if (t.accent) s += "accent = \"" + format_rgb(*t.accent) + "\"\n";
    s += "palette = [\n";
    for (std::size_t i = 0; i < 16; ++i) {
        s += (i % 8 == 0 ? "  " : " ") + q(format_rgb(t.palette[i])) + ",";
        if (i % 8 == 7) s += "\n";
    }
    s += "]\n";
    if (!t.ui.empty()) {
        s += "\n[ui]\n";
        for (const UiToken& u : t.ui) s += u.name + " = \"" + format_rgba(u.value) + "\"\n";
    }
    return s;
}

}  // namespace kt::theme
