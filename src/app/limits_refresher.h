// Runs the agents' limits probes (agents/limits_probe.h) off the UI thread, one at a time.
//
// The app asks for an agent's limits on a timer while a pane runs it and when its popover opens;
// a request for an agent whose probe ran less than `min_gap` ago, or is running, is dropped. The
// result goes to `done` on the UI thread (through `post`).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agents/context.h"

namespace kt::app {

class LimitsRefresher {
public:
    using Post = std::function<void(std::function<void()>)>;
    using Done = std::function<void(const std::string& agent, std::vector<agents::RateLimit> limits, std::int64_t at_unix_ms)>;

    LimitsRefresher(Post post, Done done);
    ~LimitsRefresher();
    LimitsRefresher(const LimitsRefresher&) = delete;
    LimitsRefresher& operator=(const LimitsRefresher&) = delete;

    // Asks for `agent`'s limits unless its probe ran less than `min_gap` ago or is queued. False
    // when the agent has no probe.
    bool request(const std::string& agent, std::chrono::seconds min_gap);

private:
    // Shared with the worker: on quit the worker is detached (a probe may run for seconds) and
    // finishes alone.
    struct State {
        Post post;
        Done done;
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::string> queue;
        std::map<std::string, std::chrono::steady_clock::time_point> last;
        std::map<std::string, std::string> last_error;   // logged once until it changes
        bool stop = false;
    };
    static void run(const std::shared_ptr<State>& st);

    std::shared_ptr<State> st_;
};

}  // namespace kt::app
