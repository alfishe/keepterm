#include "resources/cli.h"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "resources/collector.h"
#include "resources/report.h"

namespace kt::resources {
namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: keepterm --resources [--pid N [--containment job:<name>|cgroup:<path>]] [--tree] [--json]\n"
                 "                            [--no-cpu] [--cmdline] [--stats]\n");
    return 2;
}

}  // namespace

int resources_cli(int argc, char** argv, int first, const SessionLister& sessions) {
    bool tree = false, json = false, cpu = true, cmdline = false, stats = false;
    std::optional<uint32_t> pid;
    Containment containment;
    for (int i = first; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--tree") {
            tree = true;
        } else if (a == "--json") {
            json = true;
        } else if (a == "--no-cpu") {
            cpu = false;
        } else if (a == "--cmdline") {
            cmdline = true;
        } else if (a == "--stats") {
            stats = true;
        } else if (a == "--pid" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            uint32_t p = 0;
            auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), p);
            if (ec != std::errc() || end != v.data() + v.size() || p == 0) return usage();
            pid = p;
        } else if (a == "--containment" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (v.starts_with("job:")) {
                containment = {ContainmentKind::Job, std::string(v.substr(4)), Accuracy::Exact};
            } else if (v.starts_with("cgroup:")) {
                containment = {ContainmentKind::Cgroup, std::string(v.substr(7)), Accuracy::Exact};
            } else {
                return usage();
            }
        } else {
            return usage();
        }
    }
    if (containment.kind != ContainmentKind::None && !pid) return usage();

    CollectorConfig cfg;
    cfg.forest = tree && !pid;
    cfg.detail = true;
    cfg.interval = std::chrono::milliseconds(500);
    Collector col(cfg);
    if (pid) {
        SessionProcessInfo s;
        s.id = "pid:" + std::to_string(*pid);
        s.root = ProcessKey{*pid, 0};
        s.containment = containment;
        col.set_sessions({s});
    } else if (sessions) {
        col.set_sessions(sessions());
    }
    auto snap = col.tick();
    if (snap && cpu) {
        // CPU % needs two samples.
        std::this_thread::sleep_for(cfg.interval);
        snap = col.tick();
    }
    if (!snap) {
        std::fprintf(stderr, "keepterm: resources: %s\n", snap.error().what.c_str());
        return 1;
    }
    const ResourceSnapshot& rs = **snap;
    if (pid && (rs.machines.empty() || rs.machines[0].sessions.empty() ||
                rs.machines[0].sessions[0].root.synthetic))
    {
        std::fprintf(stderr, "keepterm: resources: process %u not found\n", *pid);
        return 1;
    }

    ReportOptions ro;
    ro.tree = tree || pid.has_value();
    ro.cpu = cpu;
    ro.main_metric = col.source().main_metric_name();
    ro.resident_metric = col.source().resident_metric_name();
    if (cmdline) ro.details = [&col](const ProcessKey& k) { return col.source().details(k); };
    const std::string out = json ? to_json(rs, ro) : to_text(rs, ro);
    std::fwrite(out.data(), 1, out.size(), stdout);
    if (stats) {
        const CollectorStats st = col.stats();
        std::fprintf(stderr, "stats: ticks=%llu measured=%u last_tick_cpu_us=%llu last_tick_wall_us=%llu\n",
                     static_cast<unsigned long long>(st.ticks), st.last_measured,
                     static_cast<unsigned long long>(st.last_cpu_ns / 1000),
                     static_cast<unsigned long long>(st.last_wall_ns / 1000));
    }
    if (!col.source().extended_status().empty()) {
        std::fprintf(stderr, "keepterm: resources: %s\n", std::string(col.source().extended_status()).c_str());
    }
    return 0;
}

}  // namespace kt::resources
