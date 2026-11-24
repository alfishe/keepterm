// The folder of workspace presets (`<config>/workspaces`): one readable JSON file per name.
// Core code over IFileSystem. Per-project layouts are YAML project files (project_file.h).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/error.h"
#include "workspace/fs.h"
#include "workspace/presets.h"

namespace kt::ws {

struct PresetInfo {
    std::string name;
    std::size_t projects = 0, tabs = 0, panes = 0;
    bool agents = false;                 // holds agent resume records
    bool is_default = false;
    std::int64_t modified_unix_ms = 0;
    std::string error;                   // the file does not parse (listed so it can be deleted)
};

class PresetStore {
public:
    // `dir`: the folder of the files (created on the first save).
    PresetStore(IFileSystem& fs, std::string dir) : fs_(fs), dir_(std::move(dir)) {}

    const std::string& dir() const { return dir_; }
    std::string path_of(std::string_view name) const;   // "<dir>/<name>.json"

    // Sorted by name (case-insensitive ASCII).
    std::vector<PresetInfo> list() const;
    bool exists(std::string_view name) const;
    // Writes the preset under preset.name; `overwrite`=false fails with AlreadyExists-like
    // InvalidName when the name is taken.
    WsResult<void> save(const Preset& preset, bool overwrite = true);
    WsResult<Preset> load(std::string_view name) const;
    WsResult<void> rename(std::string_view from, std::string_view to);
    WsResult<void> remove(std::string_view name);
    WsResult<void> duplicate(std::string_view from, std::string_view to);
    // The preset "Open Workspace" lists first and the launch prompt offers. nullopt clears it.
    WsResult<void> set_default(std::optional<std::string> name);
    std::optional<std::string> default_name() const;
    // "backend", "backend 2", ... : a name not taken yet.
    std::string free_name(std::string_view base) const;

private:
    IFileSystem& fs_;
    std::string dir_;
};

}  // namespace kt::ws
