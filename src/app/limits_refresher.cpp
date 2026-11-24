#include "app/limits_refresher.h"

#include <algorithm>

#include "agents/limits_probe.h"
#include "base/log.h"

namespace kt::app {

namespace {
constexpr std::chrono::seconds kProbeTimeout{45};
}

LimitsRefresher::LimitsRefresher(Post post, Done done) : st_(std::make_shared<State>()) {
    st_->post = std::move(post);
    st_->done = std::move(done);
    std::thread([st = st_] { run(st); }).detach();
}

LimitsRefresher::~LimitsRefresher() {
    {
        std::lock_guard lk(st_->mu);
        st_->stop = true;
    }
    st_->cv.notify_all();
}

bool LimitsRefresher::request(const std::string& agent, std::chrono::seconds min_gap) {
    if (!agents::limits_probe_for(agent)) return false;
    std::lock_guard lk(st_->mu);
    if (std::find(st_->queue.begin(), st_->queue.end(), agent) != st_->queue.end()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (auto it = st_->last.find(agent); it != st_->last.end() && now - it->second < min_gap) return true;
    st_->last[agent] = now;   // the start counts: a slow or failing probe is not repeated at once
    st_->queue.push_back(agent);
    st_->cv.notify_one();
    return true;
}

void LimitsRefresher::run(const std::shared_ptr<State>& st) {
    for (;;) {
        std::string agent;
        {
            std::unique_lock lk(st->mu);
            st->cv.wait(lk, [&] { return st->stop || !st->queue.empty(); });
            if (st->stop) return;
            agent = st->queue.front();
        }
        const agents::LimitsProbe* probe = agents::limits_probe_for(agent);
        auto r = probe ? agents::run_limits_probe(*probe, kProbeTimeout)
                       : Result<std::vector<agents::RateLimit>>(make_error(Errc::not_supported, "no probe"));
        const auto at = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        {
            std::lock_guard lk(st->mu);
            if (!st->queue.empty()) st->queue.pop_front();
            if (st->stop) return;   // the owner is gone: its loop may be too
            std::string& last = st->last_error[agent];
            if (!r) {
                if (last != r.error().what) log::warn("agents", "limits of " + agent + ": " + r.error().what);
                last = r.error().what;
            } else {
                last.clear();
            }
            // Posted under the lock: once the destructor set `stop`, nothing reaches the owner.
            if (r) {
                Done done = st->done;
                st->post([done, agent, limits = std::move(*r), at]() mutable { done(agent, std::move(limits), at); });
            }
        }
    }
}

}  // namespace kt::app
