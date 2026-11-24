// Command-line options of the GUI role that are not about which session the window shows.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config/config.h"
#include "session/pane_config.h"

namespace kt::app {

struct GuiOptions {
    std::optional<float> font_pt;          // --font-size: wins over font.size
    std::optional<std::string> config;     // --config <file>
    std::vector<std::string> sets;         // --set key=value, in order
    std::string renderer;                  // --renderer <name>: wins over `renderer` (diagnostics)
    bool allow_software_adapter = false;   // --allow-software-adapter: GPU on llvmpipe / WARP (tests)
    bool verbose = false;                  // --verbose: info log lines on stderr

    config::LoadOptions load_options() const { return session::load_options(config, sets); }

    // The same config for a session host or another window.
    std::vector<std::string> config_flags() const {
        std::vector<std::string> out;
        if (config) {
            out.emplace_back("--config");
            out.push_back(*config);
        }
        for (const auto& s : sets) {
            out.emplace_back("--set");
            out.push_back(s);
        }
        return out;
    }
};

}  // namespace kt::app
