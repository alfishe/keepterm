#include "resources/report.h"

#include <cinttypes>
#include <cstdio>
#include <ctime>

namespace kt::resources {
namespace {

class Json {
public:
    std::string out;

    void raw(std::string_view s) { out += s; }
    void key(std::string_view k) {
        comma();
        quoted(k);
        out += ':';
        fresh_ = true;
    }
    void str(std::string_view s) {
        comma();
        quoted(s);
    }
    void quoted(std::string_view s) {
        out += '"';
        for (char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", c);
                        out += b;
                    } else {
                        out += ch;
                    }
            }
        }
        out += '"';
    }
    void num(uint64_t v) {
        comma();
        out += std::to_string(v);
    }
    void num(double v) {
        comma();
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", v);
        out += b;
    }
    void boolean(bool v) {
        comma();
        out += v ? "true" : "false";
    }
    void null() {
        comma();
        out += "null";
    }
    void opt(const std::optional<uint64_t>& v) { v ? num(*v) : null(); }
    void begin_obj() {
        comma();
        out += '{';
        fresh_ = true;
    }
    void end_obj() {
        out += '}';
        fresh_ = false;
    }
    void begin_arr() {
        comma();
        out += '[';
        fresh_ = true;
    }
    void end_arr() {
        out += ']';
        fresh_ = false;
    }

private:
    void comma() {
        if (!fresh_) out += ',';
        fresh_ = false;
    }
    bool fresh_ = true;
};

void metrics_json(Json& j, const Metrics& m, const ReportOptions& o) {
    j.begin_obj();
    j.key("main"); j.num(m.main);
    j.key("resident"); j.opt(m.resident);
    j.key("private_resident"); j.opt(m.private_resident);
    j.key("peak"); j.opt(m.peak);
    j.key("committed"); j.opt(m.committed);
    j.key("file_cache"); j.opt(m.file_cache);
    j.key("cpu_ns"); j.num(m.cpu_ns);
    j.key("cpu_percent");
    if (o.cpu && m.cpu_percent) {
        j.num(static_cast<double>(*m.cpu_percent));
    } else {
        j.null();
    }
    j.key("processes"); j.num(uint64_t{m.processes});
    j.key("threads"); j.num(uint64_t{m.threads});
    j.key("no_access"); j.num(uint64_t{m.no_access});
    j.key("estimated"); j.num(uint64_t{m.estimated});
    j.end_obj();
}

void node_json(Json& j, const ProcNode& n, const ReportOptions& o, bool session_node) {
    j.begin_obj();
    j.key("pid"); n.synthetic ? j.null() : j.num(uint64_t{n.key.pid});
    j.key("ppid"); n.synthetic ? j.null() : j.num(uint64_t{n.ppid});
    j.key("start_time"); n.synthetic ? j.null() : j.num(n.key.start_time);
    j.key("name"); j.str(n.name);
    j.key("role"); j.str(to_string(n.role));
    j.key("how");
    session_node && !n.synthetic ? j.str(to_string(n.how)) : j.null();
    j.key("reparented"); j.boolean(n.reparented);
    j.key("escaped"); j.boolean(n.escaped);
    j.key("no_access"); j.boolean(n.no_access);
    j.key("synthetic"); j.boolean(n.synthetic);
    std::optional<ProcessDetails> d;
    if (o.details && !n.synthetic) d = o.details(n.key);
    j.key("path");
    d && d->path ? j.str(*d->path) : j.null();
    j.key("cmdline");
    if (d && d->argv) {
        j.begin_arr();
        for (const auto& a : *d->argv) j.str(a);
        j.end_arr();
    } else {
        j.null();
    }
    j.key("self"); metrics_json(j, n.self, o);
    j.key("subtree"); metrics_json(j, n.subtree, o);
    j.key("children");
    j.begin_arr();
    for (const auto& c : n.children) node_json(j, c, o, session_node);
    j.end_arr();
    j.end_obj();
}

std::string iso_time(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

std::string pct(const Metrics& m, const ReportOptions& o) {
    if (!o.cpu || !m.cpu_percent) return "-";
    char b[16];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(*m.cpu_percent));
    return b;
}

std::string cpu_time(uint64_t ns) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2fs", static_cast<double>(ns) / 1e9);
    return b;
}

void node_text(std::string& out, const ProcNode& n, const ReportOptions& o, int depth) {
    char line[512];
    std::string label(static_cast<size_t>(depth) * 2, ' ');
    label += n.name;
    std::string flags;
    if (n.role != Role::Workload) flags += " [" + std::string(to_string(n.role)) + "]";
    if (n.reparented) flags += " reparented";
    if (n.escaped) flags += " escaped";
    if (n.no_access) flags += " no-access";
    std::snprintf(line, sizeof line, "%-40s %7s %11s %11s %11s %6s %9s %4u%s\n", label.c_str(),
                  n.synthetic ? "-" : std::to_string(n.key.pid).c_str(), format_bytes(n.self.main).c_str(),
                  n.children.empty() ? "" : format_bytes(n.subtree.main).c_str(),
                  n.self.resident ? format_bytes(*n.self.resident).c_str() : "-", pct(n.self, o).c_str(),
                  cpu_time(n.self.cpu_ns).c_str(), n.self.threads, flags.c_str());
    out += line;
    for (const auto& c : n.children) node_text(out, c, o, depth + 1);
}

void tree_header(std::string& out, const ReportOptions& o) {
    char line[256];
    std::snprintf(line, sizeof line, "%-40s %7s %11s %11s %11s %6s %9s %4s\n", "NAME", "PID",
                  std::string(o.main_metric).c_str(), "SUBTREE", std::string(o.resident_metric).c_str(), "CPU%",
                  "CPU", "THR");
    out += line;
}

}  // namespace

std::string format_bytes(uint64_t v) {
    char b[32];
    if (v >= 1024ull * 1024 * 1024) {
        std::snprintf(b, sizeof b, "%.2f GiB", static_cast<double>(v) / (1024.0 * 1024 * 1024));
    } else if (v >= 1024ull * 1024) {
        std::snprintf(b, sizeof b, "%.1f MiB", static_cast<double>(v) / (1024.0 * 1024));
    } else if (v >= 1024) {
        std::snprintf(b, sizeof b, "%.0f KiB", static_cast<double>(v) / 1024.0);
    } else {
        std::snprintf(b, sizeof b, "%" PRIu64 " B", v);
    }
    return b;
}

std::string to_json(const ResourceSnapshot& s, const ReportOptions& o) {
    Json j;
    j.begin_obj();
    j.key("json_version"); j.num(uint64_t{1});
    j.key("via"); j.str(o.via);
    j.key("taken_at"); j.str(iso_time(s.taken_at));
    j.key("interval_ms"); j.num(static_cast<uint64_t>(s.interval.count()));
    j.key("machines");
    j.begin_arr();
    for (const MachineSample& m : s.machines) {
        j.begin_obj();
        j.key("id"); j.null();  // the machine UUID of the state folder comes with the GUI state
        j.key("name"); j.str(m.display_name);
        j.key("os"); j.str(to_string(m.os));
        j.key("metric");
        j.begin_obj();
        j.key("main"); j.str(o.main_metric);
        j.key("resident"); j.str(o.resident_metric);
        j.end_obj();
        j.key("visible_processes"); j.num(uint64_t{m.visible_processes});
        j.key("terminal");
        j.begin_obj();
        j.key("gui"); metrics_json(j, m.terminal.gui, o);
        j.key("overhead"); metrics_json(j, m.terminal.hosts_and_agents, o);
        j.end_obj();
        bool exact = true;
        for (const auto& ss : m.sessions) exact = exact && ss.accuracy == Accuracy::Exact;
        j.key("totals");
        j.begin_obj();
        j.key("main"); j.num(m.total.main);
        j.key("accuracy"); j.str(to_string(exact ? Accuracy::Exact : Accuracy::BestEffort));
        j.key("processes"); j.num(uint64_t{m.total.processes});
        j.end_obj();
        j.key("sessions");
        j.begin_arr();
        for (const SessionSample& ss : m.sessions) {
            j.begin_obj();
            j.key("id"); j.str(ss.id);
            j.key("containment");
            j.begin_obj();
            j.key("kind"); j.str(to_string(ss.containment.kind));
            j.key("accuracy"); j.str(to_string(ss.containment.accuracy));
            j.end_obj();
            j.key("accuracy"); j.str(to_string(ss.accuracy));
            j.key("overhead"); metrics_json(j, ss.overhead, o);
            j.key("workload"); metrics_json(j, ss.workload, o);
            j.key("total"); metrics_json(j, ss.total, o);
            j.key("peak_sampled"); j.boolean(ss.peak_sampled);
            j.key("lifetime_cpu_ns"); j.opt(ss.lifetime_cpu_ns);
            j.key("collapsed"); j.boolean(ss.collapsed);
            j.key("tree");
            o.tree ? node_json(j, ss.root, o, true) : j.null();
            j.end_obj();
        }
        j.end_arr();
        j.key("forest");
        if (o.tree && !m.forest.empty()) {
            j.begin_arr();
            for (const auto& n : m.forest) node_json(j, n, o, false);
            j.end_arr();
        } else {
            j.null();
        }
        j.end_obj();
    }
    j.end_arr();
    j.end_obj();
    j.raw("\n");
    return std::move(j.out);
}

std::string to_text(const ResourceSnapshot& s, const ReportOptions& o) {
    std::string out;
    char line[512];
    for (const MachineSample& m : s.machines) {
        std::snprintf(line, sizeof line, "%s (%s), %u processes visible, main metric: %s\n",
                      m.display_name.c_str(), std::string(to_string(m.os)).c_str(), m.visible_processes,
                      std::string(o.main_metric).c_str());
        out += line;
        std::snprintf(line, sizeof line, "keepterm (this process): %s, %s %s\n",
                      format_bytes(m.terminal.gui.main).c_str(), std::string(o.resident_metric).c_str(),
                      m.terminal.gui.resident ? format_bytes(*m.terminal.gui.resident).c_str() : "-");
        out += line;
        for (const SessionSample& ss : m.sessions) {
            std::snprintf(line, sizeof line,
                          "session %s: %s%s, %u processes, %u threads, overhead %s, workload %s, peak %s%s, "
                          "cpu %s%s, containment %s, %s%s\n",
                          ss.id.c_str(), ss.accuracy == Accuracy::Exact ? "" : "~",
                          format_bytes(ss.total.main).c_str(), ss.total.processes, ss.total.threads,
                          format_bytes(ss.overhead.main).c_str(), format_bytes(ss.workload.main).c_str(),
                          ss.total.peak ? format_bytes(*ss.total.peak).c_str() : "-",
                          ss.peak_sampled ? " (sampled)" : "", cpu_time(ss.total.cpu_ns).c_str(),
                          o.cpu && ss.total.cpu_percent ? (" (" + pct(ss.total, o) + "%)").c_str() : "",
                          std::string(to_string(ss.containment.kind)).c_str(),
                          std::string(to_string(ss.accuracy)).c_str(),
                          ss.total.no_access ? (", no access " + std::to_string(ss.total.no_access)).c_str() : "");
            out += line;
            if (o.tree) {
                tree_header(out, o);
                node_text(out, ss.root, o, 0);
            }
        }
        if (o.tree && !m.forest.empty()) {
            tree_header(out, o);
            for (const auto& n : m.forest) node_text(out, n, o, 0);
        }
    }
    return out;
}

}  // namespace kt::resources
