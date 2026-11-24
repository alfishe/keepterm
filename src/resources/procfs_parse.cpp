#include "resources/procfs_parse.h"

#include <charconv>

namespace kt::resources::procfs {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

template <class T>
bool to_num(std::string_view s, T& out) {
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

// Splits off the next space-separated token.
std::string_view next_token(std::string_view& s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    const size_t end = s.find(' ');
    std::string_view tok = s.substr(0, end);
    s.remove_prefix(end == std::string_view::npos ? s.size() : end);
    return tok;
}

// Calls fn(line) for each line.
template <class Fn>
void for_each_line(std::string_view text, Fn&& fn) {
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        fn(text.substr(0, nl));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
}

}  // namespace

std::optional<Stat> parse_stat(std::string_view text) {
    Stat st;
    const size_t open = text.find('(');
    const size_t close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return std::nullopt;
    if (!to_num(trim(text.substr(0, open)), st.pid)) return std::nullopt;
    st.comm = std::string(text.substr(open + 1, close - open - 1));
    std::string_view rest = text.substr(close + 1);
    // Fields from 3 (state) on; field numbers as in proc(5).
    std::string_view f[22];
    for (auto& tok : f) {
        tok = next_token(rest);
        if (tok.empty()) return std::nullopt;
    }
    auto field = [&](int n) { return f[static_cast<size_t>(n - 3)]; };
    if (field(3).size() != 1) return std::nullopt;
    st.state = field(3)[0];
    int64_t threads = 0, rss = 0;
    if (!to_num(field(4), st.ppid) || !to_num(field(5), st.pgrp) || !to_num(field(6), st.session) ||
        !to_num(field(7), st.tty_nr) || !to_num(field(14), st.utime) || !to_num(field(15), st.stime) ||
        !to_num(field(20), threads) || !to_num(field(22), st.starttime) || !to_num(field(24), rss))
        return std::nullopt;
    st.num_threads = threads > 0 ? static_cast<uint32_t>(threads) : 0;
    st.rss_pages = rss > 0 ? static_cast<uint64_t>(rss) : 0;
    return st;
}

std::optional<Statm> parse_statm(std::string_view text) {
    Statm m;
    std::string_view rest = trim(text);
    if (!to_num(next_token(rest), m.size) || !to_num(next_token(rest), m.resident) ||
        !to_num(next_token(rest), m.shared))
        return std::nullopt;
    return m;
}

std::optional<uint64_t> find_kb_field(std::string_view text, std::string_view key) {
    std::optional<uint64_t> out;
    for_each_line(text, [&](std::string_view line) {
        if (out || !line.starts_with(key) || line.size() <= key.size() || line[key.size()] != ':') return;
        std::string_view v = trim(line.substr(key.size() + 1));
        if (v.ends_with(" kB")) v.remove_suffix(3);
        uint64_t kb = 0;
        if (to_num(trim(v), kb)) out = kb * 1024;
    });
    return out;
}

SmapsRollup parse_smaps_rollup(std::string_view text) {
    SmapsRollup r;
    r.rss = find_kb_field(text, "Rss");
    r.pss = find_kb_field(text, "Pss");
    r.pss_anon = find_kb_field(text, "Pss_Anon");
    r.pss_file = find_kb_field(text, "Pss_File");
    r.pss_shmem = find_kb_field(text, "Pss_Shmem");
    r.swap = find_kb_field(text, "Swap");
    r.swap_pss = find_kb_field(text, "SwapPss");
    return r;
}

std::optional<uint64_t> find_key_value(std::string_view text, std::string_view key) {
    std::optional<uint64_t> out;
    for_each_line(text, [&](std::string_view line) {
        if (out || !line.starts_with(key) || line.size() <= key.size() || line[key.size()] != ' ') return;
        uint64_t v = 0;
        if (to_num(trim(line.substr(key.size() + 1)), v)) out = v;
    });
    return out;
}

std::optional<uint64_t> MemoryStat::kernel_total() const {
    if (kernel) return kernel;
    if (!slab && !kernel_stack && !sock) return std::nullopt;
    return slab.value_or(0) + kernel_stack.value_or(0) + sock.value_or(0);
}

MemoryStat parse_memory_stat(std::string_view text) {
    MemoryStat m;
    m.anon = find_key_value(text, "anon");
    m.file = find_key_value(text, "file");
    m.kernel = find_key_value(text, "kernel");
    m.shmem = find_key_value(text, "shmem");
    m.file_mapped = find_key_value(text, "file_mapped");
    m.slab = find_key_value(text, "slab");
    m.kernel_stack = find_key_value(text, "kernel_stack");
    m.sock = find_key_value(text, "sock");
    return m;
}

std::optional<uint64_t> parse_number(std::string_view text) {
    uint64_t v = 0;
    if (!to_num(trim(text), v)) return std::nullopt;
    return v;
}

std::vector<uint32_t> parse_pid_list(std::string_view text) {
    std::vector<uint32_t> out;
    for_each_line(text, [&](std::string_view line) {
        uint32_t pid = 0;
        if (to_num(trim(line), pid) && pid != 0) out.push_back(pid);
    });
    return out;
}

std::optional<std::string> parse_cgroup_v2_path(std::string_view text) {
    std::optional<std::string> out;
    for_each_line(text, [&](std::string_view line) {
        if (!out && line.starts_with("0::")) out = std::string(trim(line.substr(3)));
    });
    return out;
}

std::optional<std::string> find_cgroup2_mount(std::string_view mountinfo) {
    // "36 25 0:31 / /sys/fs/cgroup rw,nosuid shared:9 - cgroup2 cgroup2 rw"
    std::optional<std::string> out;
    for_each_line(mountinfo, [&](std::string_view line) {
        if (out) return;
        const size_t dash = line.find(" - ");
        if (dash == std::string_view::npos) return;
        std::string_view tail = line.substr(dash + 3);
        if (next_token(tail) != "cgroup2") return;
        std::string_view head = line.substr(0, dash);
        next_token(head);  // mount id
        next_token(head);  // parent id
        next_token(head);  // major:minor
        next_token(head);  // root
        const std::string_view mp = next_token(head);
        if (!mp.empty()) out = std::string(mp);
    });
    return out;
}

std::vector<std::string> split_nul(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const size_t z = text.find('\0');
        out.emplace_back(text.substr(0, z));
        if (z == std::string_view::npos) break;
        text.remove_prefix(z + 1);
    }
    return out;
}

}  // namespace kt::resources::procfs
