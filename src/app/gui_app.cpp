#include "app/gui_app.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include "app/main_window.h"
#include "base/log.h"
#include "session/os.h"
#include "render/render_select.h"

namespace kt::app {

namespace {

constexpr float kMinFontPt = 4.0f, kMaxFontPt = 96.0f;   // font.size range

int usage() {
    std::fprintf(stderr, "%s", gui_usage());
    return 2;
}

}  // namespace

const char* gui_usage() {
    return "usage: keepterm [--gui] [--local | --clean] [--font-size PT] [--config FILE] [--set KEY=VALUE]...\n"
           "                [--renderer NAME] [--allow-software-adapter] [--verbose] [-- <program> [args...]]\n";
}

int gui_main(int argc, char** argv, int i) {
    MainWindowOptions opts;
    for (; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--") {
            ++i;
            if (i >= argc) return usage();
            for (; i < argc; ++i) opts.program.emplace_back(argv[i]);
            opts.local = true;   // a program of the command line runs in a local PTY
            break;
        }
        if (a == "--font-size" && i + 1 < argc) {
            const float pt = std::strtof(argv[++i], nullptr);
            if (!(pt >= kMinFontPt && pt <= kMaxFontPt)) return usage();
            opts.gui.font_pt = pt;
        } else if (a == "--config" && i + 1 < argc) {
            opts.gui.config = argv[++i];
        } else if (a == "--set" && i + 1 < argc) {
            opts.gui.sets.emplace_back(argv[++i]);
        } else if (a == "--renderer" && i + 1 < argc) {
            opts.gui.renderer = argv[++i];
            if (opts.gui.renderer != "auto" && !render::parse_backend(opts.gui.renderer)) return usage();
        } else if (a == "--allow-software-adapter") {
            opts.gui.allow_software_adapter = true;
        } else if (a == "--verbose") {
            opts.gui.verbose = true;
        } else if (a == "--after-pid" && i + 1 < argc) {
            // keepterm restart: the old window still saves and quits. Its single-instance
            // endpoint and state file are ours once it is gone (at most 30 s).
            const std::string v = argv[++i];
            const auto colon = v.find(':');
            const auto pid = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
            const uint64_t start = colon == std::string::npos ? 0 : std::strtoull(v.c_str() + colon + 1, nullptr, 10);
            for (int waited = 0; pid && waited < 600; ++waited) {
                const bool alive = start ? session::os::process_matches(pid, start) : session::os::process_start_time(pid) != 0;
                if (!alive) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        } else if (a == "--local") {
            opts.local = true;
        } else if (a == "--clean") {
            opts.clean = true;
        } else if ((a == "--attach" || a == "--restore") && i + 1 < argc) {
            // Old single-pane window files: the workspace imports their panes at start.
            ++i;
            log::warn("gui", std::string(a) + " is no longer needed: the workspace restores every saved pane");
        } else {
            return usage();
        }
    }
#ifndef KT_HAVE_SESSIONS
    opts.local = true;
#endif
    if (opts.gui.verbose) log::set_level(log::Level::info);
    auto p = platform::create_platform();
    if (!p) {
        std::fprintf(stderr, "keepterm: no display: %s\n", p.error().what.c_str());
        return 1;
    }
    return run_main_window(**p, std::move(opts));
}

}  // namespace kt::app
