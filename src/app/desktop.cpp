#include "app/desktop.h"

#include <algorithm>
#include <cctype>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#elif !defined(__APPLE__)
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace kt::app::desktop {

bool url_allowed(const std::string& url) {
    const auto colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    std::string scheme = url.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kOk[] = {"http", "https", "ftp", "mailto", "ssh", "file", "git"};
    for (const char* s : kOk) {
        if (scheme == s) return url.find_first_of("\r\n") == std::string::npos;
    }
    return false;
}

#if defined(_WIN32)

namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
bool shell_open(const std::string& target) {
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
}
}  // namespace

bool open_url(const std::string& url) { return url_allowed(url) && shell_open(url); }
bool open_folder(const std::string& path) { return !path.empty() && shell_open(path); }
bool open_file(const std::string& path) { return !path.empty() && shell_open(path); }

bool run_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    // ShellExecute finds the program on PATH and App Paths; arguments are quoted for CommandLineToArgvW.
    std::string params;
    for (size_t k = 1; k < argv.size(); ++k) {
        if (k > 1) params += ' ';
        const std::string& a = argv[k];
        if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) {
            params += a;
            continue;
        }
        params += '"';
        size_t slashes = 0;
        for (char ch : a) {
            if (ch == '\\') {
                ++slashes;
                continue;
            }
            if (ch == '"') params.append(slashes * 2 + 1, '\\');
            else params.append(slashes, '\\');
            slashes = 0;
            params += ch;
        }
        params.append(slashes * 2, '\\');
        params += '"';
    }
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide(argv[0]).c_str(), params.empty() ? nullptr : wide(params).c_str(), nullptr, SW_SHOWNORMAL));
    return r > 32;
}

void set_badge(int) {}

namespace {
std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

bool pick_folder(void* native_view, const std::string& title, const std::string& initial,
                 std::function<void(std::string)> done, const Post&) {
    // The UI thread is an STA already, or becomes one here (balanced below).
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(init);
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        if (uninit) CoUninitialize();
        return false;
    }
    DWORD flags = 0;
    if (SUCCEEDED(dlg->GetOptions(&flags))) dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!title.empty()) dlg->SetTitle(wide(title).c_str());
    if (!initial.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wide(initial).c_str(), nullptr, IID_PPV_ARGS(&start))) && start) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    HWND owner = native_view ? GetAncestor(static_cast<HWND>(native_view), GA_ROOT) : nullptr;
    std::string chosen;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                chosen = narrow(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (uninit) CoUninitialize();
    if (!chosen.empty()) done(chosen);
    return true;
}

bool pick_save_file(void* native_view, const std::string& title, const std::string& folder, const std::string& name,
                    std::function<void(std::string)> done, const Post&) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(init);
    IFileSaveDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        if (uninit) CoUninitialize();
        return false;
    }
    DWORD flags = 0;
    if (SUCCEEDED(dlg->GetOptions(&flags))) dlg->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    if (!title.empty()) dlg->SetTitle(wide(title).c_str());
    if (!name.empty()) dlg->SetFileName(wide(name).c_str());
    if (!folder.empty()) {
        std::wstring wf = wide(folder);
        for (wchar_t& c : wf) {
            if (c == L'/') c = L'\\';   // the shell parses native separators only
        }
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wf.c_str(), nullptr, IID_PPV_ARGS(&start))) && start) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    HWND owner = native_view ? GetAncestor(static_cast<HWND>(native_view), GA_ROOT) : nullptr;
    std::string chosen;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                chosen = narrow(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (uninit) CoUninitialize();
    if (!chosen.empty()) done(chosen);
    return true;
}

#elif !defined(__APPLE__)

namespace {
// Starts a helper detached (double fork: no zombie, no wait on the UI thread).
// The argument vector is built before fork(): the child of a threaded process only calls
// async-signal-safe functions.
bool spawn_detached(std::vector<std::string> argv) {
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (fork() != 0) _exit(0);
        execvp(args[0], args.data());
        _exit(127);
    }
    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR) {}
    return true;
}
}  // namespace

bool open_url(const std::string& url) { return url_allowed(url) && spawn_detached({"xdg-open", url}); }
bool open_folder(const std::string& path) { return !path.empty() && spawn_detached({"xdg-open", path}); }
bool open_file(const std::string& path) { return !path.empty() && spawn_detached({"xdg-open", path}); }
bool run_detached(const std::vector<std::string>& argv) { return !argv.empty() && spawn_detached(argv); }

void set_badge(int) {}

namespace {
// The first of `names` that is an executable on PATH.
std::string find_on_path(std::initializer_list<const char*> names) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
    for (const char* n : names) {
        std::string dirs = path;
        size_t start = 0;
        while (start <= dirs.size()) {
            const size_t end = std::min(dirs.find(':', start), dirs.size());
            std::string cand = dirs.substr(start, end - start);
            if (!cand.empty()) {
                cand += '/';
                cand += n;
                struct stat st {};
                if (::stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(cand.c_str(), X_OK) == 0) return cand;
            }
            start = end + 1;
        }
    }
    return {};
}

// Runs `argv`, returns its standard output (empty on failure or a non-zero exit).
std::string capture_output(std::vector<std::string> argv) {
    int fds[2];
    if (pipe(fds) != 0) return {};
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return {};
    }
    if (child == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execv(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0) out.append(buf, static_cast<size_t>(n));
        else if (n == 0 || errno != EINTR) break;
    }
    close(fds[0]);
    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
}  // namespace

bool pick_folder(void*, const std::string& title, const std::string& initial, std::function<void(std::string)> done,
                 const Post& post) {
    std::vector<std::string> argv;
    if (std::string z = find_on_path({"zenity"}); !z.empty()) {
        argv = {z, "--file-selection", "--directory", "--title=" + title};
        if (!initial.empty()) argv.push_back("--filename=" + initial + (initial.back() == '/' ? "" : "/"));
    } else if (std::string k = find_on_path({"kdialog"}); !k.empty()) {
        argv = {k, "--title", title, "--getexistingdirectory", initial.empty() ? std::string(".") : initial};
    } else {
        return false;
    }
    if (!post) return false;
    std::thread([argv = std::move(argv), done = std::move(done), post] {
        std::string chosen = capture_output(argv);
        if (!chosen.empty()) post([done, chosen] { done(chosen); });
    }).detach();
    return true;
}

bool pick_save_file(void*, const std::string& title, const std::string& folder, const std::string& name,
                    std::function<void(std::string)> done, const Post& post) {
    std::string start = folder.empty() ? name : folder + (folder.back() == '/' ? "" : "/") + name;
    std::vector<std::string> argv;
    if (std::string z = find_on_path({"zenity"}); !z.empty()) {
        argv = {z, "--file-selection", "--save", "--confirm-overwrite", "--title=" + title, "--filename=" + start};
    } else if (std::string k = find_on_path({"kdialog"}); !k.empty()) {
        argv = {k, "--title", title, "--getsavefilename", start};
    } else {
        return false;
    }
    if (!post) return false;
    std::thread([argv = std::move(argv), done = std::move(done), post] {
        std::string chosen = capture_output(argv);
        if (!chosen.empty()) post([done, chosen] { done(chosen); });
    }).detach();
    return true;
}

#endif

}  // namespace kt::app::desktop
