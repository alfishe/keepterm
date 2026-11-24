#include "session/proc_chain.h"

#include <algorithm>

namespace kt::session {

std::string image_name(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    std::string s(slash == std::string_view::npos ? path : path.substr(slash + 1));
    if (!s.empty() && s[0] == '-') s.erase(0, 1);
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
    return s;
}

std::vector<proto::ChainEntry> build_chain(uint32_t root, uint32_t fg, const ProcReader& read) {
    std::vector<proto::ChainEntry> up;   // foreground first
    if (fg == 0 || root == 0 || fg == root || !read) return {};
    uint32_t cur = fg;
    std::optional<ProcFacts> facts = read(cur);
    for (int depth = 0; depth < 64; ++depth) {
        if (!facts) return {};
        proto::ChainEntry e;
        e.pid = cur;
        e.ppid = facts->ppid;
        e.start = facts->start;
        e.start_unix_ns = facts->start_unix_ns;
        e.image = facts->image;
        e.comm = facts->comm;
        up.push_back(std::move(e));
        if (facts->ppid == root) {
            std::reverse(up.begin(), up.end());
            if (up.size() > proto::kMaxChain) up.resize(proto::kMaxChain);
            return up;
        }
        if (facts->ppid == 0 || facts->ppid == cur) return {};
        auto parent = read(facts->ppid);
        // A reused parent pid: the "parent" started after its child.
        if (!parent || (parent->start_unix_ns && facts->start_unix_ns && parent->start_unix_ns > facts->start_unix_ns)) {
            return {};
        }
        cur = facts->ppid;
        facts = std::move(parent);
    }
    return {};
}

std::vector<proto::ChainEntry> foreground_chain(uint32_t root, uint32_t fg) {
    auto chain = build_chain(root, fg, make_proc_reader());
    for (auto& e : chain) {
        e.argv = process_argv(e.pid);
        e.cwd = process_cwd(e.pid);
    }
    return chain;
}

}  // namespace kt::session
