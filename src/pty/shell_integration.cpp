// Shell integration: the embedded scripts, their installation and injection.

#include "pty/shell_integration.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace kt::pty {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------- scripts
// Every script: the user's files run first; hooks are chained; the exit status is read first and
// restored; only builtins run per prompt; __kt_loaded guards a second install;
// KEEPTERM_SHELL_INTEGRATION=0 opts out; KEEPTERM_INTEGRATION switches to the side channel.

constexpr std::string_view kBash = R"KT(# keepterm shell integration for bash. Sourced through --rcfile.
# It runs the user's startup files first and changes none of them.
if [ -n "$KEEPTERM_BASH_LOGIN" ]; then
    unset KEEPTERM_BASH_LOGIN
    if [ -r /etc/profile ]; then . /etc/profile; fi
    if [ -r "$HOME/.bash_profile" ]; then . "$HOME/.bash_profile"
    elif [ -r "$HOME/.bash_login" ]; then . "$HOME/.bash_login"
    elif [ -r "$HOME/.profile" ]; then . "$HOME/.profile"
    fi
else
    if [ -r "$HOME/.bashrc" ]; then . "$HOME/.bashrc"; fi
fi
if [ "$KEEPTERM_SHELL_INTEGRATION" != 0 ] && [ -z "$__kt_loaded" ]; then
    __kt_loaded=1
    __kt_emit() {
        if [ -n "$KEEPTERM_INTEGRATION" ]; then
            printf '%s' "$1" 2>/dev/null >"${KEEPTERM_INTEGRATION//\\//}"
        else
            printf '%s' "$1"
        fi
    }
    __kt_urlencode() {
        local LC_ALL=C s="$1" out= c i v
        for (( i = 0; i < ${#s}; i++ )); do
            c=${s:i:1}
            case "$c" in
                [a-zA-Z0-9/._~-]) out+=$c ;;
                *) printf -v v '%d' "'$c"; (( v < 0 )) && (( v += 256 )); printf -v c '%%%02X' "$v"; out+=$c ;;
            esac
        done
        __kt_enc=$out
    }
    __kt_precmd() {
        local ec=$?
        if [ -n "$__kt_ran" ]; then __kt_emit $'\e]133;D;'"$ec"$'\e\\'; fi
        __kt_ran=1
        if [ -n "$MSYSTEM" ]; then
            local p="$PWD" d re='^/([a-zA-Z])(/.*)?$' lo=abcdefghijklmnopqrstuvwxyz up=ABCDEFGHIJKLMNOPQRSTUVWXYZ
            if [[ $p =~ $re ]]; then
                d=${BASH_REMATCH[1]}
                p=${BASH_REMATCH[2]}
                case "$d" in [a-z]) local head=${lo%%"$d"*}; d=${up:${#head}:1} ;; esac
                p="$d:${p:-/}"
            fi
            __kt_emit $'\e]9;9;"'"${p//\//\\}"$'"\e\\'
        else
            __kt_urlencode "$PWD"
            __kt_emit $'\e]7;file://'"$HOSTNAME$__kt_enc"$'\e\\'
        fi
        __kt_emit $'\e]133;A\e\\'
        return $ec
    }
    if [ -z "$KEEPTERM_INTEGRATION" ] && (( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )); then
        PS0="${PS0}"$'\e]133;C\e\\'
    fi
    # The end of the prompt (OSC 133 B), so that a command line is read without its prompt. It
    # runs last: a prompt the user's PROMPT_COMMAND sets anew gets it too.
    __kt_prompt_end() {
        local ec=$?
        if [ -z "$KEEPTERM_INTEGRATION" ] && [[ $PS1 != *'133;B'* ]]; then PS1+='\[\e]133;B\e\\\]'; fi
        return $ec
    }
    if [[ "$(declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
        PROMPT_COMMAND=(__kt_precmd "${PROMPT_COMMAND[@]}" __kt_prompt_end)
    else
        PROMPT_COMMAND="__kt_precmd${PROMPT_COMMAND:+; $PROMPT_COMMAND}; __kt_prompt_end"
    fi
fi
)KT";

// zsh: ZDOTDIR points at <dir>/zsh. Each file sets ZDOTDIR to the user's value, sources the
// user's file of the same name, then points ZDOTDIR back here for the next startup file. The last
// file that runs restores the user's ZDOTDIR and unsets KEEPTERM_ZDOTDIR.
constexpr std::string_view kZshEnv = R"KT(# keepterm shell integration for zsh: .zshenv
__kt_zdir=${${(%):-%x}:A:h}
__kt_had_zdotdir=${+KEEPTERM_ZDOTDIR}
if (( __kt_had_zdotdir )); then ZDOTDIR=$KEEPTERM_ZDOTDIR; else ZDOTDIR=$HOME; fi
__kt_restore_zdotdir() {
    if (( __kt_had_zdotdir )) || [[ $__kt_user_zdotdir != $HOME ]]; then
        export ZDOTDIR=$__kt_user_zdotdir
    else
        unset ZDOTDIR
    fi
    unset KEEPTERM_ZDOTDIR __kt_user_zdotdir __kt_had_zdotdir __kt_zdir
    unfunction __kt_restore_zdotdir
}
[[ -r $ZDOTDIR/.zshenv ]] && builtin source $ZDOTDIR/.zshenv
__kt_user_zdotdir=$ZDOTDIR
if [[ -o interactive || -o login ]]; then ZDOTDIR=$__kt_zdir; else __kt_restore_zdotdir; fi
)KT";

constexpr std::string_view kZshProfile = R"KT(# keepterm shell integration for zsh: .zprofile
ZDOTDIR=$__kt_user_zdotdir
[[ -r $ZDOTDIR/.zprofile ]] && builtin source $ZDOTDIR/.zprofile
__kt_user_zdotdir=$ZDOTDIR
ZDOTDIR=$__kt_zdir
)KT";

constexpr std::string_view kZshRc = R"KT(# keepterm shell integration for zsh: .zshrc
ZDOTDIR=$__kt_user_zdotdir
[[ -r $ZDOTDIR/.zshrc ]] && builtin source $ZDOTDIR/.zshrc
__kt_user_zdotdir=$ZDOTDIR
if [[ $KEEPTERM_SHELL_INTEGRATION != 0 && -z $__kt_loaded ]]; then
    __kt_loaded=1
    __kt_emit() {
        if [[ -n $KEEPTERM_INTEGRATION ]]; then
            builtin print -rn -- "$1" 2>/dev/null >${KEEPTERM_INTEGRATION//\\//}
        else
            builtin print -rn -- "$1"
        fi
    }
    __kt_urlencode() {
        emulate -L zsh
        setopt extendedglob
        local LC_ALL=C
        REPLY=${1//(#m)[^a-zA-Z0-9\/._~-]/%${(l:2::0:)$(( [##16] #MATCH ))}}
    }
    __kt_precmd() {
        local ec=$? REPLY
        (( ${+__kt_ran} )) && __kt_emit $'\e]133;D;'$ec$'\e\\'
        __kt_ran=1
        __kt_urlencode "$PWD"
        __kt_emit $'\e]7;file://'$HOST$REPLY$'\e\\'
        __kt_emit $'\e]133;A\e\\'
        return $ec
    }
    __kt_preexec() { [[ -z $KEEPTERM_INTEGRATION ]] && __kt_emit $'\e]133;C\e\\' }
    # The end of the prompt (OSC 133 B), so that a command line is read without its prompt. It
    # runs last: a theme that sets the prompt anew in its precmd gets it too.
    __kt_prompt_end() {
        local ec=$?
        [[ -z $KEEPTERM_INTEGRATION && $PS1 != *$'\e]133;B'* ]] && PS1+=$'%{\e]133;B\e\\%}'
        return $ec
    }
    typeset -ga precmd_functions preexec_functions
    precmd_functions=(__kt_precmd $precmd_functions __kt_prompt_end)
    preexec_functions+=(__kt_preexec)
fi
if [[ -o login ]]; then ZDOTDIR=$__kt_zdir; else __kt_restore_zdotdir; fi
)KT";

constexpr std::string_view kZshLogin = R"KT(# keepterm shell integration for zsh: .zlogin
ZDOTDIR=$__kt_user_zdotdir
[[ -r $ZDOTDIR/.zlogin ]] && builtin source $ZDOTDIR/.zlogin
__kt_user_zdotdir=$ZDOTDIR
__kt_restore_zdotdir
)KT";

constexpr std::string_view kFish = R"KT(# keepterm shell integration for fish. Runs through --init-command, after config.fish.
if test "$KEEPTERM_SHELL_INTEGRATION" != 0; and not set -q __kt_loaded
    set -g __kt_loaded 1
    function __kt_emit
        if test -n "$KEEPTERM_INTEGRATION"
            set -l p (string replace -a '\\' / -- $KEEPTERM_INTEGRATION)
            printf '%s' $argv[1] 2>/dev/null >$p
        else
            printf '%s' $argv[1]
        end
    end
    function __kt_prompt --on-event fish_prompt
        set -l ec $status
        __kt_emit (printf '\e]7;file://%s%s\e\\' $hostname (string escape --style=url -- $PWD))
        __kt_emit (printf '\e]133;A\e\\')
        return $ec
    end
    function __kt_preexec --on-event fish_preexec
        test -z "$KEEPTERM_INTEGRATION"; and __kt_emit (printf '\e]133;C\e\\')
    end
    function __kt_postexec --on-event fish_postexec
        __kt_emit (printf '\e]133;D;%s\e\\' $status)
    end
    # The end of the prompt (OSC 133 B), so that a command line is read without its prompt.
    if test -z "$KEEPTERM_INTEGRATION"; and functions -q fish_prompt
        functions -c fish_prompt __kt_user_prompt
        function fish_prompt
            __kt_user_prompt
            printf '\e]133;B\e\\'
        end
    end
end
)KT";

constexpr std::string_view kPwsh = R"KT(# keepterm shell integration for PowerShell 7 and Windows PowerShell 5.1.
# Loaded after the profile; wraps the user's prompt function and still calls it.
if ($env:KEEPTERM_SHELL_INTEGRATION -eq '0' -or $global:__kt_loaded) { return }
$global:__kt_loaded = $true
$global:__kt_orig = $function:prompt
$global:__kt_ran = $false
$global:__kt_pipe = $null
function global:__kt_emit([string]$s) {
    if (-not $env:KEEPTERM_INTEGRATION) { return $s }
    try {
        if (-not $global:__kt_pipe -or -not $global:__kt_pipe.IsConnected) {
            $n = $env:KEEPTERM_INTEGRATION -replace '^\\\\\.\\pipe\\', ''
            $global:__kt_pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $n, [System.IO.Pipes.PipeDirection]::Out)
            $global:__kt_pipe.Connect(50)
        }
        $b = [System.Text.Encoding]::UTF8.GetBytes($s)
        $global:__kt_pipe.Write($b, 0, $b.Length)
        $global:__kt_pipe.Flush()
    } catch { $global:__kt_pipe = $null }
    return ''
}
function global:prompt {
    $ok = $global:?
    $code = $global:LASTEXITCODE
    $e = [char]27
    $out = ''
    if ($global:__kt_ran) {
        $ec = if ($ok) { 0 } elseif ($code) { $code } else { 1 }
        $out += __kt_emit "$e]133;D;$ec$e\"
    }
    $global:__kt_ran = $true
    $loc = $executionContext.SessionState.Path.CurrentLocation
    if ($loc.Provider.Name -eq 'FileSystem') { $out += __kt_emit "$e]9;9;`"$($loc.ProviderPath)`"$e\" }
    $out += __kt_emit "$e]133;A$e\"
    $p = if ($global:__kt_orig) { & $global:__kt_orig } else { "PS $($loc)> " }
    $global:LASTEXITCODE = $code
    $out + $p + (__kt_emit "$e]133;B$e\")
}
)KT";

constexpr std::array<IntegrationFile, 7> kFiles = {{
    {"keepterm.bash", kBash},
    {"zsh/.zshenv", kZshEnv},
    {"zsh/.zprofile", kZshProfile},
    {"zsh/.zshrc", kZshRc},
    {"zsh/.zlogin", kZshLogin},
    {"keepterm.fish", kFish},
    {"keepterm.ps1", kPwsh},
}};

constexpr std::string_view kCmdPrefix = "$E]9;9;$P$E\\$E]133;A$E\\";
constexpr std::string_view kCmdSuffix = "$E]133;B$E\\";

// ---------------------------------------------------------------------------------- helpers

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view basename(std::string_view p) {
#ifdef _WIN32
    const std::size_t slash = p.find_last_of("/\\");
#else
    const std::size_t slash = p.rfind('/');
#endif
    return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

fs::path to_path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string from_path(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string content_hash() {
    uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](std::string_view s) {
        for (char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ull;
        }
        h ^= 0xFFu;
        h *= 0x100000001b3ull;
    };
    for (const auto& f : kFiles) {
        mix(f.relative_path);
        mix(f.content);
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// An environment variable of this process (UTF-8), or nullopt.
std::optional<std::string> get_env(const char* name) {
#ifdef _WIN32
    std::wstring wname;
    for (const char* p = name; *p; ++p) wname.push_back(static_cast<wchar_t>(*p));
    std::wstring buf(32768, L'\0');
    const DWORD n = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return std::nullopt;
    buf.resize(n);
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), out.data(), len, nullptr, nullptr);
    return out;
#else
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

// The value the child will see for `name`: the spec's last edit, else ours.
std::optional<std::string> child_env(const SpawnSpec& spec, std::string_view name) {
    for (auto it = spec.env_edits.rbegin(); it != spec.env_edits.rend(); ++it) {
#ifdef _WIN32
        if (lower(it->name) == lower(name)) return it->value;
#else
        if (it->name == name) return it->value;
#endif
    }
    return get_env(std::string(name).c_str());
}

void set_env(SpawnSpec& spec, std::string name, std::optional<std::string> value) {
    std::erase_if(spec.env_edits, [&](const EnvEdit& e) { return e.name == name; });
    spec.env_edits.push_back({std::move(name), std::move(value)});
}

std::string join(const std::string& dir, std::string_view rel, bool forward) {
    std::string out = dir;
#ifdef _WIN32
    const char sep = forward ? '/' : '\\';
    if (forward) std::replace(out.begin(), out.end(), '\\', '/');
#else
    (void)forward;
    const char sep = '/';
#endif
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back(sep);
    for (char c : rel) out.push_back(c == '/' ? sep : c);
    return out;
}

// POSIX shells: a non-option argument is a script; -c runs a command. `rc_opts` are options the
// user chose that conflict with injection (their own rc file, no rc at all).
bool posix_runs_command(const std::vector<std::string>& args, std::initializer_list<std::string_view> blocking) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") return i + 1 < args.size();
        if (a.empty() || a[0] != '-') return true;  // a script path
        for (std::string_view b : blocking) {
            if (a == b) return true;
        }
        // Grouped single-letter options: -ic, -lc.
        if (a.size() > 1 && a[1] != '-' && a.find('c') != std::string::npos) return true;
    }
    return false;
}

bool pwsh_runs_command(const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.empty()) continue;
        if (a[0] != '-' && a[0] != '/') return true;  // positional: -File (pwsh 7) or -Command (5.1)
        const std::string name = lower(std::string_view(a).substr(1));
        if (name == "c" || name == "f" || name == "e" || name == "ec" || name == "enc") return true;
        for (std::string_view full : {std::string_view("command"), std::string_view("file"),
                                      std::string_view("encodedcommand"), std::string_view("commandwithargs"),
                                      std::string_view("cwa")}) {
            if (name.size() >= 3 && full.substr(0, name.size()) == name) return true;
        }
    }
    return false;
}

bool cmd_runs_command(const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string a = lower(args[i]);
        if (a.size() >= 2 && a[0] == '/' && (a[1] == 'c' || a[1] == 'k')) return true;
    }
    return false;
}

bool has_flag(const std::vector<std::string>& args, std::string_view flag) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (lower(args[i]) == lower(flag)) return true;
    }
    return false;
}

}  // namespace

std::span<const IntegrationFile> integration_files() { return kFiles; }

std::string_view to_string(ShellKind k) {
    switch (k) {
        case ShellKind::Pwsh: return "pwsh";
        case ShellKind::PowerShell: return "powershell";
        case ShellKind::Cmd: return "cmd";
        case ShellKind::Bash: return "bash";
        case ShellKind::Zsh: return "zsh";
        case ShellKind::Fish: return "fish";
        case ShellKind::Wsl: return "wsl";
        case ShellKind::Other: return "other";
    }
    return "other";
}

ShellInfo classify_shell(std::string_view program) {
    std::string name(basename(program));
    if (!name.empty() && name[0] == '-') name.erase(0, 1);
#ifdef _WIN32
    name = lower(name);
    if (name.size() > 4 && name.ends_with(".exe")) name.resize(name.size() - 4);
#endif
    ShellInfo info;
    if (name == "pwsh") info.kind = ShellKind::Pwsh;
    else if (name == "powershell") info.kind = ShellKind::PowerShell;
    else if (name == "cmd") info.kind = ShellKind::Cmd;
    else if (name == "bash") info.kind = ShellKind::Bash;
    else if (name == "zsh") info.kind = ShellKind::Zsh;
    else if (name == "fish") info.kind = ShellKind::Fish;
    else if (name == "wsl") info.kind = ShellKind::Wsl;
#ifdef _WIN32
    if (info.kind == ShellKind::Bash) {
        const std::string_view dir = program.substr(0, program.size() - basename(program).size());
        if (!dir.empty()) {
            std::error_code ec;
            info.msys = fs::exists(to_path(std::string(dir) + "msys-2.0.dll"), ec);
        }
    }
#endif
    return info;
}

Result<std::string> install_integration_scripts(const std::string& state_dir) {
    if (state_dir.empty()) return make_error(Errc::invalid_argument, "shell integration: no state folder");
    std::error_code ec;
    const fs::path root = to_path(state_dir) / "shell-integration";
    const fs::path final_dir = root / content_hash();
    if (fs::is_directory(final_dir, ec)) return from_path(final_dir);

    fs::create_directories(root, ec);
    if (ec) return make_error(Errc::os_error, "shell integration: create folder: " + ec.message(), ec.value());
#ifndef _WIN32
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
    const long pid = static_cast<long>(::getpid());
#else
    const long pid = static_cast<long>(::GetCurrentProcessId());
#endif
    const fs::path tmp = root / ("." + content_hash() + ".tmp-" + std::to_string(pid));
    fs::remove_all(tmp, ec);
    const auto cleanup_fail = [&](const std::string& what) -> Result<std::string> {
        std::error_code ignore;
        fs::remove_all(tmp, ignore);
        return make_error(Errc::os_error, "shell integration: " + what);
    };
    if (!fs::create_directories(tmp / "zsh", ec) || ec) return cleanup_fail("create temp folder");
    for (const auto& f : kFiles) {
        const fs::path p = tmp / to_path(std::string(f.relative_path));
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(f.content.data(), static_cast<std::streamsize>(f.content.size()));
            out.close();
            if (!out) return cleanup_fail("write " + std::string(f.relative_path));
        }
#ifndef _WIN32
        fs::permissions(p, fs::perms::owner_read, fs::perm_options::replace, ec);
#endif
    }
#ifndef _WIN32
    fs::permissions(tmp / "zsh", fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::permissions(tmp, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
    fs::rename(tmp, final_dir, ec);
    if (ec) {
        // Another host of the same build won the race: its folder has the same content.
        std::error_code ignore;
        if (fs::is_directory(final_dir, ignore)) {
            fs::remove_all(tmp, ignore);
            return from_path(final_dir);
        }
        return cleanup_fail("rename: " + ec.message());
    }
    return from_path(final_dir);
}

bool apply_integration(SpawnSpec& spec, const std::string& scripts_dir, const std::string& integration_endpoint) {
    if (spec.argv.empty() || scripts_dir.empty()) return false;
    const ShellInfo info = classify_shell(spec.argv[0]);
    std::vector<std::string>& a = spec.argv;
    switch (info.kind) {
        case ShellKind::Bash: {
            if (posix_runs_command(a, {"--rcfile", "--init-file", "--norc", "--posix", "--command"})) return false;
            bool login = spec.login_shell;
            for (std::size_t i = 1; i < a.size();) {
                if (a[i] == "--login" || a[i] == "-l") {
                    login = true;
                    a.erase(a.begin() + static_cast<std::ptrdiff_t>(i));
                } else if (a[i] == "-i") {
                    a.erase(a.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            spec.login_shell = false;
            a.insert(a.begin() + 1, {"--rcfile", join(scripts_dir, "keepterm.bash", true), "-i"});
            if (login) set_env(spec, "KEEPTERM_BASH_LOGIN", std::string("1"));
            break;
        }
        case ShellKind::Zsh: {
            if (posix_runs_command(a, {"--no-rcs", "-f", "--no-globalrcs"})) return false;
            if (auto z = child_env(spec, "ZDOTDIR")) {
                set_env(spec, "KEEPTERM_ZDOTDIR", *z);
            } else {
                set_env(spec, "KEEPTERM_ZDOTDIR", std::nullopt);
            }
            set_env(spec, "ZDOTDIR", join(scripts_dir, "zsh", true));
            break;
        }
        case ShellKind::Fish: {
            if (posix_runs_command(a, {"--command", "-N", "--no-config"})) return false;
            std::string path = join(scripts_dir, "keepterm.fish", true);
            std::string quoted;
            for (char c : path) {
                if (c == '\'' || c == '\\') quoted.push_back('\\');
                quoted.push_back(c);
            }
            a.insert(a.begin() + 1, {"--init-command", "source '" + quoted + "'"});
            break;
        }
        case ShellKind::Pwsh:
        case ShellKind::PowerShell: {
            if (pwsh_runs_command(a)) return false;
            std::string path = join(scripts_dir, "keepterm.ps1", false);
            std::string quoted;
            for (char c : path) {
                if (c == '\'') quoted.push_back('\'');
                quoted.push_back(c);
            }
            if (!has_flag(a, "-NoLogo")) a.push_back("-NoLogo");
            if (!has_flag(a, "-NoExit")) a.push_back("-NoExit");
            a.push_back("-Command");
            a.push_back(". ([ScriptBlock]::Create([IO.File]::ReadAllText('" + quoted + "')))");
            break;
        }
        case ShellKind::Cmd: {
            if (cmd_runs_command(a)) return false;
            std::string user = child_env(spec, "PROMPT").value_or("");
            if (user.empty()) user = "$P$G";
            set_env(spec, "PROMPT", std::string(kCmdPrefix) + user + std::string(kCmdSuffix));
            break;
        }
        case ShellKind::Wsl:
        case ShellKind::Other:
            return false;
    }
    if (!integration_endpoint.empty()) set_env(spec, "KEEPTERM_INTEGRATION", integration_endpoint);
    return true;
}

}  // namespace kt::pty
