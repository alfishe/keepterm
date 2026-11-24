#include "theme/cli.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "platform/paths.h"
#include "text/font_source.h"
#include "theme/library.h"
#include "theme/theme_set.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace kt::theme {

namespace {

bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// True-color escapes on a terminal; Windows consoles need VT processing turned on. --color
// forces them even when stdout is no console (a pipe, ctest).
bool enable_color() {
#ifdef _WIN32
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !::GetConsoleMode(h, &mode)) return false;
    return ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

std::string sgr_fg(Rgb c) {
    return "\x1b[38;2;" + std::to_string((c >> 16) & 0xFF) + ";" + std::to_string((c >> 8) & 0xFF) + ";" +
           std::to_string(c & 0xFF) + "m";
}
std::string sgr_bg(Rgb c) {
    return "\x1b[48;2;" + std::to_string((c >> 16) & 0xFF) + ";" + std::to_string((c >> 8) & 0xFF) + ";" +
           std::to_string(c & 0xFF) + "m";
}
constexpr const char* kReset = "\x1b[0m";

struct Common {
    std::string profile;
    bool color = false, no_color = false, toml = false, all = false;
    std::vector<std::string> args;
};

// Parses --profile NAME, --color, --no-color, --toml, --all; the rest are arguments.
bool parse_common(int argc, char** argv, int i, Common& c) {
    for (; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--profile") {
            if (i + 1 >= argc || !platform::paths::valid_profile_name(argv[i + 1])) {
                std::fprintf(stderr, "keepterm: invalid profile name (allowed: [a-z0-9-]{1,32})\n");
                return false;
            }
            c.profile = argv[++i];
        } else if (a == "--color") {
            c.color = true;
        } else if (a == "--no-color") {
            c.no_color = true;
        } else if (a == "--toml") {
            c.toml = true;
        } else if (a == "--all") {
            c.all = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "keepterm: unknown option %s\n", argv[i]);
            return false;
        } else {
            c.args.emplace_back(a);
        }
    }
    return true;
}

void print_diags(std::span<const config::Diagnostic> ds) {
    for (const config::Diagnostic& d : ds) {
        if (d.level == config::Diagnostic::Level::Info) continue;
        std::fprintf(stderr, "%s\n", config::format(d).c_str());
    }
}

int list_themes(const Common& c) {
    const auto dirs = user_theme_dirs(c.profile);
    const ThemeLibrary lib = ThemeLibrary::load(dirs);
    print_diags(lib.diagnostics());
    config::LoadOptions opts;
    opts.profile = c.profile;
    const config::Config cfg = config::Config::load(opts);
    const Selection sel = selection(cfg);
    const bool color = !c.no_color && ((stdout_is_tty() && enable_color()) || c.color);
    for (const ThemeInfo& t : lib.list()) {
        const Theme* th = lib.find(t.name);
        std::string mark;
        if (th && lib.find(sel.light) == th) mark += " [light]";
        if (th && lib.find(sel.dark) == th) mark += " [dark]";
        std::string swatch;
        if (color && th) {
            swatch = sgr_bg(th->background) + " ";
            for (std::size_t i = 1; i < 7; ++i) swatch += sgr_fg(th->palette[i]) + "\xE2\x96\xA0";   // U+25A0
            swatch += sgr_fg(th->foreground) + " Aa " + kReset + " ";
        }
        std::printf("%s%-28s %-5s  %s%s%s\n", swatch.c_str(), t.name.c_str(), std::string(to_string(t.appearance)).c_str(),
                    t.builtin ? "built-in" : t.source.c_str(), t.shadows_builtin ? " (replaces the built-in)" : "",
                    mark.c_str());
    }
    return 0;
}

void preview(const Theme& t) {
    const std::string bg = sgr_bg(t.background), fg = sgr_fg(t.foreground);
    auto line = [&](const std::string& body) { std::printf("  %s%s %s%s%s\x1b[K%s\n", bg.c_str(), fg.c_str(), body.c_str(), bg.c_str(), fg.c_str(), kReset); };
    std::printf("\n");
    line("");
    line("\x1b[1m" + sgr_fg(t.bold.value_or(t.foreground)) + "~/project" + fg + "\x1b[22m " + sgr_fg(t.palette[2]) +
         "(main)" + fg + " $ ls --color");
    line(sgr_fg(t.palette[4]) + "src  " + sgr_fg(t.palette[2]) + "build.sh  " + sgr_fg(t.palette[6]) + "link -> src  " +
         sgr_fg(t.palette[1]) + "core.dump" + fg);
    line(sgr_bg(t.selection_background) + (t.selection_foreground ? sgr_fg(*t.selection_foreground) : fg) +
         " selected text " + bg + fg + " " + sgr_bg(t.cursor) + sgr_fg(t.cursor_text) + "c" + bg + fg + "ursor" +
         (t.link ? "  " + sgr_fg(*t.link) + "\x1b[4mhttps://example.com\x1b[24m" + fg : std::string()));
    line("");
    for (int row = 0; row < 2; ++row) {
        std::string s;
        for (int i = 0; i < 8; ++i) {
            const Rgb c = t.palette[static_cast<std::size_t>(row * 8 + i)];
            s += sgr_bg(c) + sgr_fg(contrast(c, 0xFFFFFF) > contrast(c, 0) ? 0xFFFFFFu : 0u) + " " +
                 (row * 8 + i < 10 ? " " : "") + std::to_string(row * 8 + i) + "  ";
        }
        line(s + bg + fg);
    }
    line("");
    std::printf("%s\n", kReset);
}

int show_theme(const Common& c) {
    if (c.args.size() != 1) {
        std::fprintf(stderr, "usage: keepterm --theme show NAME [--toml] [--color|--no-color] [--profile NAME]\n");
        return 2;
    }
    const ThemeLibrary lib = ThemeLibrary::load(user_theme_dirs(c.profile));
    const Theme* t = lib.find(c.args[0]);
    if (!t) {
        std::fprintf(stderr, "keepterm: no theme \"%s\" (keepterm --theme list)\n", c.args[0].c_str());
        return 1;
    }
    for (const config::Diagnostic& d : lib.diagnostics()) {
        if (!t->builtin && d.file == t->source && d.level != config::Diagnostic::Level::Info)
            std::fprintf(stderr, "%s\n", config::format(d).c_str());
    }
    if (c.toml) {
        std::fputs(to_toml(*t).c_str(), stdout);
        return 0;
    }
    std::printf("%s (%s, %s)\n", t->name.c_str(), std::string(to_string(t->appearance)).c_str(),
                t->builtin ? "built-in" : t->source.c_str());
    if (!c.no_color && ((stdout_is_tty() && enable_color()) || c.color)) preview(*t);
    std::printf("  background %s  foreground %s  cursor %s / %s  selection %s%s\n", format_rgb(t->background).c_str(),
                format_rgb(t->foreground).c_str(), format_rgb(t->cursor).c_str(), format_rgb(t->cursor_text).c_str(),
                format_rgb(t->selection_background).c_str(),
                t->selection_foreground ? (" / " + format_rgb(*t->selection_foreground)).c_str() : "");
    std::printf("  palette");
    for (std::size_t i = 0; i < 16; ++i) std::printf("%s %s", i == 8 ? "\n         " : "", format_rgb(t->palette[i]).c_str());
    std::printf("\n  contrast %.1f:1\n", contrast(t->foreground, t->background));
    return 0;
}

}  // namespace

const char* cli_usage() {
    return "       keepterm --theme list [--profile NAME] | --theme show NAME [--toml] [--color|--no-color]\n"
           "       keepterm --font list [--all]\n";
}

int theme_cli(int argc, char** argv, int i) {
    if (i >= argc) {
        std::fputs(cli_usage(), stderr);
        return 2;
    }
    const std::string_view sub = argv[i];
    Common c;
    if (!parse_common(argc, argv, i + 1, c)) return 2;
    if (sub == "list" && c.args.empty()) return list_themes(c);
    if (sub == "show") return show_theme(c);
    std::fputs(cli_usage(), stderr);
    return 2;
}

int font_cli(int argc, char** argv, int i) {
    Common c;
    if (i >= argc || std::string_view(argv[i]) != "list" || !parse_common(argc, argv, i + 1, c) || !c.args.empty()) {
        std::fputs(cli_usage(), stderr);
        return 2;
    }
    auto src = text::create_font_source();
    if (!src) {
        std::fprintf(stderr, "keepterm: no font source on this system\n");
        return 1;
    }
    const std::string def = src->default_monospace();
    std::vector<std::string> names;
    if (c.all) {
        for (const text::FontFamilyInfo& f : src->families()) names.push_back(f.name + (f.monospace ? "  (monospace)" : ""));
        std::sort(names.begin(), names.end());
    } else {
        names = text::monospace_families(*src);
    }
    for (const std::string& n : names) std::printf("%s%s\n", n.c_str(), n == def ? "  (default)" : "");
    return 0;
}

}  // namespace kt::theme
