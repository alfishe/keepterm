// Discovery: registry records and endpoint probing.
//
// One record file per session in <runtime>/sessions/<id>.json, written once by the host with
// temp + rename and deleted at Ended. The record is a hint; the endpoint and HELLO are the truth.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/error.h"
#include "session/ids.h"
#include "session/json.h"
#include "session/os.h"
#include "session/protocol.h"
#include "session/client.h"

namespace kt::session {

struct ProcessKey {
    uint32_t pid = 0;
    uint64_t start_time = 0;
    auto operator<=>(const ProcessKey&) const = default;
};

struct RegistryRecord {
    uint16_t schema = 1;
    SessionId id;
    ProcessKey host;
    std::string boot_id;    // hex
    std::string endpoint;
    std::string build_key, version;
    uint16_t level_min = proto::kLevelMin, level_max = proto::kLevelMax;
    std::string profile;
    uint64_t created_unix_ms = 0;
};

json::Value to_json(const RegistryRecord&);
Result<RegistryRecord> registry_record_from_json(const json::Value&);

enum class HostState : uint8_t { Live, Unresponsive, Stale, Incompatible };
std::string_view to_string(HostState);

// What the probe saw in the host's HELLO.
struct NegotiatedInfo {
    uint16_t level = 0;
    std::string build_key, version;
    std::vector<std::string> caps;
    uint32_t pid = 0;
    // The host lists "gui-clients=<n>" in its caps: attached GUI clients right now.
    int gui_clients() const;
};

struct DiscoveredSession {
    RegistryRecord record;
    HostState state = HostState::Stale;
    std::optional<NegotiatedInfo> hello;
    bool from_endpoint_only = false;   // no record file, found by endpoint enumeration
};

// The host probe state machine over injectable facts, so tests can drive it with a
// fake process table, boot id and HELLO.
struct ProbeFacts {
    std::function<os::BootId()> boot_id;
    std::function<uint64_t(uint32_t pid)> start_time;   // 0 = no such process
    // Connects and runs HELLO within the timeout; nullopt = no answer.
    std::function<std::optional<std::pair<proto::Hello, uint32_t /*peer pid*/>>(const RegistryRecord&)> hello;
};
HostState probe_state(const RegistryRecord&, const ProbeFacts&, std::optional<NegotiatedInfo>* hello_out = nullptr);
ProbeFacts real_probe_facts(int timeout_ms = 200);

class Registry {
public:
    explicit Registry(os::Paths paths) : paths_(std::move(paths)) {}

    std::string record_path(const SessionId& id) const;
    Result<void> write(const RegistryRecord&) const;
    Result<RegistryRecord> read(const SessionId&) const;
    void remove(const SessionId&) const;

    // Probes every record (and endpoints without a record) in parallel, up to 16 at a time.
    std::vector<DiscoveredSession> discover() const;
    std::optional<DiscoveredSession> find(const SessionId&, int timeout_ms = 200) const;
    // Removes Stale records and their socket files. Never touches Live or Unresponsive ones.
    std::size_t sweep_stale() const;

    const os::Paths& paths() const { return paths_; }

private:
    os::Paths paths_;
};


}  // namespace kt::session
