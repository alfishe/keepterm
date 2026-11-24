// platform::paths on Windows.
#include <system_error>

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include "platform/paths.h"

namespace kt::platform::paths {
namespace fs = std::filesystem;

namespace {

Result<fs::path> known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr)) {
        if (raw) ::CoTaskMemFree(raw);
        return make_error(Errc::os_error, "SHGetKnownFolderPath", hr);
    }
    fs::path p(raw);
    ::CoTaskMemFree(raw);
    return p;
}

fs::path with_profile(fs::path base, std::string_view profile) {
    if (!profile.empty()) base = base / "profiles" / std::string(profile);
    return base;
}

}  // namespace

fs::path home() {
    if (auto p = known_folder(FOLDERID_Profile)) return *p;
    if (auto h = env_utf8("USERPROFILE"); h && !h->empty()) return from_utf8(*h);
    return fs::path(L"C:\\");
}

Result<fs::path> get(Kind kind, std::string_view profile) {
    if (!profile.empty() && !valid_profile_name(profile))
        return make_error(Errc::invalid_argument, "invalid profile name");
    if (kind == Kind::config) {
        auto roaming = known_folder(FOLDERID_RoamingAppData);
        if (!roaming) return Unexpected(roaming.error());
        return with_profile(*roaming / L"keepterm", profile);
    }
    auto local = known_folder(FOLDERID_LocalAppData);
    if (!local) return Unexpected(local.error());
    const fs::path base = *local / L"keepterm";
    switch (kind) {
        case Kind::state: return with_profile(base / L"state", profile);
        case Kind::runtime: return with_profile(base / L"run", profile);
        case Kind::logs: return with_profile(base / L"logs", profile);
        case Kind::cache: return with_profile(base / L"cache", profile);
        case Kind::config: break;
    }
    return make_error(Errc::invalid_argument, "unknown folder kind");
}

std::vector<fs::path> extra_config_dirs(std::string_view) { return {}; }

std::optional<std::string> env_utf8(std::string_view name) {
    const std::wstring wname = from_utf8(name).wstring();
    std::wstring buf(256, L'\0');
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD n = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
            return std::string();
        }
        if (n < buf.size()) {
            buf.resize(n);
            return to_utf8(fs::path(buf));
        }
        buf.resize(n);
    }
}

Result<void> make_private_dir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return make_error(Errc::os_error, "create_directories: " + ec.message(), ec.value());
    return {};
}

}  // namespace kt::platform::paths
