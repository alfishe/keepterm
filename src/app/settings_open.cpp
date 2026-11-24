#include "app/settings_open.h"

#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace kt::app {

Result<void> open_with_default_app(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream create(file, std::ios::app);
        if (!create) return make_error(Errc::os_error, "cannot create " + file.filename().string());
    }
#ifdef _WIN32
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) return make_error(Errc::os_error, "no application opens " + file.filename().string(), r);
    return {};
#else
#ifdef __APPLE__
    const char* tool = "/usr/bin/open";
#else
    const char* tool = "xdg-open";
#endif
    std::string arg = file.string();
    char* argv[] = {const_cast<char*>(tool), arg.data(), nullptr};
    pid_t pid = 0;
#ifdef __APPLE__
    const int rc = posix_spawn(&pid, tool, nullptr, nullptr, argv, environ);
#else
    const int rc = posix_spawnp(&pid, tool, nullptr, nullptr, argv, environ);
#endif
    if (rc != 0) return make_error(Errc::os_error, std::string("cannot run ") + tool, rc);
    // open and xdg-open hand the file over and exit quickly; reap them so no zombie stays.
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return make_error(Errc::os_error, "no application opens " + file.filename().string());
    return {};
#endif
}

}  // namespace kt::app
