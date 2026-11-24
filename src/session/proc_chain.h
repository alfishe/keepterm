// The foreground chain of a session: the processes from the
// shell's child down to the foreground process, with the facts agent recognition needs.
//
// Linux /proc/<pid>/{stat,comm,exe,cmdline,cwd}, macOS proc_pidinfo, proc_pidpath and
// sysctl(KERN_PROCARGS2), Windows Toolhelp, GetProcessTimes and the process's PEB (command line
// and directory, session/cwd.h).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "session/protocol.h"

namespace kt::session {

// What one lookup of a process gives (argv and cwd are read only for chain entries).
struct ProcFacts {
    uint32_t ppid = 0;
    uint64_t start = 0;           // OS-native, as os::process_start_time
    uint64_t start_unix_ns = 0;
    std::string image;            // lowercase basename without ".exe"
    std::string comm;
};
using ProcReader = std::function<std::optional<ProcFacts>(uint32_t pid)>;

// A reader over the live process table. On Windows it takes one Toolhelp snapshot when first
// called; on POSIX every call reads the process.
ProcReader make_proc_reader();
// argv and cwd of a process (empty when unreadable). Windows: from the PEB, the command line
// split as CommandLineToArgvW does; cached per process (the directory for 2 s).
std::vector<std::string> process_argv(uint32_t pid);
std::string process_cwd(uint32_t pid);

// Walks from `fg` up the validated parent links (a parent must not start after its child) to
// `root`. Returns the chain root's child first, at most proto::kMaxChain entries from the shell
// side; empty when fg is the root, 0, or not a descendant of root. argv and cwd stay empty.
std::vector<proto::ChainEntry> build_chain(uint32_t root, uint32_t fg, const ProcReader& read);

// build_chain over the live table, with argv and cwd filled in.
std::vector<proto::ChainEntry> foreground_chain(uint32_t root, uint32_t fg);

// Lowercase basename without a Windows ".exe" and without a login shell's '-'.
std::string image_name(std::string_view path);

}  // namespace kt::session
