#include "workspace/preset_store.h"

#include <algorithm>

namespace kt::ws {

namespace {

constexpr std::string_view kExt = ".json";
constexpr std::string_view kDefaultFile = ".default";   // the default preset's name, one line

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

bool has_ext(const std::string& file) {
    return file.size() > kExt.size() && file.compare(file.size() - kExt.size(), kExt.size(), kExt) == 0 && file[0] != '.';
}

WsResult<void> check_name(std::string_view name) {
    if (!valid_preset_name(name))
        return ws_error(WsErrc::InvalidName, "a name needs 1 to 64 characters without / \\ < > : \" | ? * and no leading or trailing space or dot");
    return {};
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

std::string PresetStore::path_of(std::string_view name) const { return path_join(dir_, std::string(name) + std::string(kExt)); }

std::vector<PresetInfo> PresetStore::list() const {
    std::vector<PresetInfo> out;
    const auto def = default_name();
    for (const std::string& f : fs_.list(dir_)) {
        if (!has_ext(f)) continue;
        PresetInfo i;
        i.name = f.substr(0, f.size() - kExt.size());
        const std::string path = path_join(dir_, f);
        i.modified_unix_ms = fs_.mtime_unix_ms(path).value_or(0);
        i.is_default = def && *def == i.name;
        auto p = load(i.name);
        if (p) {
            i.projects = p->projects.size();
            for (const auto& pr : p->projects) i.tabs += pr.tabs.size();
            i.panes = p->pane_count();
            i.agents = p->has_agents();
        } else {
            i.error = p.error().message;
        }
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) { return lower(a.name) < lower(b.name); });
    return out;
}

bool PresetStore::exists(std::string_view name) const { return valid_preset_name(name) && fs_.exists(path_of(name)); }

WsResult<void> PresetStore::save(const Preset& preset, bool overwrite) {
    if (auto r = check_name(preset.name); !r) return r;
    if (!overwrite && exists(preset.name)) return ws_error(WsErrc::InvalidName, "a workspace named \"" + preset.name + "\" exists");
    if (auto r = fs_.make_private_dir(dir_); !r) return r;
    return fs_.write_atomic(path_of(preset.name), preset_to_text(preset));
}

WsResult<Preset> PresetStore::load(std::string_view name) const {
    if (auto r = check_name(name); !r) return Unexpected<WsError>(r.error());
    ReadResult rr = fs_.read(path_of(name));
    if (rr.status == ReadStatus::Missing) return ws_error(WsErrc::NotFound, "no workspace named \"" + std::string(name) + "\"");
    if (rr.status == ReadStatus::Error) return ws_error(WsErrc::Io, rr.error);
    auto p = preset_from_text(rr.data);
    if (!p) return p;
    p->name = std::string(name);   // the file name wins over the "name" inside
    return p;
}

WsResult<void> PresetStore::rename(std::string_view from, std::string_view to) {
    if (auto r = check_name(to); !r) return r;
    if (from == to) return {};
    if (!exists(from)) return ws_error(WsErrc::NotFound, "no workspace named \"" + std::string(from) + "\"");
    // A case-only rename on a case-insensitive file system is the same file: allow it.
    if (exists(to) && lower(from) != lower(to)) return ws_error(WsErrc::InvalidName, "a workspace named \"" + std::string(to) + "\" exists");
    auto p = load(from);
    if (!p) return Unexpected<WsError>(p.error());
    p->name = std::string(to);
    if (lower(from) == lower(to)) {
        if (auto r = fs_.rename(path_of(from), path_of(to)); !r) return r;
        if (auto r = fs_.write_atomic(path_of(to), preset_to_text(*p)); !r) return r;
    } else {
        if (auto r = fs_.write_atomic(path_of(to), preset_to_text(*p)); !r) return r;
        if (auto r = fs_.remove(path_of(from)); !r) return r;
    }
    if (default_name() == std::string(from)) return set_default(std::string(to));
    return {};
}

WsResult<void> PresetStore::remove(std::string_view name) {
    if (!exists(name)) return ws_error(WsErrc::NotFound, "no workspace named \"" + std::string(name) + "\"");
    if (auto r = fs_.remove(path_of(name)); !r) return r;
    if (default_name() == std::string(name)) return set_default(std::nullopt);
    return {};
}

WsResult<void> PresetStore::duplicate(std::string_view from, std::string_view to) {
    if (auto r = check_name(to); !r) return r;
    if (exists(to)) return ws_error(WsErrc::InvalidName, "a workspace named \"" + std::string(to) + "\" exists");
    auto p = load(from);
    if (!p) return Unexpected<WsError>(p.error());
    p->name = std::string(to);
    return save(*p, false);
}

WsResult<void> PresetStore::set_default(std::optional<std::string> name) {
    const std::string path = path_join(dir_, kDefaultFile);
    if (!name) {
        if (fs_.exists(path)) return fs_.remove(path);
        return {};
    }
    if (!exists(*name)) return ws_error(WsErrc::NotFound, "no workspace named \"" + *name + "\"");
    if (auto r = fs_.make_private_dir(dir_); !r) return r;
    return fs_.write_atomic(path, *name + "\n");
}

std::optional<std::string> PresetStore::default_name() const {
    ReadResult rr = fs_.read(path_join(dir_, kDefaultFile));
    if (rr.status != ReadStatus::Ok) return std::nullopt;
    std::string n = trim(std::move(rr.data));
    if (!valid_preset_name(n)) return std::nullopt;
    return n;
}

std::string PresetStore::free_name(std::string_view base) const {
    std::string b = valid_preset_name(base) ? std::string(base) : std::string("Workspace");
    if (!exists(b)) return b;
    for (int i = 2; i < 10000; ++i) {
        std::string n = b + " " + std::to_string(i);
        if (!exists(n)) return n;
    }
    return b;
}

}  // namespace kt::ws
