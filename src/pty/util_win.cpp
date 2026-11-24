#include "pty/util_win.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include <sddl.h>
#include <userenv.h>

#include "platform/random.h"

#ifndef KT_VERSION
#define KT_VERSION "0"
#endif

namespace kt::pty::win {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n =
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

Unexpected<Error> win_error(std::string what, DWORD code) {
    Errc e = Errc::os_error;
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_BAD_EXE_FORMAT:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME: e = Errc::unavailable; break;
        case ERROR_SEM_TIMEOUT:
        case WAIT_TIMEOUT: e = Errc::timeout; break;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
        case ERROR_PIPE_NOT_CONNECTED: e = Errc::lost; break;
        default: break;
    }
    what += " (error ";
    what += std::to_string(code);
    what += ")";
    return make_error(e, std::move(what), static_cast<int64_t>(code));
}

std::wstring build_command_line(const std::vector<std::string>& argv) {
    std::wstring out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) out += L' ';
        const std::wstring a = widen(argv[i]);
        if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            out += a;
            continue;
        }
        out += L'"';
        std::size_t backslashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            if (c == L'"') {
                out.append(backslashes * 2 + 1, L'\\');
            } else {
                out.append(backslashes, L'\\');
            }
            backslashes = 0;
            out += c;
        }
        out.append(backslashes * 2, L'\\');
        out += L'"';
    }
    return out;
}

namespace {

bool name_equal(std::wstring_view a, std::wstring_view b) {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                  TRUE) == CSTR_EQUAL;
}

std::wstring_view var_name(std::wstring_view entry) {
    const auto eq = entry.find(L'=', 1);  // "=C:=C:\dir" entries start with '='
    return entry.substr(0, eq == std::wstring_view::npos ? entry.size() : eq);
}

void parse_block(const wchar_t* block, std::vector<std::wstring>& out) {
    for (const wchar_t* p = block; p && *p; p += wcslen(p) + 1) out.emplace_back(p);
}

void set_var(std::vector<std::wstring>& vars, std::wstring_view name, const std::wstring* value) {
    std::erase_if(vars, [&](const std::wstring& v) { return name_equal(var_name(v), name); });
    if (value) vars.push_back(std::wstring(name) + L"=" + *value);
}

}  // namespace

std::wstring build_environment(const std::vector<EnvEdit>& edits, bool vt_backend) {
    std::vector<std::wstring> vars;
    HANDLE token = nullptr;
    void* block = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token) &&
        ::CreateEnvironmentBlock(&block, token, FALSE)) {
        parse_block(static_cast<const wchar_t*>(block), vars);
        ::DestroyEnvironmentBlock(block);
    } else {
        wchar_t* env = ::GetEnvironmentStringsW();
        parse_block(env, vars);
        if (env) ::FreeEnvironmentStringsW(env);
    }
    if (token) ::CloseHandle(token);

    std::erase_if(vars, [](const std::wstring& v) {
        const auto n = var_name(v);
        return name_equal(n, L"LINES") || name_equal(n, L"COLUMNS") ||
               (n.size() >= 9 && name_equal(n.substr(0, 9), L"KEEPTERM_"));
    });
    const std::wstring term = L"xterm-256color", truecolor = L"truecolor";
    set_var(vars, L"TERM", vt_backend ? &term : nullptr);
    set_var(vars, L"COLORTERM", vt_backend ? &truecolor : nullptr);
    const std::wstring prog = L"keepterm", ver = widen(KT_VERSION);
    set_var(vars, L"TERM_PROGRAM", &prog);
    set_var(vars, L"TERM_PROGRAM_VERSION", &ver);
    for (const auto& e : edits) {
        const std::wstring name = widen(e.name);
        if (name.empty() || name.find(L'=') != std::wstring::npos) continue;
        if (e.value) {
            const std::wstring v = widen(*e.value);
            set_var(vars, name, &v);
        } else {
            set_var(vars, name, nullptr);
        }
    }
    std::stable_sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        const auto na = var_name(a), nb = var_name(b);
        return ::CompareStringOrdinal(na.data(), static_cast<int>(na.size()), nb.data(), static_cast<int>(nb.size()),
                                      TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const auto& v : vars) {
        out += v;
        out += L'\0';
    }
    out += L'\0';
    if (vars.empty()) out += L'\0';
    return out;
}

namespace {

std::wstring user_sid() {
    std::wstring sid;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return sid;
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<std::byte> buf(len ? len : 1);
    if (::GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
        LPWSTR str = nullptr;
        if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) {
            sid = str;
            ::LocalFree(str);
        }
    }
    ::CloseHandle(token);
    return sid;
}

}  // namespace

const std::string& sid_hash() {
    static const std::string h = [] {
        uint64_t x = 1469598103934665603ull;
        for (wchar_t c : user_sid()) {
            x ^= static_cast<uint64_t>(c);
            x *= 1099511628211ull;
        }
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(x));
        return std::string(buf);
    }();
    return h;
}

std::string random_hex(std::size_t bytes) {
    std::vector<std::byte> b(bytes);
    if (!kt::platform::random_bytes(b)) {
        // Fall back to time and counters; names stay unique, only less unpredictable.
        LARGE_INTEGER t;
        ::QueryPerformanceCounter(&t);
        uint64_t x = static_cast<uint64_t>(t.QuadPart) ^ (uint64_t{::GetCurrentProcessId()} << 32);
        for (auto& v : b) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            v = static_cast<std::byte>(x >> 56);
        }
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    for (auto v : b) {
        s.push_back(digits[static_cast<unsigned>(v) >> 4]);
        s.push_back(digits[static_cast<unsigned>(v) & 15]);
    }
    return s;
}

UserOnlySecurity::UserOnlySecurity(bool with_system) {
    const std::wstring sid = user_sid();
    if (sid.empty()) return;
    std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    if (with_system) sddl += L"(A;;GA;;;SY)";
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
        sd_ = nullptr;
        return;
    }
    sa_.nLength = sizeof sa_;
    sa_.lpSecurityDescriptor = sd_;
    sa_.bInheritHandle = FALSE;
}

UserOnlySecurity::~UserOnlySecurity() {
    if (sd_) ::LocalFree(sd_);
}

std::string name_part(const std::string& session_id) {
    std::string s;
    for (char c : session_id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            s.push_back(c);
        }
        if (s.size() >= 64) break;
    }
    return s.empty() ? random_hex(8) : s;
}

Result<Job> create_job(const std::string& session_id) {
    UserOnlySecurity sec(true);
    const std::string session = name_part(session_id);
    for (int attempt = 0; attempt < 3; ++attempt) {
        Job job;
        job.name = "Local\\keepterm-" + sid_hash() + "-" + session + "-" + random_hex(8);
        job.handle.reset(::CreateJobObjectW(sec.get(), widen(job.name).c_str()));
        if (!job.handle) return win_error("CreateJobObjectW");
        if (::GetLastError() == ERROR_ALREADY_EXISTS) continue;  // pre-created name: new nonce
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        ::SetInformationJobObject(job.handle.get(), JobObjectExtendedLimitInformation, &li, sizeof li);
        return job;
    }
    return make_error(Errc::os_error, "job name collision");
}

Result<std::vector<Pid>> job_process_ids(HANDLE job) {
    DWORD cap = 256;
    for (int i = 0; i < 8; ++i) {
        std::vector<std::byte> buf(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + cap * sizeof(ULONG_PTR));
        auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
        if (::QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(buf.size()),
                                        nullptr) ||
            ::GetLastError() == ERROR_MORE_DATA) {
            if (list->NumberOfProcessIdsInList < list->NumberOfAssignedProcesses) {
                cap = list->NumberOfAssignedProcesses + 64;
                continue;
            }
            std::vector<Pid> out;
            for (DWORD k = 0; k < list->NumberOfProcessIdsInList; ++k) {
                out.push_back(static_cast<Pid>(list->ProcessIdList[k]));
            }
            return out;
        }
        return win_error("QueryInformationJobObject");
    }
    return make_error(Errc::os_error, "job process list keeps growing");
}

Result<Handle> create_pipe_server(const std::wstring& name, DWORD access) {
    UserOnlySecurity sec(false);
    HANDLE h = ::CreateNamedPipeW(name.c_str(), access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                  64 * 1024, 64 * 1024, 0, sec.get());
    if (h == INVALID_HANDLE_VALUE) return win_error("CreateNamedPipeW");
    return Handle(h);
}

uint64_t creation_time(HANDLE process) {
    FILETIME c, e, k, u;
    if (!::GetProcessTimes(process, &c, &e, &k, &u)) return 0;
    return (uint64_t{c.dwHighDateTime} << 32) | c.dwLowDateTime;
}

uint64_t creation_time_of(Pid pid) {
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    return h ? creation_time(h.get()) : 0;
}

Pid pick_foreground(const std::vector<Pid>& pids, const std::vector<Pid>& exclude, Pid root) {
    Pid best = 0;
    uint64_t best_t = 0;
    bool root_seen = false;
    for (Pid p : pids) {
        if (std::find(exclude.begin(), exclude.end(), p) != exclude.end()) continue;
        if (p == root) {
            root_seen = true;
            continue;
        }
        const uint64_t t = creation_time_of(p);
        if (t == 0) continue;
        if (t > best_t || (t == best_t && p > best)) {
            best = p;
            best_t = t;
        }
    }
    if (best == 0 && root_seen) return root;
    return best;
}

void clear_ctrl_c_ignore() {
    static std::once_flag once;
    std::call_once(once, [] { ::SetConsoleCtrlHandler(nullptr, FALSE); });
}

OverlappedWriter::OverlappedWriter() : ev_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) { ov_.hEvent = ev_.get(); }

OverlappedWriter::~OverlappedWriter() { cancel(); }

bool OverlappedWriter::pending() const {
    std::lock_guard l(m_);
    return pending_;
}

bool OverlappedWriter::broken() const {
    std::lock_guard l(m_);
    return broken_;
}

std::size_t OverlappedWriter::queued() const {
    std::lock_guard l(m_);
    return queue_.size() + inflight_.size();
}

std::size_t OverlappedWriter::enqueue(std::string_view bytes, bool force) {
    std::lock_guard l(m_);
    if (broken_) return 0;
    const std::size_t used = queue_.size() + inflight_.size();
    const std::size_t room = used >= kMaxInFlight ? 0 : kMaxInFlight - used;
    const std::size_t n = force ? bytes.size() : std::min(room, bytes.size());
    queue_.append(bytes.substr(0, n));
    pump_locked();
    return n;
}

bool OverlappedWriter::pump() {
    std::lock_guard l(m_);
    return pump_locked();
}

bool OverlappedWriter::pump_locked() {
    bool completed = false;
    if (pending_) {
        DWORD n = 0;
        if (!::GetOverlappedResult(h_, &ov_, &n, FALSE)) {
            if (::GetLastError() == ERROR_IO_INCOMPLETE) return false;
            broken_ = true;
        }
        pending_ = false;
        inflight_.clear();
        completed = true;
    }
    if (!pending_ && !broken_ && !queue_.empty() && h_) {
        const std::size_t n = std::min(queue_.size(), kMaxInFlight);
        inflight_.assign(queue_, 0, n);
        queue_.erase(0, n);
        if (::WriteFile(h_, inflight_.data(), static_cast<DWORD>(n), nullptr, &ov_) ||
            ::GetLastError() == ERROR_IO_PENDING) {
            pending_ = true;
        } else {
            broken_ = true;
            inflight_.clear();
        }
    }
    return completed;
}

void OverlappedWriter::flush(DWORD ms) {
    const ULONGLONG deadline = ::GetTickCount64() + ms;
    for (;;) {
        {
            std::lock_guard l(m_);
            pump_locked();
            if (broken_ || (!pending_ && queue_.empty())) return;
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) return;
        ::WaitForSingleObject(ev_.get(), static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 50)));
    }
}

void OverlappedWriter::cancel() {
    std::lock_guard l(m_);
    if (pending_ && h_) {
        ::CancelIoEx(h_, &ov_);
        DWORD n = 0;
        ::GetOverlappedResult(h_, &ov_, &n, TRUE);
    }
    pending_ = false;
    inflight_.clear();
    queue_.clear();
}

}  // namespace kt::pty::win
