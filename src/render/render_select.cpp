#include "render/render_select.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "base/log.h"
#include "platform/paths.h"
#include "render/probe_scene.h"

#ifdef _WIN32
#include <windows.h>

#include "render/d3d11/d3d11_backend.h"
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace kt::render {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- minimal JSON

namespace {

// Just enough JSON for the probe protocol and the cache: objects, arrays, strings, numbers,
// booleans and null. Unknown fields are ignored by the readers.
struct Json {
    enum class T { null, boolean, number, string, array, object } t = T::null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    // Declared here, defaulted in the .cpp: inline special members would instantiate the
    // pair<string, Json> of the member list while Json is incomplete (clang with libstdc++ 15).
    Json();
    Json(const Json&);
    Json(Json&&) noexcept;
    Json& operator=(const Json&);
    Json& operator=(Json&&) noexcept;
    ~Json();

    const Json* get(std::string_view key) const {
        for (const auto& [k, v] : o) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    std::string str(std::string_view key) const {
        const Json* v = get(key);
        return v && v->t == T::string ? v->s : std::string();
    }
    double num(std::string_view key, double def = 0) const {
        const Json* v = get(key);
        return v && v->t == T::number ? v->n : def;
    }
    bool flag(std::string_view key) const {
        const Json* v = get(key);
        return v && v->t == T::boolean && v->b;
    }
};

Json::Json() = default;
Json::Json(const Json&) = default;
Json::Json(Json&&) noexcept = default;
Json& Json::operator=(const Json&) = default;
Json& Json::operator=(Json&&) noexcept = default;
Json::~Json() = default;

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : p_(text.data()), end_(text.data() + text.size()) {}

    std::optional<Json> parse() {
        Json v;
        if (!value(v, 0)) return std::nullopt;
        ws();
        return v;
    }

private:
    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
    }
    bool lit(const char* w) {
        const size_t n = std::strlen(w);
        if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, w, n) != 0) return false;
        p_ += n;
        return true;
    }
    bool string(std::string& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            char c = *p_++;
            if (c == '\\') {
                if (p_ >= end_) return false;
                c = *p_++;
                switch (c) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        if (end_ - p_ < 4) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = *p_++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return false;
                        }
                        // BMP only; enough for adapter names and error texts.
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        continue;
                    }
                    default: break;   // \" \\ \/
                }
            }
            out += c;
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }
    bool value(Json& v, int depth) {
        if (depth > 16) return false;
        ws();
        if (p_ >= end_) return false;
        if (*p_ == '{') {
            ++p_;
            v.t = Json::T::object;
            ws();
            if (p_ < end_ && *p_ == '}') {
                ++p_;
                return true;
            }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p_ >= end_ || *p_ != ':') return false;
                ++p_;
                Json item;
                if (!value(item, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(item));
                ws();
                if (p_ < end_ && *p_ == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < end_ && *p_ == '}') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (*p_ == '[') {
            ++p_;
            v.t = Json::T::array;
            ws();
            if (p_ < end_ && *p_ == ']') {
                ++p_;
                return true;
            }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                v.a.push_back(std::move(item));
                ws();
                if (p_ < end_ && *p_ == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < end_ && *p_ == ']') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (*p_ == '"') {
            v.t = Json::T::string;
            return string(v.s);
        }
        if (lit("true")) {
            v.t = Json::T::boolean;
            v.b = true;
            return true;
        }
        if (lit("false")) {
            v.t = Json::T::boolean;
            return true;
        }
        if (lit("null")) return true;
        const char* start = p_;
        while (p_ < end_ && (std::strchr("+-.eE", *p_) || (*p_ >= '0' && *p_ <= '9'))) ++p_;
        if (p_ == start) return false;
        v.t = Json::T::number;
        v.n = std::strtod(std::string(start, p_).c_str(), nullptr);
        return true;
    }

    const char* p_;
    const char* end_;
};

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", u);
            out += buf;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

ProbeResult from_json(const Json& j) {
    ProbeResult r;
    if (auto k = parse_backend(j.str("backend"))) r.backend = *k;
    r.ok = j.flag("ok");
    r.unavailable = j.flag("unavailable");
    r.reason = j.str("reason");
    r.adapter = j.str("adapter");
    r.vendor = static_cast<uint32_t>(j.num("vendor"));
    r.device = static_cast<uint32_t>(j.num("device"));
    r.ms = j.num("ms");
    r.max_diff = static_cast<int>(j.num("max_diff", -1));
    r.window_tested = j.flag("window");
    if (const Json* caps = j.get("caps")) r.dual_source_blend = caps->flag("dual_source_blend");
    return r;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file_atomic(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f.flush()) return false;
    }
    fs::rename(tmp, p, ec);   // replaces atomically on POSIX and with MoveFileEx semantics on Windows
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

std::string to_json(const ProbeResult& r) {
    std::ostringstream o;
    o << "{\"backend\":" << quote(backend_name(r.backend)) << ",\"ok\":" << (r.ok ? "true" : "false");
    if (r.unavailable) o << ",\"unavailable\":true";
    if (!r.reason.empty()) o << ",\"reason\":" << quote(r.reason);
    if (!r.adapter.empty()) o << ",\"adapter\":" << quote(r.adapter);
    if (r.vendor || r.device) o << ",\"vendor\":" << r.vendor << ",\"device\":" << r.device;
    char ms[32];
    std::snprintf(ms, sizeof(ms), "%.1f", r.ms);
    o << ",\"ms\":" << ms;
    if (r.max_diff >= 0) o << ",\"max_diff\":" << r.max_diff;
    if (r.window_tested) o << ",\"window\":true";
    o << ",\"caps\":{\"dual_source_blend\":" << (r.dual_source_blend ? "true" : "false") << "}}";
    return o.str();
}

std::optional<ProbeResult> parse_probe_result(std::string_view json) {
    auto j = JsonParser(json).parse();
    if (!j || j->t != Json::T::object || !parse_backend(j->str("backend"))) return std::nullopt;
    return from_json(*j);
}

std::optional<ProbeCache> load_probe_cache(const fs::path& file) {
    const std::string text = read_file(file);
    if (text.empty()) return std::nullopt;
    auto j = JsonParser(text).parse();
    if (!j || j->t != Json::T::object || j->num("schema") != 1) return std::nullopt;
    ProbeCache c;
    c.key = j->str("key");
    if (const Json* res = j->get("results"); res && res->t == Json::T::array) {
        for (const Json& e : res->a) {
            if (e.t == Json::T::object && parse_backend(e.str("backend"))) c.results.push_back(from_json(e));
        }
    }
    return c;
}

bool save_probe_cache(const fs::path& file, const ProbeCache& c) {
    std::string text = "{\n  \"schema\": 1,\n  \"key\": " + quote(c.key) + ",\n  \"results\": [";
    for (size_t i = 0; i < c.results.size(); ++i) text += (i ? ",\n    " : "\n    ") + to_json(c.results[i]);
    text += "\n  ]\n}\n";
    return write_file_atomic(file, text);
}

// ---------------------------------------------------------------- machine key, state, remote

std::string machine_key() {
    std::string key = "probe" + std::to_string(kProbeVersion) + ";";
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) fn(&v);
    }
    key += "windows-" + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber) + ";";
    key += d3d11::adapter_key();
#elif defined(__APPLE__)
    char buf[256];
    size_t n = sizeof(buf);
    if (sysctlbyname("kern.osproductversion", buf, &n, nullptr, 0) == 0) key += std::string("macos-") + buf + ";";
    n = sizeof(buf);
    if (sysctlbyname("kern.osversion", buf, &n, nullptr, 0) == 0) key += std::string(buf) + ";";
    n = sizeof(buf);
    if (sysctlbyname("hw.model", buf, &n, nullptr, 0) == 0) key += std::string(buf) + ";";
#else
    utsname u{};
    if (uname(&u) == 0) key += std::string("linux-") + u.release + ";";
    // DRM devices (vendor:device) and the NVIDIA driver version; the Mesa version would need
    // libGL or the Vulkan ICD and is not read here (optional input).
    std::error_code ec;
    std::vector<std::string> cards;
    for (const auto& e : fs::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
            return s;
        };
        cards.push_back("drm:" + trim(read_file(e.path() / "device" / "vendor")) + ":" + trim(read_file(e.path() / "device" / "device")));
    }
    std::sort(cards.begin(), cards.end());
    for (const auto& c : cards) key += c + ";";
    std::string nv = read_file("/proc/driver/nvidia/version");
    if (!nv.empty()) key += "nvidia:" + nv.substr(0, nv.find('\n')) + ";";
#endif
    return key;
}

fs::path default_state_dir() {
    if (auto dir = platform::paths::get(platform::paths::Kind::cache)) {
        if (platform::paths::make_private_dir(*dir)) return *dir;
    }
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) base = ".";
#ifdef _WIN32
    return base / "keepterm-render";
#else
    return base / ("keepterm-render-" + std::to_string(getuid()));
#endif
}

bool is_remote_session() {
#ifdef _WIN32
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
#elif defined(__APPLE__)
    return false;
#else
    if (const char* x = std::getenv("XRDP_SESSION"); x && *x) return true;
    // Forwarded X11: DISPLAY names a host ("host:10.0", "localhost:10.0") in an SSH session.
    const char* display = std::getenv("DISPLAY");
    const char* ssh = std::getenv("SSH_CONNECTION");
    return display && ssh && *ssh && display[0] != ':' && std::strncmp(display, "unix:", 5) != 0;
#endif
}

void write_crash_marker(const fs::path& dir, BackendKind kind) {
    (void)write_file_atomic(dir / "render-attempt", std::string(backend_name(kind)) + "\n");
}

void clear_crash_marker(const fs::path& dir) {
    std::error_code ec;
    fs::remove(dir / "render-attempt", ec);
}

std::optional<BackendKind> read_crash_marker(const fs::path& dir) {
    std::string text = read_file(dir / "render-attempt");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (text.empty()) return std::nullopt;
    return parse_backend(text);
}

// ---------------------------------------------------------------- image compare, probe

bool ImageCompare::within_tolerance() const {
    // <= 2 on >= 99.9 % of the pixels and <= 16 on the rest.
    return same_size && over_large == 0 && over_small * 1000 <= pixels;
}

ImageCompare compare_bgra(const Image& a, const Image& b) {
    ImageCompare c;
    c.same_size = a.w == b.w && a.h == b.h && a.pixels.size() == b.pixels.size();
    if (!c.same_size) return c;
    c.pixels = a.pixels.size();
    const bool swap = a.order != b.order;   // bgra vs rgba: R and B trade places
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        uint32_t pa = a.pixels[i], pb = b.pixels[i];
        if (swap) pb = (pb & 0xFF00FF00u) | ((pb >> 16) & 0xFFu) | ((pb & 0xFFu) << 16);
        int m = 0;
        for (int s = 0; s < 32; s += 8) {
            const int d = std::abs(static_cast<int>((pa >> s) & 0xFFu) - static_cast<int>((pb >> s) & 0xFFu));
            m = std::max(m, d);
        }
        c.max_diff = std::max(c.max_diff, m);
        if (m > 2) ++c.over_small;
        if (m > 16) ++c.over_large;
    }
    return c;
}

ProbeResult probe_offscreen(BackendKind kind, const BackendOptions& opts) {
    ProbeResult res;
    res.backend = kind;
    const auto t0 = std::chrono::steady_clock::now();
    auto done = [&] {
        res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return res;
    };
    auto r = create_backend(kind, opts);
    if (!r) {
        res.unavailable = r.error().code == Errc::unavailable || r.error().code == Errc::not_supported;
        res.reason = r.error().what;
        return done();
    }
    IRenderer& gpu = **r;
    res.adapter = gpu.caps().adapter_name;
    res.vendor = gpu.caps().vendor_id;
    res.device = gpu.caps().device_id;
    res.dual_source_blend = gpu.caps().dual_source_blend;
    const IRect all{0, 0, kProbeSceneSize.w, kProbeSceneSize.h};
    Result<Image> got = make_error(Errc::unavailable, "not rendered");
    if (auto ok = gpu.attach_offscreen(kProbeSceneSize, 1.0f); !ok) {
        res.reason = "offscreen target: " + ok.error().what;
        return done();
    }
    if (auto ok = draw_probe_scene(gpu); !ok) {
        res.reason = "test frame: " + ok.error().what;
        return done();
    }
    got = gpu.read_back(all);
    if (!got) {
        res.reason = "read-back: " + got.error().what;
        return done();
    }
    BackendOptions sw_opts = opts;
    sw_opts.threads = 1;
    auto sw = create_backend(BackendKind::software, sw_opts);
    if (!sw || !(*sw)->attach_offscreen(kProbeSceneSize, 1.0f) || !draw_probe_scene(**sw)) {
        res.reason = "software reference failed";
        return done();
    }
    auto expected = (*sw)->read_back(all);
    if (!expected) {
        res.reason = "software reference failed";
        return done();
    }
    const ImageCompare c = compare_bgra(*got, *expected);
    res.max_diff = c.max_diff;
    if (!c.within_tolerance()) {
        res.reason = "bad-output (max diff " + std::to_string(c.max_diff) + ", " + std::to_string(c.over_small) + " pixels over 2)";
        return done();
    }
    res.ok = true;
    return done();
}

// ---------------------------------------------------------------- child process

fs::path current_executable() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    fs::path p = fs::canonical(buf, ec);
    return ec ? fs::path(buf) : p;
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

ProbeResult run_probe_child(const fs::path& exe, BackendKind kind, bool allow_sw, std::chrono::milliseconds timeout) {
    ProbeResult res;
    res.backend = kind;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    std::string out;
    bool timed_out = false;
    int exit_code = -1;
#ifdef _WIN32
    std::wstring cmd = L"\"" + exe.wstring() + L"\" --probe-renderer " + fs::path(backend_name(kind)).wstring();
    if (allow_sw) cmd += L" --allow-software-adapter";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        res.reason = "probe-spawn: CreatePipe failed";
        return res;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        res.reason = "probe-spawn: CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return res;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()));
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 3);
        WaitForSingleObject(pi.hProcess, 1000);
        timed_out = true;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        exit_code = static_cast<int>(code);
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    int fds[2];
    if (pipe(fds) != 0) {
        res.reason = "probe-spawn: pipe failed";
        return res;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    const std::string exe_s = exe.string();
    std::vector<std::string> args{exe_s, "--probe-renderer", backend_name(kind)};
    if (allow_sw) args.emplace_back("--allow-software-adapter");
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, exe_s.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        res.reason = std::string("probe-spawn: ") + std::strerror(rc);
        return res;
    }
    const auto deadline = t0 + timeout;
    char buf[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int pr = poll(&p, 1, static_cast<int>(left));
        if (pr < 0 && errno == EINTR) continue;
        if (pr == 0) {
            timed_out = true;
            break;
        }
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;   // EOF: the child closed stdout (exited)
    }
    close(fds[0]);
    int status = 0;
    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    } else {
        // stdout closed; the exit follows. Give it the rest of the timeout, then kill.
        for (;;) {
            const pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) break;
            if (w < 0 && errno != EINTR) break;
            if (std::chrono::steady_clock::now() > deadline) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                timed_out = true;
                break;
            }
            usleep(2000);
        }
        if (!timed_out) exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif
    if (timed_out) {
        res.reason = "probe-timeout";
        res.ms = elapsed();
        return res;
    }
    const size_t brace = out.find('{');
    auto parsed = brace == std::string::npos ? std::nullopt : parse_probe_result(std::string_view(out).substr(brace));
    if (!parsed || (exit_code != 0 && exit_code != 1 && exit_code != 2)) {
        res.reason = "probe-crash (exit " + std::to_string(exit_code) + ")";
        res.ms = elapsed();
        return res;
    }
    res = *parsed;
    res.backend = kind;
    res.ok = res.ok && exit_code == 0;
    res.unavailable = exit_code == 2 || res.unavailable;
    res.ms = elapsed();
    return res;
}

// ---------------------------------------------------------------- selection

fs::path probe_cache_file(const fs::path& dir, const RenderConfig& cfg) {
    return dir / (cfg.backend.allow_software_adapter ? "render-probe-software-adapters.json" : "render-probe.json");
}

bool selection_is_cheap(const RenderConfig& cfg) {
    if (cfg.renderer != "auto" && parse_backend(cfg.renderer)) return true;   // forced: no probe
    const bool remote = cfg.remote.value_or(is_remote_session());
    if (remote && cfg.renderer_remote != "auto") return true;
    const fs::path dir = cfg.state_dir.empty() ? default_state_dir() : cfg.state_dir;
    const auto cache = load_probe_cache(probe_cache_file(dir, cfg));
    return cache && cache->key == machine_key();
}

Selection select_backends(const RenderConfig& cfg, const Prober& prober_in) {
    Selection sel;
    const auto order = platform_order();
    auto push = [&](BackendKind k) {
        if (std::find(sel.order.begin(), sel.order.end(), k) == sel.order.end()) sel.order.push_back(k);
    };
    auto finish = [&] {
        push(BackendKind::software);
        return sel;
    };

    // Forced backend: first; on failure the caller continues with auto.
    if (cfg.renderer != "auto") {
        if (auto k = parse_backend(cfg.renderer)) {
            push(*k);
            sel.why = "forced " + cfg.renderer + "; ";
            if (*k == BackendKind::software) return finish();
        } else {
            sel.why = "unknown renderer '" + cfg.renderer + "', using auto; ";
        }
    }
    const bool remote = cfg.remote.value_or(is_remote_session());
    if (remote && cfg.renderer_remote != "auto") {
        sel.why += "remote session: software (renderer_remote = software)";
        return finish();
    }

    const fs::path dir = cfg.state_dir.empty() ? default_state_dir() : cfg.state_dir;
    const fs::path file = probe_cache_file(dir, cfg);
    const std::string key = machine_key();
    std::optional<ProbeCache> cache = load_probe_cache(file);
    bool dirty = false;
    if (!cache || cache->key != key) {
        sel.why += cache ? "probe cache stale; " : "no probe cache; ";
        Prober prober = prober_in;
        if (!prober) {
            prober = [&](BackendKind k) {
                if (cfg.probe_in_process) return probe_offscreen(k, cfg.backend);
                const fs::path exe = cfg.exe.empty() ? current_executable() : cfg.exe;
                return run_probe_child(exe, k, cfg.backend.allow_software_adapter, cfg.probe_timeout);
            };
        }
        cache = ProbeCache{key, {}};
        for (BackendKind k : order) {
            if (k == BackendKind::software) continue;
            ProbeResult r = prober(k);
            log::info("render", "probe " + std::string(backend_name(k)) + ": " + (r.ok ? "ok on " + r.adapter : r.reason));
            cache->results.push_back(std::move(r));
            if (cache->results.back().ok) break;   // later backends are probed only when needed
        }
        dirty = true;
    }
    // Crash marker: the previous run died inside this backend.
    if (auto crashed = read_crash_marker(dir)) {
        for (auto& r : cache->results) {
            if (r.backend == *crashed && r.ok) {
                r.ok = false;
                r.reason = "crashed";
                dirty = true;
            }
        }
        clear_crash_marker(dir);
        sel.why += std::string("previous run crashed in ") + backend_name(*crashed) + "; ";
    }
    if (dirty && !save_probe_cache(file, *cache)) log::warn("render", "cannot write the probe cache in " + dir.string());
    for (const auto& r : cache->results) {
        if (r.ok) {
            push(r.backend);
            sel.why += std::string("probe: ") + backend_name(r.backend) + " ok";
            break;
        }
    }
    if (sel.why.find("probe:") == std::string::npos) sel.why += "no GPU backend passed the probe";
    // Backends after the winner stay as run-time fallbacks only if they were not probed bad.
    for (BackendKind k : order) {
        const auto it = std::find_if(cache->results.begin(), cache->results.end(), [&](const ProbeResult& r) { return r.backend == k; });
        if (it == cache->results.end() || it->ok) push(k);
    }
    return finish();
}

// ---------------------------------------------------------------- managed renderer

ManagedRenderer::ManagedRenderer(std::vector<BackendKind> order, Factory factory, fs::path state_dir)
    : order_(std::move(order)), factory_(std::move(factory)), state_dir_(std::move(state_dir)) {
    if (order_.empty() || order_.back() != BackendKind::software) order_.push_back(BackendKind::software);
    clock_ = [] { return std::chrono::steady_clock::now(); };
}

ManagedRenderer::~ManagedRenderer() {
    if (inner_) inner_->detach();
}

Result<void> ManagedRenderer::start(const platform::NativeSurface* surface, PxSize size, float scale) {
    if (surface) surface_ = *surface;
    size_ = size;
    scale_ = scale;
    attached_ = true;
    std::string reason = "start";
    for (size_t i = 0; i < order_.size(); ++i) {
        auto ok = create_at(i, reason);
        if (ok) return {};
        log::warn("render", std::string(backend_name(order_[i])) + " failed: " + ok.error().what);
        reason = std::string(backend_name(order_[i])) + " failed: " + ok.error().what;
        if (order_[i] == BackendKind::software) return ok;
    }
    return make_error(Errc::unavailable, "no renderer could start");
}

Result<void> ManagedRenderer::attach_inner() {
    if (!attached_) return {};
    if (surface_) return inner_->attach(*surface_, size_, scale_);
    return inner_->attach_offscreen(size_, scale_);
}

Result<void> ManagedRenderer::create_at(size_t index, const std::string& reason) {
    const BackendKind kind = order_[index];
    const BackendKind from = kind_;
    const bool had = inner_ != nullptr;
    if (inner_) {
        inner_->detach();
        inner_.reset();
    }
    // Crash marker around in-process creation of a GPU backend.
    const bool gpu = kind != BackendKind::software;
    if (gpu && !state_dir_.empty()) write_crash_marker(state_dir_, kind);
    auto created = factory_(kind);
    if (!created) {
        if (gpu && !state_dir_.empty()) clear_crash_marker(state_dir_);
        return Unexpected<Error>(created.error());
    }
    inner_ = std::move(*created);
    kind_ = kind;
    index_ = index;
    if (auto ok = attach_inner(); !ok) {
        inner_.reset();
        if (gpu && !state_dir_.empty()) clear_crash_marker(state_dir_);
        return ok;
    }
    marker_pending_ = gpu && !state_dir_.empty();
    // Re-create every texture from its CPU copy.
    identity_ = true;
    for (auto& [id, t] : textures_) {
        auto nt = inner_->create_texture(t.desc);
        if (!nt) {
            inner_.reset();
            return Unexpected<Error>(nt.error());
        }
        t.inner = *nt;
        if (!t.pixels.empty()) {
            const uint32_t bpp = t.desc.format == TexFormat::a8 ? 1u : 4u;
            inner_->update_texture(t.inner, IRect{0, 0, t.desc.w, t.desc.h}, t.pixels.data(), t.desc.w * bpp);
        }
        if (t.inner != id) identity_ = false;
    }
    force_full_ = true;
    if (had) {
        log::warn("render", std::string("switched renderer: ") + backend_name(from) + " -> " + backend_name(kind) + " (" + reason + ")");
        if (listener_) listener_(from, kind, reason);
    }
    return {};
}

Result<void> ManagedRenderer::switch_order(std::vector<BackendKind> order, const std::string& reason) {
    if (order.empty() || order.back() != BackendKind::software) order.push_back(BackendKind::software);
    if (inner_ && order.front() == kind_) {
        order_ = std::move(order);
        index_ = 0;
        return {};
    }
    order_ = std::move(order);
    in_frame_ = false;
    std::string why = reason;
    for (size_t i = 0; i < order_.size(); ++i) {
        if (auto ok = create_at(i, why); ok) return {};
        else why = std::string(backend_name(order_[i])) + " failed: " + ok.error().what;
        log::warn("render", why);
    }
    return make_error(Errc::unavailable, "no renderer could start");
}

bool ManagedRenderer::handle(const Error& e) {
    if (e.code != Errc::lost) return false;
    const auto now = clock_();
    const BackendKind lost = kind_;
    const bool twice = last_loss_ && last_loss_kind_ == lost && now - *last_loss_ < std::chrono::seconds(60);
    last_loss_ = now;
    last_loss_kind_ = lost;
    in_frame_ = false;
    if (marker_pending_) {
        clear_crash_marker(state_dir_);   // a loss is not a crash
        marker_pending_ = false;
    }
    // Recreate the same backend once; a second loss within 60 s moves on.
    size_t start = twice ? index_ + 1 : index_;
    const std::string reason = std::string(backend_name(lost)) + (twice ? " lost twice: " : " lost: ") + e.what;
    for (size_t i = start; i < order_.size(); ++i) {
        if (create_at(i, reason)) return true;
    }
    return false;
}

Result<void> ManagedRenderer::attach(const platform::NativeSurface& s, PxSize size, float scale) {
    surface_ = s;
    size_ = size;
    scale_ = scale;
    attached_ = true;
    force_full_ = true;
    return inner_->attach(s, size, scale);
}

Result<void> ManagedRenderer::attach_offscreen(PxSize size, float scale) {
    surface_.reset();
    size_ = size;
    scale_ = scale;
    attached_ = true;
    force_full_ = true;
    return inner_->attach_offscreen(size, scale);
}

void ManagedRenderer::resize(PxSize size, float scale) {
    size_ = size;
    scale_ = scale;
    inner_->resize(size, scale);
}

void ManagedRenderer::detach() {
    attached_ = false;
    surface_.reset();
    inner_->detach();
}

Result<TextureId> ManagedRenderer::create_texture(const TextureDesc& desc) {
    auto t = inner_->create_texture(desc);
    if (!t) return t;
    TextureId id = *t;
    // Keep the inner id as the public one while possible, so draw lists need no remapping.
    if (textures_.count(id)) {
        while (textures_.count(next_id_) || next_id_ == 0) ++next_id_;
        id = next_id_++;
        identity_ = false;
    }
    Tex tex;
    tex.desc = desc;
    tex.inner = *t;
    textures_.emplace(id, std::move(tex));
    return id;
}

void ManagedRenderer::update_texture(TextureId id, IRect region, const void* pixels, uint32_t stride) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    Tex& t = it->second;
    const uint32_t bpp = t.desc.format == TexFormat::a8 ? 1u : 4u;
    const IRect clipped = intersect(region, IRect{0, 0, t.desc.w, t.desc.h});
    if (!clipped.empty()) {
        if (t.pixels.empty()) t.pixels.assign(static_cast<size_t>(t.desc.w) * t.desc.h * bpp, 0);
        const auto* src = static_cast<const uint8_t*>(pixels);
        for (int32_t y = 0; y < clipped.h; ++y) {
            const uint8_t* s = src + static_cast<size_t>(clipped.y - region.y + y) * stride + static_cast<size_t>(clipped.x - region.x) * bpp;
            uint8_t* d = t.pixels.data() + (static_cast<size_t>(clipped.y + y) * t.desc.w + static_cast<size_t>(clipped.x)) * bpp;
            std::memcpy(d, s, static_cast<size_t>(clipped.w) * bpp);
        }
    }
    inner_->update_texture(t.inner, region, pixels, stride);
}

void ManagedRenderer::destroy_texture(TextureId id) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    inner_->destroy_texture(it->second.inner);
    textures_.erase(it);
}

Result<bool> ManagedRenderer::begin_frame(const Damage& damage) {
    Damage d = damage;
    if (force_full_) d = Damage{{}, true};
    auto r = inner_->begin_frame(d);
    if (!r && handle(r.error())) {
        r = inner_->begin_frame(Damage{{}, true});
    }
    if (r && *r) {
        force_full_ = false;
        in_frame_ = true;
    }
    return r;
}

void ManagedRenderer::submit(const DrawList& dl) {
    if (!in_frame_) return;
    if (identity_) {
        inner_->submit(dl);
        return;
    }
    remapped_ = dl;
    for (Batch& b : remapped_.batches) {
        if (b.texture == 0) continue;
        auto it = textures_.find(b.texture);
        b.texture = it == textures_.end() ? ~TextureId{0} : it->second.inner;
    }
    inner_->submit(remapped_);
}

Result<void> ManagedRenderer::end_frame() {
    if (!in_frame_) return make_error(Errc::invalid_argument, "end_frame without begin_frame");
    in_frame_ = false;
    auto r = inner_->end_frame();
    if (!r && handle(r.error())) return {};   // the new backend draws the next frame in full
    return r;
}

Result<void> ManagedRenderer::present() {
    auto r = inner_->present();
    if (!r) {
        if (handle(r.error())) return {};
        return r;
    }
    if (marker_pending_) {
        clear_crash_marker(state_dir_);
        marker_pending_ = false;
    }
    return r;
}

void ManagedRenderer::copy_rect(IRect src, int32_t dst_x, int32_t dst_y) {
    if (in_frame_) inner_->copy_rect(src, dst_x, dst_y);
}

Result<Image> ManagedRenderer::read_back(IRect rect) {
    auto r = inner_->read_back(rect);
    if (!r && handle(r.error())) return make_error(Errc::unavailable, "the renderer was switched; render a new frame first");
    return r;
}

// ---------------------------------------------------------------- entry point

Result<std::unique_ptr<ManagedRenderer>> create_renderer(std::string_view name, const platform::NativeSurface* surface, PxSize size,
                                                         float scale, RenderConfig config) {
    config.renderer = std::string(name);
    if (config.state_dir.empty()) config.state_dir = default_state_dir();
    const Selection sel = select_backends(config);
    log::info("render", "renderer order: " + [&] {
        std::string s;
        for (BackendKind k : sel.order) s += std::string(s.empty() ? "" : ", ") + backend_name(k);
        return s;
    }() + " (" + sel.why + ")");
    const BackendOptions opts = config.backend;
    auto m = std::make_unique<ManagedRenderer>(
        sel.order, [opts](BackendKind k) { return create_backend(k, opts); }, config.state_dir);
    if (auto ok = m->start(surface, size, scale); !ok) return Unexpected<Error>(ok.error());
    return m;
}

}  // namespace kt::render
