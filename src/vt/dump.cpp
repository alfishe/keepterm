#include "vt/dump.h"

#include <cstdio>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "vt/terminal.h"
#include "vt/utf8.h"

namespace kt::vt {
namespace {

constexpr std::string_view kLetters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

void append_cells(std::string& out, std::span<const Cell> cells, const GraphemePool& pool) {
    for (const Cell& c : cells) {
        switch (c.width()) {
            case CellWidth::WideTail: out.push_back('~'); continue;
            case CellWidth::Empty: out.push_back(' '); continue;
            default: break;
        }
        if (c.cluster()) {
            for (char32_t cp : pool[c.cp()]) append_utf8(out, cp);
        } else {
            append_utf8(out, c.cp() == 0 ? U' ' : c.cp());
        }
    }
}

std::string color_str(Color c) {
    char buf[32];
    if (c.tag() == Color::Palette) {
        std::snprintf(buf, sizeof buf, "palette:%u", static_cast<unsigned>(c.index()));
    } else {
        std::snprintf(buf, sizeof buf, "rgb:%02x%02x%02x", static_cast<unsigned>(c.r()), static_cast<unsigned>(c.g()),
                      static_cast<unsigned>(c.b()));
    }
    return buf;
}

std::string describe(const Style& s, const Link* link) {
    std::string out;
    const auto add = [&out](const std::string& part) {
        if (!out.empty()) out.push_back(' ');
        out += part;
    };
    if (s.fg.tag() != Color::Default) add("fg=" + color_str(s.fg));
    if (s.bg.tag() != Color::Default) add("bg=" + color_str(s.bg));
    static constexpr const char* kUnderline[] = {"none", "single", "double", "curly", "dotted", "dashed"};
    if (s.underline != Underline::None) add(std::string("underline=") + kUnderline[static_cast<int>(s.underline)]);
    if (s.underline_color.tag() != Color::Default) add("ulcolor=" + color_str(s.underline_color));
    static constexpr std::pair<Attr, const char*> kAttrs[] = {
        {kBold, "bold"},       {kFaint, "faint"},         {kItalic, "italic"}, {kBlink, "blink"},
        {kInverse, "inverse"}, {kInvisible, "invisible"}, {kStrike, "strike"}, {kOverline, "overline"}};
    for (const auto& [a, name] : kAttrs) {
        if (s.has(a)) add(name);
    }
    if (link != nullptr) {
        add("link=" + link->uri);
        if (!link->id.empty()) add("link-id=" + link->id);
    }
    return out.empty() ? "default" : out;
}

}  // namespace

std::string dump_screen(const Terminal& t, const DumpOptions& opts) {
    const Screen& s = t.screen();
    const CursorState cur = t.cursor();
    std::string out = "# keepterm-dump 1\n";
    static constexpr const char* kShapes[] = {"block", "underline", "bar"};
    out += "size " + std::to_string(s.cols()) + "x" + std::to_string(s.rows()) + " cursor " + std::to_string(cur.row) +
           "," + std::to_string(cur.col) + (cur.visible ? " visible" : " hidden") + " shape=" +
           kShapes[static_cast<int>(cur.shape)] + " alt=" + (t.alt_active() ? "1" : "0") +
           (cur.pending_wrap ? " pending-wrap" : "") + "\n";

    std::map<std::pair<uint16_t, uint16_t>, char> legend;
    std::vector<std::pair<uint16_t, uint16_t>> order;
    const std::pair<uint16_t, uint16_t> plain{uint16_t{0}, uint16_t{0}};
    legend[plain] = 'a';
    order.push_back(plain);
    for (uint16_t y = 0; y < s.rows(); ++y) {
        const auto row = s.row(y);
        out += "row " + std::to_string(y) + " |";
        append_cells(out, row, t.graphemes());
        out += "|";
        if (s.flags(y) & kRowWrapped) out += "W";
        out += "\n";
        if (!opts.attrs) continue;
        out += "attr " + std::to_string(y) + " |";
        for (const Cell& c : row) {
            const std::pair<uint16_t, uint16_t> key{c.style, c.link};
            auto it = legend.find(key);
            if (it == legend.end()) {
                const char letter = order.size() < kLetters.size() ? kLetters[order.size()] : '?';
                it = legend.emplace(key, letter).first;
                order.push_back(key);
            }
            out.push_back(it->second);
        }
        out += "|\n";
    }
    if (opts.attrs) {
        for (std::size_t i = 0; i < order.size(); ++i) {
            const auto [style, link] = order[i];
            const char letter = i < kLetters.size() ? kLetters[i] : '?';
            out += "style ";
            out.push_back(letter);
            out += " = " + describe(t.styles()[style], link != 0 ? &t.links()[link] : nullptr) + "\n";
        }
    }
    return out;
}

std::string dump_scrollback(const Terminal& t) {
    const Scrollback& sb = t.scrollback();
    std::string out;
    for (std::size_t i = 0; i < sb.size(); ++i) {
        const auto& line = sb.line(i);
        out += "sb " + std::to_string(sb.first_abs_line() + i) + " |";
        append_cells(out, line.cells, t.graphemes());
        out += "|";
        if (line.flags & kRowWrapped) out += "W";
        out += "\n";
    }
    return out;
}

}  // namespace kt::vt
