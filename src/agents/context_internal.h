// Shared pieces of the context reducers (agents/context.h). Not a public header.
#pragma once

#include <cstddef>
#include <deque>
#include <set>
#include <string>
#include <string_view>

#include "agents/context.h"
#include "agents/json_scan.h"

namespace kt::agents::detail {

// Splits the fed bytes into lines; a line is applied once its newline has arrived. A line longer
// than kMaxLine (a huge tool result) is skipped unread.
class LineReducer : public IContextReducer {
public:
    static constexpr std::size_t kMaxLine = 4u * 1024 * 1024;

    void feed(std::string_view bytes) final;
    void reset_from_tail(std::string_view tail) final;
    void reset() final;
    const ContextUsage& usage() const final { return u_; }
    bool saw_usage() const final { return saw_usage_; }

protected:
    virtual void apply_line(std::string_view line) = 0;
    virtual void clear_state() = 0;   // subclass state; u_ is reset by the base
    // Records the agent version and flags a newer major as UnknownVersion.
    void note_version(std::string_view v, int known_major);

    ContextUsage u_;
    bool saw_usage_ = false;

private:
    std::string pending_;
    bool skip_first_ = false;
    bool overlong_ = false;
};

// A bounded set of recently seen ids (message ids are monotonic per turn; old ones never
// return), so a long session does not grow memory.
class RecentIds {
public:
    explicit RecentIds(std::size_t cap = 4096) : cap_(cap) {}
    // True when `id` was not seen before (and remembers it).
    bool insert(std::string_view id);
    void clear() {
        set_.clear();
        order_.clear();
    }

private:
    std::size_t cap_;
    std::set<std::string, std::less<>> set_;
    std::deque<std::string> order_;
};

// A file name without its folder ('/' or '\\').
std::string_view base_name(std::string_view path);
std::string parent_dir(std::string_view path);

}  // namespace kt::agents::detail
