#include "commands/context.h"

#include <iterator>

namespace kt::commands {

namespace {

struct CtxName {
    Ctx ctx;
    std::string_view name;
};
constexpr CtxName kNames[] = {
    {Ctx::Always, "always"},       {Ctx::Window, "window"},         {Ctx::Terminal, "terminal"},
    {Ctx::TextInput, "text_input"}, {Ctx::Palette, "palette"},      {Ctx::FindBar, "find_bar"},
    {Ctx::CopyMode, "copy_mode"},  {Ctx::Sidebar, "sidebar"},       {Ctx::QuickTerminal, "quick_terminal"},
    {Ctx::HasProject, "has_project"}, {Ctx::HasSplit, "has_split"}, {Ctx::Remote, "remote"},
};

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace

int specificity(Ctx c) {
    int rank = static_cast<int>(std::size(kSpecificityOrder));
    for (Ctx s : kSpecificityOrder) {
        if (any(c & s)) return rank;
        --rank;
    }
    return 0;
}

std::string format_ctx(Ctx c) {
    std::string out;
    for (const auto& n : kNames) {
        if (!any(c & n.ctx)) continue;
        if (!out.empty()) out += '|';
        out += n.name;
    }
    return out.empty() ? "none" : out;
}

bool parse_ctx_name(std::string_view name, Ctx& out) {
    for (const auto& n : kNames) {
        if (iequal(n.name, name)) {
            out = n.ctx;
            return true;
        }
    }
    return false;
}

std::string_view scope_name(PaletteScope s) {
    switch (s) {
        case PaletteScope::Root: return "root";
        case PaletteScope::Files: return "files";
        case PaletteScope::Sessions: return "sessions";
        case PaletteScope::Tabs: return "tabs";
        case PaletteScope::Folders: return "folders";
        case PaletteScope::MoveTabToProject: return "move_tab_to_project";
        case PaletteScope::Workspaces: return "workspaces";
        case PaletteScope::History: return "history";
    }
    return "root";
}

}  // namespace kt::commands
