#include "session/suspend.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "base/log.h"
#include "session/client.h"
#include "session/registry.h"

namespace kt::session {

namespace {

using Clock = std::chrono::steady_clock;

struct Outcome {
    std::atomic<int> state{0};   // 0 running, 1 saved, 2 killed, 3 failed
};

bool running(const RegistryRecord& r) { return r.host.pid && os::process_matches(r.host.pid, r.host.start_time); }

void wait_gone(const RegistryRecord& r, Clock::time_point until) {
    while (running(r) && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

void end_one(const Registry& reg, const DiscoveredSession& d, Clock::time_point deadline, Outcome& out) {
    const RegistryRecord& r = d.record;
    if (d.state == HostState::Live) {
        proto::Kill k;
        k.mode = proto::KillMode::Suspend;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        auto res = request_kill(r.endpoint, k, static_cast<int>(std::clamp<long long>(left / 3, 200, 1500)));
        if (res && res->ok) wait_gone(r, deadline);
    }
    if (!running(r)) {
        out.state = 1;
        return;
    }
    (void)os::kill_process(r.host.pid, r.host.start_time);
    wait_gone(r, Clock::now() + std::chrono::milliseconds(500));
    if (running(r)) {
        out.state = 3;
        return;
    }
    // Ended from outside: its record and socket file stay behind; the folder is kept.
    reg.remove(r.id);
#ifndef _WIN32
    (void)os::remove_file(os::endpoint_name(reg.paths(), r.id));
    (void)os::remove_file(os::endpoint_name(reg.paths(), r.id, true));
#endif
    out.state = 2;
}

}  // namespace

SuspendReport suspend_all_hosts(const os::Paths& paths, const std::string& last, std::chrono::milliseconds budget) {
    const auto start = Clock::now();
    Registry reg(paths);
    std::vector<DiscoveredSession> all = reg.discover();
    std::vector<DiscoveredSession> first, tail;
    for (auto& d : all) {
        if (d.state == HostState::Stale || !running(d.record)) continue;
        (d.record.id.str() == last ? tail : first).push_back(std::move(d));
    }
    SuspendReport rep;
    auto run_batch = [&](std::vector<DiscoveredSession>& batch, Clock::time_point deadline) {
        // One thread per host: a host that hangs costs its own budget only. The threads are
        // detached with shared state, so a stuck OS call cannot hold the caller past the budget.
        std::vector<std::shared_ptr<Outcome>> outs;
        for (auto& d : batch) {
            auto o = std::make_shared<Outcome>();
            outs.push_back(o);
            std::thread([reg, d, deadline, o] { end_one(reg, d, deadline, *o); }).detach();
        }
        const auto hard = deadline + std::chrono::milliseconds(1000);
        for (;;) {
            bool done = true;
            for (auto& o : outs) done = done && o->state.load() != 0;
            if (done || Clock::now() >= hard) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (auto& o : outs) {
            switch (o->state.load()) {
                case 1: ++rep.saved; break;
                case 2: ++rep.killed; break;
                default: ++rep.failed; break;
            }
        }
    };
    const auto end = start + budget;
    run_batch(first, tail.empty() ? end : end - budget / 4);
    run_batch(tail, end);
    log::info("session", "suspend: " + std::to_string(rep.saved) + " saved, " + std::to_string(rep.killed) + " ended, " +
                             std::to_string(rep.failed) + " still running");
    return rep;
}

}  // namespace kt::session
