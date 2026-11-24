// The old per-window files (<state>/windows/<session-id>.json, session/gui_state.h) as legacy panes.
#include "session/gui_state.h"
#include "session/os.h"
#include "workspace/runtime.h"

namespace kt::ws {

std::vector<LegacyPane> read_legacy_panes() {
    std::vector<LegacyPane> out;
    auto paths = session::os::paths();
    if (!paths) return out;
    const auto states = session::read_pane_states(*paths);
    if (states.empty()) return out;
    const auto plan = session::plan_startup(
        states, [&](const session::PaneState& p) { return session::real_pane_facts(*paths, p); });
    for (const auto& item : plan) {
        if (item.action == session::PaneAction::Drop) continue;
        LegacyPane lp;
        lp.session = item.pane.session;
        lp.cwd = item.pane.cwd;
        lp.cwd_source = item.pane.cwd_source;
        lp.title = item.pane.title;
        lp.updated_unix_ms = item.pane.updated_unix_ms;
        out.push_back(std::move(lp));
    }
    return out;
}

void retire_legacy_panes() {
    auto paths = session::os::paths();
    if (!paths) return;
    const std::string dir = session::windows_dir(*paths);
    if (!session::os::is_dir(dir)) return;
    const std::string target = dir + ".imported";
    if (session::os::is_dir(target)) (void)session::os::remove_tree(target);
    (void)session::os::rename_file(dir, target);
}

}  // namespace kt::ws
