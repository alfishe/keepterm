// Working directory of a session: sources, parsing and precedence.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/error.h"

namespace kt::session {

// Order of trust: Integration > OsQuery / ProcessPeb > PromptParse > LastKnown.
enum class CwdSource : uint8_t { None, Integration, OsQuery, ProcessPeb, PromptParse, LastKnown };

// "integration", "os_query", "process_peb", "prompt_parse", "last_known", "none".
std::string_view to_string(CwdSource);
std::optional<CwdSource> parse_cwd_source(std::string_view);

// OSC 7 value "file://host/path", percent-decoded. Returns the path; *host gets the host part.
// Windows: "/C:/x" -> "C:\x" and '/' -> '\'. nullopt when malformed (no "file://", bad %XX, NUL).
std::optional<std::string> cwd_from_osc7(std::string_view uri, std::string* host = nullptr);
// OSC 9;9 value: the path as sent, surrounding double quotes removed. nullopt when empty.
std::optional<std::string> cwd_from_osc9_9(std::string_view value);
// Empty, "localhost", or this machine's host name (case-insensitive, first label also matches).
bool is_local_host(std::string_view host);

// Default cmd ("C:\path>") and PowerShell ("PS C:\path> ") prompts on the cursor row
//. No file system check here.
std::optional<std::string> parse_prompt_cwd(std::string_view cursor_row_text);

// Linux readlink /proc/<pid>/cwd; macOS proc_pidinfo(PROC_PIDVNODEPATHINFO).
// Windows: the process's PEB (NtQueryInformationProcess, resolved at run time, then
// ReadProcessMemory of RTL_USER_PROCESS_PARAMETERS.CurrentDirectory), WOW64 aware; a trailing
// separator is removed except at a drive root.
Result<std::string> query_process_cwd(uint32_t pid);

#ifdef _WIN32
// The command line of a process from its PEB (the same way as the directory). UTF-8.
Result<std::string> query_process_command_line(uint32_t pid);
#endif

bool directory_exists(const std::string& utf8_path);
// The nearest existing folder at or above `path`, else the home folder.
std::string nearest_existing_dir(const std::string& path);
std::string home_dir();

// The session's cwd and its source. Integration is active once reported; while
// active only integration reports change the value. Lower sources never override a higher one
// that is active; OsQuery / ProcessPeb / PromptParse replace each other and LastKnown.
// Foreign-host OSC 7 values are not passed in here (the caller checks is_local_host).
class CwdTracker {
public:
    void set_initial(std::string dir);              // LastKnown
    bool report(CwdSource source, std::string path);  // true if value or source changed
    const std::string& cwd() const { return cwd_; }
    CwdSource source() const { return source_; }
    bool integration_active() const { return integration_; }

private:
    std::string cwd_;
    CwdSource source_ = CwdSource::None;
    bool integration_ = false;
};

}  // namespace kt::session
