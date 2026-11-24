#include "workspace/paths.h"

#include <vector>

namespace kt::ws {

std::string to_slashes(std::string_view path, PathStyle style) {
    std::string s(path);
    if (style == PathStyle::Windows)
        for (char& c : s)
            if (c == '\\') c = '/';
    return s;
}

std::string canonical_path(std::string_view path, PathStyle style) {
    std::string s = to_slashes(path, style);
    std::string prefix;
    std::size_t pos = 0;
    if (style == PathStyle::Windows) {
        if (s.size() >= 2 && s[1] == ':' &&
            ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
            prefix = s.substr(0, 2);
            pos = 2;
        } else if (s.rfind("//", 0) == 0) {
            prefix = "//";   // UNC
            pos = 2;
        }
    }
    const bool absolute = pos < s.size() && s[pos] == '/';
    std::vector<std::string> parts;
    while (pos < s.size()) {
        std::size_t next = s.find('/', pos);
        if (next == std::string::npos) next = s.size();
        std::string part = s.substr(pos, next - pos);
        pos = next + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!absolute) parts.push_back(part);
            continue;
        }
        parts.push_back(std::move(part));
    }
    std::string out = prefix;
    if (absolute && prefix != "//") out.push_back('/');
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.push_back('/');
        out += parts[i];
    }
    if (out.empty()) out = absolute ? "/" : ".";
    if (style == PathStyle::Windows) {
        for (char& c : out)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (out.size() >= 2 && out[1] == ':') out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

bool same_folder(std::string_view a, std::string_view b, PathStyle style) {
    return canonical_path(a, style) == canonical_path(b, style);
}

std::string folder_name(std::string_view path) {
    std::string s = to_slashes(path, PathStyle::Windows);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const std::size_t slash = s.rfind('/');
    std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
    if (name.empty() || (name.size() == 2 && name[1] == ':')) name = s;
    return name;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    while (a < b && sp(s[a])) ++a;
    while (b > a && sp(s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

}  // namespace kt::ws
