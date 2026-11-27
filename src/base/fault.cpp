#include "base/fault.h"

#ifdef KT_FAULTS

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace kt::fault {

namespace {

enum class Action { abort, fail, hang, delay };

struct Rule {
    std::string name;
    Action action = Action::fail;
    uint64_t delay_ms = 0;
    uint64_t from = 1, to = UINT64_MAX;  // hits
    double p = -1;                       // >= 0: by probability
};

struct State {
    std::mutex mu;
    bool loaded = false;
    std::vector<Rule> rules;
    std::map<std::string, uint64_t, std::less<>> counts;
    std::mt19937_64 rng{0x6b74u};
};

State& state() {
    static State* s = new State;  // never destroyed: points may be hit during exit
    return *s;
}

uint64_t to_u64(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

std::vector<Rule> parse(std::string_view spec) {
    std::vector<Rule> out;
    while (!spec.empty()) {
        const auto semi = spec.find(';');
        std::string_view item = spec.substr(0, semi);
        spec = semi == std::string_view::npos ? std::string_view() : spec.substr(semi + 1);
        const auto colon = item.find(':');
        if (item.empty() || colon == std::string_view::npos || colon == 0) continue;
        Rule r;
        r.name = std::string(item.substr(0, colon));
        std::string_view act = item.substr(colon + 1), hits;
        if (const auto at = act.find('@'); at != std::string_view::npos) {
            hits = act.substr(at + 1);
            act = act.substr(0, at);
        }
        if (act == "abort") r.action = Action::abort;
        else if (act == "fail") r.action = Action::fail;
        else if (act == "hang") r.action = Action::hang;
        else if (act.rfind("delay=", 0) == 0) {
            r.action = Action::delay;
            r.delay_ms = to_u64(act.substr(6));
        } else {
            continue;
        }
        if (hits.rfind("p=", 0) == 0) {
            r.p = std::strtod(std::string(hits.substr(2)).c_str(), nullptr);
        } else if (!hits.empty()) {
            const auto dash = hits.find('-');
            r.from = to_u64(hits.substr(0, dash));
            r.to = dash == std::string_view::npos ? r.from : to_u64(hits.substr(dash + 1));
        }
        out.push_back(std::move(r));
    }
    return out;
}

void load(State& s) {
    if (s.loaded) return;
    s.loaded = true;
#ifdef _WIN32
    char* e = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&e, &n, "KEEPTERM_FAULTS") == 0 && e) {
        s.rules = parse(e);
        std::free(e);
    }
#else
    if (const char* e = std::getenv("KEEPTERM_FAULTS")) s.rules = parse(e);
#endif
}

}  // namespace

bool hit(std::string_view name) {
    State& s = state();
    Action act;
    uint64_t delay = 0;
    {
        std::lock_guard lk(s.mu);
        load(s);
        auto it = s.counts.find(name);
        if (it == s.counts.end()) it = s.counts.emplace(std::string(name), 0).first;
        const uint64_t n = ++it->second;
        if (s.rules.empty()) return false;
        const Rule* rule = nullptr;
        for (const Rule& r : s.rules) {
            if (r.name != name) continue;
            const bool on = r.p >= 0 ? std::uniform_real_distribution<double>(0, 1)(s.rng) < r.p : n >= r.from && n <= r.to;
            if (on) {
                rule = &r;
                break;
            }
        }
        if (!rule) return false;
        act = rule->action;
        delay = rule->delay_ms;
    }
    switch (act) {
        case Action::abort:
            std::fprintf(stderr, "fault: abort at %.*s\n", static_cast<int>(name.size()), name.data());
            std::fflush(stderr);
            std::_Exit(86);
        case Action::hang:
            for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
        case Action::delay:
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            return false;
        case Action::fail: return true;
    }
    return false;
}

uint64_t hits(std::string_view name) {
    State& s = state();
    std::lock_guard lk(s.mu);
    auto it = s.counts.find(name);
    return it == s.counts.end() ? 0 : it->second;
}

void configure(std::string_view spec) {
    State& s = state();
    std::lock_guard lk(s.mu);
    s.loaded = true;
    s.rules = parse(spec);
    s.counts.clear();
}

}  // namespace kt::fault

#endif
