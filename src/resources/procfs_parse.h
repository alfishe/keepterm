// Parsers for Linux procfs and cgroupfs files. Core code: plain text in, values
// out, so the tests run on every OS.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kt::resources::procfs {

// /proc/<pid>/stat. comm may hold spaces and parentheses: it ends at the last ')'.
struct Stat {
    uint32_t pid = 0;
    std::string comm;
    char state = '?';
    uint32_t ppid = 0;
    int32_t pgrp = 0, session = 0;
    int32_t tty_nr = 0;
    uint64_t utime = 0, stime = 0;  // clock ticks
    uint32_t num_threads = 0;
    uint64_t starttime = 0;         // clock ticks since boot
    uint64_t rss_pages = 0;
};
std::optional<Stat> parse_stat(std::string_view text);

// /proc/<pid>/statm, in pages.
struct Statm {
    uint64_t size = 0, resident = 0, shared = 0;
};
std::optional<Statm> parse_statm(std::string_view text);

// /proc/<pid>/smaps_rollup, in bytes. Pss_Anon / Pss_File / Pss_Shmem exist on newer kernels.
struct SmapsRollup {
    std::optional<uint64_t> rss, pss, pss_anon, pss_file, pss_shmem, swap, swap_pss;
};
SmapsRollup parse_smaps_rollup(std::string_view text);

// A "Key:   123 kB" line of /proc/<pid>/status or smaps_rollup, in bytes.
std::optional<uint64_t> find_kb_field(std::string_view text, std::string_view key);

// cgroup v2 memory.stat (bytes). Unknown keys are ignored.
struct MemoryStat {
    std::optional<uint64_t> anon, file, kernel, shmem, file_mapped, slab, kernel_stack, sock;
    // kernel (5.18+), else slab + kernel_stack + sock where present.
    std::optional<uint64_t> kernel_total() const;
};
MemoryStat parse_memory_stat(std::string_view text);

// The first value of a "key value" line (cpu.stat usage_usec, memory.stat entries).
std::optional<uint64_t> find_key_value(std::string_view text, std::string_view key);

// A single unsigned number with optional trailing newline ("max" gives nullopt).
std::optional<uint64_t> parse_number(std::string_view text);

// cgroup.procs: one PID per line.
std::vector<uint32_t> parse_pid_list(std::string_view text);

// /proc/self/cgroup: the path of the "0::<path>" line (cgroup v2).
std::optional<std::string> parse_cgroup_v2_path(std::string_view text);

// /proc/self/mountinfo: the mount point of the cgroup2 file system.
std::optional<std::string> find_cgroup2_mount(std::string_view mountinfo);

// /proc/<pid>/cmdline: NUL-separated arguments.
std::vector<std::string> split_nul(std::string_view text);

}  // namespace kt::resources::procfs
