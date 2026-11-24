#include "config/cli.h"

#include <cstdio>
#include <string>
#include <string_view>

#include "config/config.h"
#include "platform/paths.h"

namespace kt::config {
namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: keepterm --config dump [--defaults] [--no-origins] [--profile NAME] [--file PATH]"
                 " [--set key=value]...\n"
                 "       keepterm --config check [PATH] [--profile NAME]\n"
                 "       keepterm --config path [--profile NAME]\n");
    return 2;
}

void print_diagnostics(const Config& cfg, FILE* out) {
    for (const auto& d : cfg.diagnostics()) std::fprintf(out, "%s\n", format(d).c_str());
}

}  // namespace

int run_cli(int argc, char** argv, int first) {
    if (first >= argc) return usage();
    const std::string_view cmd = argv[first];
    LoadOptions opts;
    DumpOptions dopts;
    std::optional<std::string> positional;
    for (int i = first + 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--defaults") {
            dopts.defaults_only = true;
        } else if (a == "--no-origins") {
            dopts.origins = false;
        } else if (a == "--profile") {
            const char* v = next();
            if (!v || !platform::paths::valid_profile_name(v)) {
                std::fprintf(stderr, "keepterm: invalid profile name (allowed: [a-z0-9-]{1,32})\n");
                return 2;
            }
            opts.profile = v;
        } else if (a == "--file") {
            const char* v = next();
            if (!v) return usage();
            opts.file = platform::paths::from_utf8(v);
        } else if (a == "--set") {
            const char* v = next();
            if (!v) return usage();
            opts.cli_sets.emplace_back(v);
        } else if (!a.starts_with("--") && !positional) {
            positional = std::string(a);
        } else {
            return usage();
        }
    }

    if (cmd == "path") {
        for (const auto& f : user_files(opts.profile)) std::printf("%s\n", platform::paths::to_utf8(f).c_str());
        return 0;
    }
    if (cmd == "dump") {
        if (positional) return usage();
        const Config cfg = dopts.defaults_only ? Config() : Config::load(opts);
        std::fputs(dump(cfg, dopts).c_str(), stdout);
        print_diagnostics(cfg, stderr);
        return 0;
    }
    if (cmd == "check") {
        if (positional) opts.file = platform::paths::from_utf8(*positional);
        if (opts.file) {
            std::error_code ec;
            if (!std::filesystem::exists(*opts.file, ec)) {
                std::fprintf(stderr, "%s: error: file not found\n", display_path(*opts.file).c_str());
                return 1;
            }
        }
        const Config cfg = Config::load(opts);
        print_diagnostics(cfg, stdout);
        std::size_t errors = 0, warnings = 0;
        for (const auto& d : cfg.diagnostics()) {
            if (d.level == Diagnostic::Level::Error) ++errors;
            else if (d.level == Diagnostic::Level::Warning) ++warnings;
        }
        std::printf("%zu error(s), %zu warning(s)\n", errors, warnings);
        return errors ? 1 : 0;
    }
    return usage();
}

}  // namespace kt::config
