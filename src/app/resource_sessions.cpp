#include "app/resource_sessions.h"

#include <future>

#include "session/client.h"
#include "session/registry.h"

namespace kt::app {

std::vector<resources::SessionProcessInfo> live_session_processes() {
    std::vector<resources::SessionProcessInfo> out;
    auto paths = session::os::paths();
    if (!paths) return out;
    const session::Registry reg(*paths);
    std::vector<std::pair<std::string, std::future<Result<session::proto::ProcessInfo>>>> asks;
    for (const session::DiscoveredSession& d : reg.discover()) {
        if (d.state != session::HostState::Live) continue;
        const std::string endpoint = d.record.endpoint;
        asks.emplace_back(d.record.id.str(), std::async(std::launch::async, [endpoint] {
                              return session::request_process_info(endpoint);
                          }));
    }
    for (auto& [id, f] : asks) {
        auto pi = f.get();
        if (pi) out.push_back(session_process_info(id, *pi));
    }
    return out;
}

}  // namespace kt::app
