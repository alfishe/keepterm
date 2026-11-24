#include "workspace/model.h"

#include <array>

#include "workspace/error.h"

namespace kt::ws {

namespace {
template <class E, std::size_t N>
std::optional<E> lookup(const std::array<std::string_view, N>& names, std::string_view s) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == s) return static_cast<E>(i);
    return std::nullopt;
}

constexpr std::array<std::string_view, 10> kColors = {"none", "red",    "orange", "yellow", "green",
                                                      "teal", "blue",   "purple", "pink",   "gray"};
constexpr std::array<std::string_view, 5> kAttention = {"idle", "running", "done", "failed", "bell"};
constexpr std::array<std::string_view, 5> kCwd = {"integration", "os_query", "process_peb", "prompt_parse",
                                                  "last_known"};
}  // namespace

std::string_view to_string(ColorTag c) { return kColors[static_cast<std::size_t>(c)]; }
std::optional<ColorTag> color_from_string(std::string_view s) { return lookup<ColorTag>(kColors, s); }

std::string_view to_string(Attention a) { return kAttention[static_cast<std::size_t>(a)]; }
std::optional<Attention> attention_from_string(std::string_view s) { return lookup<Attention>(kAttention, s); }

std::string_view to_string(CwdSource c) { return kCwd[static_cast<std::size_t>(c)]; }
std::optional<CwdSource> cwd_source_from_string(std::string_view s) { return lookup<CwdSource>(kCwd, s); }

std::string_view to_string(RestoreStep r) {
    switch (r) {
        case RestoreStep::None: return "none";
        case RestoreStep::Respawning: return "respawning";
        case RestoreStep::HistoryRestored: return "history_restored";
        case RestoreStep::Failed: return "failed";
    }
    return "none";
}

std::string_view to_string(BindState b) {
    switch (b) {
        case BindState::Unbound: return "unbound";
        case BindState::Probing: return "probing";
        case BindState::Spawning: return "spawning";
        case BindState::Ready: return "ready";
        case BindState::Failed: return "failed";
        case BindState::Ended: return "ended";
    }
    return "unbound";
}

std::string_view to_string(WsErrc e) {
    switch (e) {
        case WsErrc::NotFound: return "not_found";
        case WsErrc::Busy: return "busy";
        case WsErrc::LastPane: return "last_pane";
        case WsErrc::InvalidRatio: return "invalid_ratio";
        case WsErrc::InvalidName: return "invalid_name";
        case WsErrc::ScratchImmutable: return "scratch_immutable";
        case WsErrc::SameProject: return "same_project";
        case WsErrc::LayoutParse: return "layout_parse";
        case WsErrc::LayoutNoTabs: return "layout_no_tabs";
        case WsErrc::LayoutNoFile: return "layout_no_file";
        case WsErrc::StateCorrupt: return "state_corrupt";
        case WsErrc::StateNewerSchema: return "state_newer_schema";
        case WsErrc::StateReadOnly: return "state_read_only";
        case WsErrc::Io: return "io";
        case WsErrc::InvalidArgument: return "invalid_argument";
    }
    return "unknown";
}

}  // namespace kt::ws
