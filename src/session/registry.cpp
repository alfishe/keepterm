// Discovery.

#include "session/registry.h"

#include <algorithm>
#include <cstdlib>
#include <future>
#include <set>

namespace kt::session {

json::Value to_json(const RegistryRecord& r) {
    auto v = json::Value::object();
    v.set("schema", json::Value::integer(r.schema));
    v.set("session", json::Value::string(r.id.str()));
    v.set("host_pid", json::Value::integer(r.host.pid));
    v.set("host_start", json::Value::string(std::to_string(r.host.start_time)));
    v.set("boot_id", json::Value::string(r.boot_id));
    v.set("endpoint", json::Value::string(r.endpoint));
    v.set("build_key", json::Value::string(r.build_key));
    v.set("version", json::Value::string(r.version));
    v.set("level_min", json::Value::integer(r.level_min));
    v.set("level_max", json::Value::integer(r.level_max));
    v.set("profile", json::Value::string(r.profile));
    v.set("created_unix_ms", json::Value::string(std::to_string(r.created_unix_ms)));
    return v;
}

Result<RegistryRecord> registry_record_from_json(const json::Value& v) {
    if (v.kind() != json::Value::Kind::Object) return make_error(Errc::corrupt, "registry record is not an object");
    RegistryRecord r;
    r.schema = static_cast<uint16_t>(v.int_or("schema", 0));
    if (r.schema != 1) return make_error(Errc::not_supported, "registry record schema");
    auto id = SessionId::parse(v.str_or("session"));
    if (!id) return make_error(Errc::corrupt, "registry record without a session id");
    r.id = *id;
    r.host.pid = static_cast<uint32_t>(v.uint_or("host_pid", 0));
    r.host.start_time = std::strtoull(v.str_or("host_start", "0").c_str(), nullptr, 10);
    r.boot_id = v.str_or("boot_id");
    r.endpoint = v.str_or("endpoint");
    r.build_key = v.str_or("build_key");
    r.version = v.str_or("version");
    r.level_min = static_cast<uint16_t>(v.uint_or("level_min", 1));
    r.level_max = static_cast<uint16_t>(v.uint_or("level_max", 1));
    r.profile = v.str_or("profile");
    r.created_unix_ms = std::strtoull(v.str_or("created_unix_ms", "0").c_str(), nullptr, 10);
    if (r.endpoint.empty()) return make_error(Errc::corrupt, "registry record without an endpoint");
    return r;
}

std::string_view to_string(HostState s) {
    switch (s) {
        case HostState::Live: return "live";
        case HostState::Unresponsive: return "unresponsive";
        case HostState::Stale: return "stale";
        case HostState::Incompatible: return "incompatible";
    }
    return "?";
}

int NegotiatedInfo::gui_clients() const {
    for (const auto& c : caps) {
        if (c.rfind("gui-clients=", 0) == 0) return std::atoi(c.c_str() + 12);
    }
    return 0;
}

// 
//   1. boot id differs (exact on Linux)            -> Stale
//   2. pid gone or start time differs              -> Stale
//   3. HELLO within the timeout                    -> Live, or Incompatible without a common level
//   4. process alive, no HELLO                     -> Unresponsive
HostState probe_state(const RegistryRecord& rec, const ProbeFacts& f, std::optional<NegotiatedInfo>* out) {
    if (f.boot_id) {
        const os::BootId b = f.boot_id();
        if (b.exact && !rec.boot_id.empty() && os::hex(b.bytes) != rec.boot_id) return HostState::Stale;
    }
    if (rec.host.pid != 0 && f.start_time) {
        const uint64_t st = f.start_time(rec.host.pid);
        if (st == 0 || (rec.host.start_time != 0 && st != rec.host.start_time)) return HostState::Stale;
    }
    if (!f.hello) return HostState::Unresponsive;
    auto h = f.hello(rec);
    if (!h) {
        // No record pid to check (endpoint-only discovery): nothing answers, nothing to keep.
        return rec.host.pid == 0 ? HostState::Stale : HostState::Unresponsive;
    }
    if (rec.host.pid != 0 && h->second != 0 && h->second != rec.host.pid) {
        // Someone else serves the endpoint; the record's host is not reachable through it.
        return HostState::Unresponsive;
    }
    NegotiatedInfo ni;
    ni.build_key = h->first.build_key;
    ni.version = h->first.version;
    ni.caps = h->first.caps;
    ni.pid = h->first.pid;
    const auto level = proto::negotiate_level(proto::kLevelMin, proto::kLevelMax, h->first.level_min, h->first.level_max);
    if (out) *out = ni;
    if (!level) return HostState::Incompatible;
    if (out) (*out)->level = *level;
    return HostState::Live;
}

ProbeFacts real_probe_facts(int timeout_ms) {
    ProbeFacts f;
    f.boot_id = [] { return os::boot_id(); };
    f.start_time = [](uint32_t pid) { return os::process_start_time(pid); };
    f.hello = [timeout_ms](const RegistryRecord& rec) -> std::optional<std::pair<proto::Hello, uint32_t>> {
        auto c = Conn::connect(rec.endpoint, timeout_ms);
        if (!c) return std::nullopt;
        // A level-less HELLO exchange: a rejection still tells us the host is there.
        std::vector<std::byte> out;
        proto::encode(make_hello(proto::Role::Cli, std::nullopt), out);
        if (!(*c)->send(out)) return std::nullopt;
        auto fr = (*c)->recv(timeout_ms);
        if (!fr || !*fr) return std::nullopt;
        proto::Hello h;
        if ((*fr)->type == static_cast<uint16_t>(proto::Type::HelloReject)) {
            proto::HelloReject rej;
            if (!proto::decode((*fr)->payload, rej)) return std::nullopt;
            h.level_min = rej.level_min;
            h.level_max = rej.level_max;
            return std::make_pair(h, (*c)->peer_pid());
        }
        if ((*fr)->type != static_cast<uint16_t>(proto::Type::Hello)) return std::nullopt;
        if (!proto::decode((*fr)->payload, h) || h.magic != proto::kMagic) return std::nullopt;
        return std::make_pair(h, (*c)->peer_pid());
    };
    return f;
}

std::string Registry::record_path(const SessionId& id) const { return paths_.run_sessions + "/" + id.str() + ".json"; }

Result<void> Registry::write(const RegistryRecord& r) const {
    return os::write_file_atomic(record_path(r.id), to_json(r).dump(true) + "\n");
}

Result<RegistryRecord> Registry::read(const SessionId& id) const {
    auto text = os::read_file(record_path(id), 64 << 10);
    if (!text) return Unexpected(text.error());
    auto v = json::parse(*text);
    if (!v) return Unexpected(v.error());
    auto r = registry_record_from_json(*v);
    if (!r) return r;
    if (r->id != id) return make_error(Errc::corrupt, "registry record id does not match its file name");
    return r;
}

void Registry::remove(const SessionId& id) const { (void)os::remove_file(record_path(id)); }

std::vector<DiscoveredSession> Registry::discover() const {
    std::vector<DiscoveredSession> items;
    std::set<SessionId> seen;
    for (const std::string& name : os::list_dir(paths_.run_sessions)) {
        if (name.size() != SessionId::kLength + 5 || name.substr(SessionId::kLength) != ".json") continue;
        auto id = SessionId::parse(name.substr(0, SessionId::kLength));
        if (!id) continue;
        DiscoveredSession d;
        auto r = read(*id);
        if (!r) {
            // Unreadable record: probe by endpoint only.
            d.record.id = *id;
            d.record.endpoint = os::endpoint_name(paths_, *id);
            d.from_endpoint_only = true;
        } else {
            d.record = *r;
        }
        seen.insert(*id);
        items.push_back(std::move(d));
    }
    for (const SessionId& id : os::list_endpoints(paths_)) {
        if (seen.count(id)) continue;
        DiscoveredSession d;
        d.record.id = id;
        d.record.endpoint = os::endpoint_name(paths_, id);
        d.from_endpoint_only = true;
        items.push_back(std::move(d));
    }
    const ProbeFacts facts = real_probe_facts();
    // At most 16 probes at a time.
    for (std::size_t base = 0; base < items.size(); base += 16) {
        std::vector<std::future<void>> fs;
        for (std::size_t i = base; i < std::min(items.size(), base + 16); ++i) {
            fs.push_back(std::async(std::launch::async, [&, i] {
                items[i].state = probe_state(items[i].record, facts, &items[i].hello);
            }));
        }
        for (auto& f : fs) f.get();
    }
    std::sort(items.begin(), items.end(), [](const DiscoveredSession& a, const DiscoveredSession& b) {
        return a.record.created_unix_ms != b.record.created_unix_ms ? a.record.created_unix_ms < b.record.created_unix_ms
                                                                    : a.record.id < b.record.id;
    });
    return items;
}

std::optional<DiscoveredSession> Registry::find(const SessionId& id, int timeout_ms) const {
    DiscoveredSession d;
    auto r = read(id);
    if (r) {
        d.record = *r;
    } else {
        d.record.id = id;
        d.record.endpoint = os::endpoint_name(paths_, id);
        d.from_endpoint_only = true;
        if (!os::exists(d.record.endpoint)) {
#ifndef _WIN32
            return std::nullopt;
#endif
        }
    }
    d.state = probe_state(d.record, real_probe_facts(timeout_ms), &d.hello);
    return d;
}

std::size_t Registry::sweep_stale() const {
    std::size_t n = 0;
    for (const auto& d : discover()) {
        if (d.state != HostState::Stale) continue;
        remove(d.record.id);
#ifndef _WIN32
        (void)os::remove_file(os::endpoint_name(paths_, d.record.id));
        (void)os::remove_file(os::endpoint_name(paths_, d.record.id, true));
#endif
        ++n;
    }
    return n;
}

}  // namespace kt::session
