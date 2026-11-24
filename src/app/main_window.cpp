#include "app/main_window.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <future>
#include <thread>
#include <vector>

#include "agents/capture.h"
#include "agents/os_access.h"
#include "agents/registry.h"
#include "agents/session_files.h"
#include "app/desktop.h"
#include "app/clip_image.h"
#include "app/gui_input.h"
#include "app/palette_controller.h"
#include "app/pane_session.h"
#include "app/workspace_adapter.h"
#include "base/log.h"
#include "commands/keymap.h"
#include "commands/cli.h"
#include "commands/registry.h"
#include "commands/resolver.h"
#include "commands/workspace_target.h"
#include "config/config.h"
#include "config/service.h"
#include "platform/paths.h"
#include "workspace/paths.h"
#include "platform_gui/appearance.h"
#include "render/render_select.h"
#include "resources/collector.h"
#include "session/agent_statusline.h"
#include "session/client.h"
#include "session/proc_chain.h"
#include "session/os.h"
#include "session/registry.h"
#include "session/suspend.h"
#include "app/control.h"
#include "app/pane_sample.h"
#include "session/pane_config.h"
#include "store/catalog.h"
#include "store/retention.h"
#include "store/usage.h"
#include "text/font.h"
#include "text/font_source.h"
#include "theme/apply.h"
#include "theme/library.h"
#include "theme/theme_set.h"
#include "ui/find_bar.h"
#include "ui/resource_panel.h"
#include "view/terminal/text_ops.h"
#include "ui/text_service.h"
#include "ui/window.h"
#include "ui/workspace_widgets.h"
#include "app/settings_controller.h"
#include "app/settings_open.h"
#include "app/limits_refresher.h"
#include "app/ui_window_host.h"
#include "ui/settings_view.h"
#include "view/terminal/atlas_textures.h"
#include "workspace/preset_store.h"
#include "workspace/project_file.h"
#include "workspace/presets.h"
#include "workspace/runtime.h"
#include "workspace/workspace.h"

#ifdef __APPLE__
#include "app/mac_window.h"
#endif

namespace kt::app {

namespace {

using platform::Clock;
namespace cmd = commands;

#ifdef __APPLE__
constexpr bool kMac = true;
#else
constexpr bool kMac = false;
#endif
#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif
constexpr float kMinFontPt = 4.0f, kMaxFontPt = 96.0f;   // font.size range
constexpr std::chrono::milliseconds kMonitorInterval{2000};
constexpr int kDiscoverEvery = 5;   // rounds between two looks for detached hosts
constexpr int kStorageEvery = 15;                          // storage figures every 30 s
constexpr size_t kHistory = 40;                            // sparkline samples

std::string_view as_chars(std::u8string_view s) { return {reinterpret_cast<const char*>(s.data()), s.size()}; }

bool fail(const char* what, const Error& e) {
    std::fprintf(stderr, "keepterm: %s: %s\n", what, e.what.c_str());
    return false;
}

std::string home_dir() {
    std::string s = platform::paths::env_utf8("HOME").value_or("");
    if (s.empty()) s = platform::paths::env_utf8("USERPROFILE").value_or("");
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

ui::MotionLevel motion_of(const std::string& v) {
    if (v == "none") return ui::MotionLevel::None;
    if (v == "reduced") return ui::MotionLevel::Reduced;
    return ui::MotionLevel::Full;
}

// ---------------------------------------------------------------- the resource and storage monitor

// What a host reported about its pane, plus the collector's figures.
struct PaneStats : PaneSample {
    ui::Resources res;
};

// A live session host no pane of this window shows (the sessions list, the palette's "!").
struct DetachedSession {
    std::string id, endpoint, cwd, program, agent, saved_agent;
    int gui_clients = 0;
};

struct MonitorReport {
    std::map<std::string, PaneStats> sessions;   // by session id
    std::optional<std::vector<DetachedSession>> detached;   // every few rounds, else unset
    std::map<std::string, ui::SessionTree> trees;   // by session id (the resource popover)
    ui::Resources total;                         // the GUI plus every session
    std::optional<ui::StorageUsage> storage;
};

// OS-native process start time (resources::ProcessKey units) to Unix milliseconds; 0 = unknown.
uint64_t start_unix_ms(uint64_t native) {
    if (native == 0) return 0;
#if defined(__APPLE__)
    return native / 1000;   // microseconds since the epoch
#elif defined(_WIN32)
    constexpr uint64_t kEpoch = 116444736000000000ull;   // FILETIME of 1970-01-01
    return native > kEpoch ? (native - kEpoch) / 10000 : 0;
#else
    return 0;               // clock ticks since boot: the chain entry has the Unix time
#endif
}

// What a session's host reported about the processes in front (command lines, folders).
struct ChainFacts {
    std::map<uint32_t, session::proto::ChainEntry> chain;
    uint32_t root = 0;
    std::optional<uint32_t> agent;
};

ui::ProcItem to_item(const resources::ProcNode& n, const ChainFacts& f) {
    ui::ProcItem it;
    it.pid = n.key.pid;
    it.start = n.key.start_time;
    it.name = n.synthetic ? std::string("other") : n.name;
    switch (n.role) {
        case resources::Role::Gui: it.role = "keepterm"; break;
        case resources::Role::SessionHost: it.role = "session host"; break;
        case resources::Role::ConsoleAgent: it.role = "console agent"; break;
        case resources::Role::ConsoleHost: it.role = "console host"; break;
        case resources::Role::Workload:
            if (n.key.pid == f.root) it.role = "shell";
            else if (f.agent && n.key.pid == *f.agent) it.role = "agent";
            break;
    }
    it.killable = n.role == resources::Role::Workload && !n.synthetic;
    it.keepterm = n.role != resources::Role::Workload && !n.synthetic;
    it.no_access = n.no_access;
    it.memory = n.self.main;
    it.subtree_memory = n.subtree.main;
    it.cpu = n.self.cpu_percent.value_or(0.0f);
    it.subtree_cpu = n.subtree.cpu_percent.value_or(0.0f);
    it.resident = n.self.resident.value_or(0);
    it.private_resident = n.self.private_resident.value_or(0);
    it.peak = n.self.peak.value_or(0);
    it.committed = n.self.committed.value_or(0);
    it.cpu_ns = n.self.cpu_ns;
    it.threads = n.self.threads;
    it.start_unix_ms = start_unix_ms(n.key.start_time);
    if (auto c = f.chain.find(n.key.pid); c != f.chain.end()) {
        for (const auto& a : c->second.argv) it.cmdline += (it.cmdline.empty() ? "" : " ") + a;
        it.cwd = c->second.cwd;
        if (c->second.start_unix_ns) it.start_unix_ms = c->second.start_unix_ns / 1000000;
    }
#ifdef KT_HAVE_SESSIONS
    // Every user process gets its command line (several python.exe tell apart by it); the
    // reader caches it per process on Windows.
    if (it.cmdline.empty() && n.role == resources::Role::Workload && !n.synthetic && n.key.pid) {
        for (const auto& a : session::process_argv(n.key.pid)) {
            const bool quote = a.find(' ') != std::string::npos;
            it.cmdline += (it.cmdline.empty() ? "" : " ") + (quote ? "\"" + a + "\"" : a);
        }
    }
#endif
    it.tip = "pid " + std::to_string(it.pid) + " \xC2\xB7 private " + ui::format_bytes(it.memory);
    if (it.resident) it.tip += " \xC2\xB7 resident " + ui::format_bytes(it.resident);
    if (it.peak) it.tip += " \xC2\xB7 peak " + ui::format_bytes(it.peak);
    it.tip += "\nfound by: " + std::string(resources::to_string(n.how));
    if (!it.cmdline.empty()) it.tip += "\n" + it.cmdline;
    for (const auto& c : n.children) it.children.push_back(to_item(c, f));
    return it;
}

// A worker thread: PROCESS_INFO of every attached session, one collector tick, and now and then
// the history store's usage. Results go to the UI thread through `post`.
class Monitor {
public:
    using Post = std::function<void(std::function<void()>)>;
    Monitor(Post post, std::function<void(MonitorReport)> on_report, std::filesystem::path store_root, store::Policy policy)
        : post_(std::move(post)), on_report_(std::move(on_report)), store_root_(std::move(store_root)), policy_(policy) {
        resources::CollectorConfig cc;
        cc.interval = kMonitorInterval;
        collector_ = std::make_unique<resources::Collector>(cc);
        thread_ = std::thread([this] { run(); });
    }
    ~Monitor() {
        {
            std::lock_guard lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    // A fresh round now (a pane menu action changed a host's state); `discover`: list the
    // live hosts again in it.
    void poke(bool discover = false) {
        {
            std::lock_guard lk(mu_);
            poke_ = true;
            discover_ = discover_ || discover;
        }
        cv_.notify_all();
    }
    // Session id -> endpoint of the panes to watch (UI thread).
    void set_sessions(std::vector<std::pair<std::string, std::string>> s) {
        std::lock_guard lk(mu_);
        sessions_ = std::move(s);
    }

private:
    static std::string agent_of_chain(const std::vector<session::proto::ChainEntry>& chain) { return app::agent_of_chain(chain); }

    void run() {
        int tick = 0;
        for (;;) {
            std::vector<std::pair<std::string, std::string>> sessions;
            bool discover = false;
            {
                std::unique_lock lk(mu_);
                if (tick > 0) cv_.wait_for(lk, kMonitorInterval, [this] { return stop_ || poke_; });
                if (stop_) return;
                poke_ = false;
                sessions = sessions_;
                discover = discover_ || tick % kDiscoverEvery == 0;
                discover_ = false;
            }
            MonitorReport rep;
            std::vector<resources::SessionProcessInfo> infos;
            std::map<std::string, ChainFacts> facts;
#ifdef KT_HAVE_SESSIONS
            for (const auto& [id, endpoint] : sessions) {
                auto pi = session::request_process_info(endpoint, 500);
                if (!pi) continue;
                resources::SessionProcessInfo s;
                s.id = id;
                if (pi->host_pid) s.host = resources::ProcessKey{pi->host_pid, pi->host_start};
                s.root = resources::ProcessKey{pi->root_pid, pi->root_start};
                if (pi->agent_pid) s.agent = resources::ProcessKey{*pi->agent_pid, 0};
                if (pi->containment_kind <= static_cast<uint8_t>(resources::ContainmentKind::None))
                    s.containment.kind = static_cast<resources::ContainmentKind>(pi->containment_kind);
                s.containment.handle = pi->containment_handle;
                for (uint32_t c : pi->children) s.tracked.push_back(resources::ProcessKey{c, 0});
                if (pi->posix_sid) s.posix_sid = static_cast<int32_t>(*pi->posix_sid);
                s.tty_dev = pi->tty_dev;
                infos.push_back(std::move(s));
                PaneStats& ps = rep.sessions[id];
                static_cast<PaneSample&>(ps) = sample_pane(*pi, endpoint);
                ChainFacts& cf = facts[id];
                cf.root = pi->root_pid;
                cf.agent = pi->agent_pid;
                for (const auto& e : pi->foreground_chain) cf.chain[e.pid] = e;
            }
            if (discover) rep.detached = detached(sessions);
#endif
            collector_->set_sessions(std::move(infos));
            if (auto snap = collector_->tick(); snap && *snap && !(*snap)->machines.empty()) {
                const resources::MachineSample& m = (*snap)->machines.front();
                rep.total.valid = true;
                // Figures are the user's processes only: keepterm's own cost is overhead (the
                // resource popover shows it per pane), not part of a tab's or the status bar's total.
                for (const resources::SessionSample& s : m.sessions) {
                    ui::SessionTree& tr = rep.trees[s.id];
                    tr.session = s.id;
                    tr.containment = std::string(resources::to_string(s.containment.kind));
                    tr.accuracy = std::string(resources::to_string(s.accuracy));
                    ui::ProcItem whole = to_item(s.root, facts[s.id]);
                    ui::split_session_tree(std::move(whole), tr);
                    PaneStats& ps = rep.sessions[s.id];
                    ps.res.valid = true;
                    ps.res.memory = tr.memory;
                    ps.res.cpu = tr.cpu;
                    rep.total.memory += tr.memory;
                    rep.total.cpu += tr.cpu;
                }
            }
            if (tick % kStorageEvery == 0) rep.storage = storage();
            ++tick;
            post_([this, rep = std::move(rep)]() mutable { on_report_(std::move(rep)); });
        }
    }

#ifdef KT_HAVE_SESSIONS
    // The live hosts of this profile minus the watched ones, with what runs in them.
    static std::vector<DetachedSession> detached(const std::vector<std::pair<std::string, std::string>>& watched) {
        std::vector<DetachedSession> out;
        auto paths = session::os::paths();
        if (!paths) return out;
        const session::Registry reg(*paths);
        for (const session::DiscoveredSession& d : reg.discover()) {
            if (d.state != session::HostState::Live) continue;
            const std::string id = d.record.id.str();
            bool seen = false;
            for (const auto& w : watched) seen = seen || w.first == id;
            if (seen) continue;
            DetachedSession s;
            s.id = id;
            s.endpoint = d.record.endpoint;
            if (d.hello) s.gui_clients = d.hello->gui_clients();
            if (auto pi = session::request_process_info(s.endpoint, 500)) {
                s.agent = agent_of_chain(pi->foreground_chain);
                s.cwd = pane_cwd(*pi, s.agent);
                if (!pi->foreground_is_shell && !pi->foreground_chain.empty()) s.program = pi->foreground_chain.back().image;
                if (s.program.empty()) s.program = pi->shell_image;
                s.saved_agent = pi->saved_agent;
            }
            out.push_back(std::move(s));
        }
        return out;
    }
#endif

    ui::StorageUsage storage() {
        ui::StorageUsage u;
        std::error_code ec;
        if (store_root_.empty() || !std::filesystem::exists(store_root_ / store::kCatalogFile, ec)) {
            u.label = "No history yet";
            return u;
        }
        if (!catalog_) {
            auto c = store::Catalog::open(store_root_ / store::kCatalogFile);
            if (!c) {
                u.label = "History unavailable";
                return u;
            }
            catalog_ = std::move(*c);
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        auto r = store::usage(*catalog_, store_root_, policy_, static_cast<uint64_t>(now.count()));
        if (!r) {
            u.label = "History unavailable";
            return u;
        }
        u.valid = true;
        u.used = r->total.used;
        u.limit = r->total.limit;
        return u;
    }

    Post post_;
    std::function<void(MonitorReport)> on_report_;
    std::filesystem::path store_root_;
    store::Policy policy_;
    std::unique_ptr<store::Catalog> catalog_;
    std::unique_ptr<resources::Collector> collector_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool poke_ = false;
    bool discover_ = false;
    std::vector<std::pair<std::string, std::string>> sessions_;
    std::thread thread_;
};

// Where the hosts write the history: their state folder (KEEPTERM_STATE_DIR, KEEPTERM_PROFILE).
std::filesystem::path store_root() {
    if (auto p = session::os::paths()) return platform::paths::from_utf8(p->state) / "history";
    if (auto e = platform::paths::env_utf8("KEEPTERM_STATE_DIR"); e && !e->empty()) return platform::paths::from_utf8(*e) / "history";
    auto p = platform::paths::get(platform::paths::Kind::state);
    return p ? *p / "history" : std::filesystem::path();
}

// ---------------------------------------------------------------- the window

class MainWindow;

// The quick terminal's chord as a platform hotkey: the physical key named after the US layout
// (`ctrl+grave` is the key left of 1 on every layout).
std::optional<platform::GlobalHotkey> global_hotkey_of(const cmd::Sequence& s) {
    if (s.length != 1) return std::nullopt;
    const cmd::Chord& c = s.chords[0];
    platform::GlobalHotkey hk;
    if (c.mods & cmd::Ctrl) hk.mods |= platform::mod_ctrl;
    if (c.mods & cmd::Shift) hk.mods |= platform::mod_shift;
    if (c.mods & cmd::Alt) hk.mods |= platform::mod_alt;
    if (c.mods & cmd::Super) hk.mods |= platform::mod_super;
    using K = platform::Key;
    const cmd::Key& k = c.key;
    if (k.kind == cmd::Key::Kind::Physical) {
        hk.key = static_cast<K>(k.physical);
    } else if (k.kind == cmd::Key::Kind::Char) {
        static constexpr std::string_view kRow = "`1234567890-=[]\\;',./ ";
        static constexpr K kRowKeys[] = {K::grave, K::d1, K::d2, K::d3, K::d4, K::d5, K::d6, K::d7, K::d8, K::d9,
                                         K::d0, K::minus, K::equal, K::bracket_left, K::bracket_right, K::backslash,
                                         K::semicolon, K::quote, K::comma, K::period, K::slash, K::space};
        const char32_t ch = k.ch;
        if (ch >= U'a' && ch <= U'z') {
            hk.key = static_cast<K>(static_cast<int>(K::a) + static_cast<int>(ch - U'a'));
        } else if (ch < 128) {
            if (const auto i = kRow.find(static_cast<char>(ch)); i != std::string_view::npos) hk.key = kRowKeys[i];
        }
    } else if (k.kind == cmd::Key::Kind::Named) {
        const auto n = static_cast<int>(k.named);
        if (n >= static_cast<int>(cmd::NamedKey::F1) && n <= static_cast<int>(cmd::NamedKey::F12))
            hk.key = static_cast<K>(static_cast<int>(K::f1) + n - static_cast<int>(cmd::NamedKey::F1));
        else if (k.named == cmd::NamedKey::Space) hk.key = K::space;
        else if (k.named == cmd::NamedKey::Enter) hk.key = K::enter;
        else if (k.named == cmd::NamedKey::Tab) hk.key = K::tab;
        else if (k.named == cmd::NamedKey::Escape) hk.key = K::escape;
    }
    if (hk.key == K::unknown) return std::nullopt;
    return hk;
}

// What the windows of one process share: the workspace model and its
// runtime, the control channel, the process monitor and the way out. Every window is a
// MainWindow with its own renderer, widgets and pane mirrors (a session host takes several
// GUI clients); the first open window is the owner that forwards session events to the model.
struct AppCore {
    AppCore(platform::IPlatform& platform, MainWindowOptions o) : p(platform), opts(std::move(o)) {}
    ~AppCore();
    platform::IPlatform& p;
    MainWindowOptions opts;
    std::unique_ptr<ws::WorkspaceRuntime> rt;
    std::unique_ptr<ws::Workspace> local_ws;
    std::unique_ptr<Monitor> monitor;
#ifdef KT_HAVE_SESSIONS
    std::unique_ptr<control::Server> control;
    std::atomic<bool> control_stopping{false};
#endif
    std::vector<MainWindow*> windows;   // open, the owner first
    MainWindow* menu_owner = nullptr;   // macOS: the window the menu bar acts on
    bool quitting = false;              // quit or exit: the last closed window ends the loop
    int exit_code = 0;
    // The quick terminal: one panel, made on first use, hidden and shown again.
    std::unique_ptr<MainWindow> quick;
    bool quick_shown = false;
    uint64_t quick_prev = 0;                   // the window in front before it showed
    std::optional<platform::ScreenArea> quick_area;   // the screen it was placed on
    std::optional<platform::HotkeyId> hotkey;
    std::string hotkey_chord, hotkey_error;    // what is registered, or why it is not
    std::vector<std::unique_ptr<MainWindow>> owned;   // last: the windows go first

    MainWindow* owner() const { return windows.empty() ? nullptr : windows.front(); }
    // Registers the global hotkey from the quick_terminal.toggle binding (none when disabled).
    void sync_hotkey(const config::Config& c, const cmd::Keymap& km);
    bool show_quick();
    void hide_quick(bool give_back_focus = true);
    void toggle_quick() {
        if (quick_shown) hide_quick();
        else (void)show_quick();
    }
    void quick_closed(MainWindow*);
    // Opens a window on `wid` (none: the restored key window). False when it could not start.
    bool open(std::optional<ws::WindowId> wid, std::optional<platform::DipRect> place = {});
    void closed(MainWindow*);   // the native window went: forget it, end the loop after the last
    MainWindow* route() const;   // the window a control request is for: the key window
    void quit_all();
};

// The quick terminal's default size at most, when width and height are not set.
constexpr platform::DipSize kQuickDefaultMax{1100, 600};

class MainWindow final : public platform::IWindowListener, public cmd::ICommandTarget {
    friend struct AppCore;

public:
    MainWindow(AppCore& core, std::optional<ws::WindowId> wid, std::optional<platform::DipRect> place, bool quick = false)
        : p_(core.p), opts_(core.opts), core_(core), want_window_(wid), place_(place), quick_(quick),
#ifdef KT_HAVE_SESSIONS
          control_(core.control),
#endif
          rt_(core.rt), local_ws_(core.local_ws), monitor_(core.monitor) {
        env_.loop = &p_.loop();
        env_.clipboard = &p_.clipboard();
        env_.copy_on_select = [this] { return cfg_.terminal_copy_on_select(); };
        env_.request_frame = [this] {
            if (win_) win_->request_frame();
        };
        env_.consume_key = [this] {
            if (win_) win_->consume_key();
        };
    }

    ~MainWindow() override {
        dying_ = true;
        appearance_.reset();
        ws_sub_.reset();
        cfg_subs_.clear();
        cfg_svc_.reset();
        if (probe_thread_.joinable()) probe_thread_.join();
        *alive_ = false;
        for (auto& t : workers_)
            if (t.joinable()) t.join();
        palette_.reset();
        uiw_.reset();   // the widgets reference panes and the adapter
        panes_.clear();
        target_.reset();
        adapter_.reset();
        if (rt_) rt_->flush();
        drop_render();
        if (win_) win_->close();
        win_.reset();
        if (set_ && fonts_) fonts_->release(set_);
    }

    ws::WindowId window_id() const { return window_id_; }
    void close_native() {
        if (rt_) rt_->flush();
        if (win_) win_->close();
        else if (quick_) core_.quick_closed(this);
        else core_.closed(this);
    }

    // ------------------------------------------------------------ IWindowListener

    // The window's close button: this window goes. The last one keeps its place in the model
    // (it opens again at the next start); another one is closed there too.
    void on_close_requested() override {
        if (quick_) {   // the panel only hides; its tabs stay
            core_.hide_quick();
            return;
        }
        if (core_.windows.size() > 1 && !core_.quitting) (void)model().close_window(window_id_);
        close_native();
    }

    void on_destroyed() override {
        if (dying_) return;
        drop_render();
        if (quick_) {
            core_.quick_closed(this);
            return;
        }
        core_.exit_code = exit_code_;
        core_.closed(this);
    }

    void on_resized(platform::PxSize px, platform::DipSize dip) override {
        if (!r_) return;
        px_ = px;
        r_->resize(render::PxSize{px.w, px.h}, scale_);
        uiw_->resize(ui::Size{dip.w, dip.h}, scale_);
        full_next_ = true;
        win_->request_frame();
    }

    void on_scale_changed(float scale, platform::DipRect) override {
        if (!r_ || scale <= 0) return;
        scale_ = scale;
        env_.scale = scale;
        if (!load_font()) return;
        px_ = win_->client_size_px();
        r_->resize(render::PxSize{px_.w, px_.h}, scale_);
        uiw_->resize(ui::Size{static_cast<float>(px_.w) / scale_, static_cast<float>(px_.h) / scale_}, scale_);
        full_next_ = true;
        win_->request_frame();
    }

    void on_focus(bool focused) override {
        if (!uiw_) return;
        uiw_->on_focus(focused);
        if (resolver_) resolver_->reset();
        if (focused && adapter_) (void)model().set_key_window(window_id_);
        // The panel hides when the user goes elsewhere (quick_terminal.hide_on_focus_loss); the
        // check waits a moment, so a focus bounce inside an activation does not hide it.
        if (quick_ && !focused && cfg_.quick_terminal_hide_on_focus_loss()) {
            p_.loop().add_timer(platform::Clock::now() + std::chrono::milliseconds(150),
                                [core = &core_, alive = std::weak_ptr<bool>(alive_), this] {
                                    if (alive.expired() || !core->quick_shown || moving_ || (win_ && win_->focused())) return;
                                    core->hide_quick(false);
                                });
        }
#ifdef __APPLE__
        if (focused && adapter_ && core_.menu_owner != this) install_mac_menu();   // the menu acts on this window
#endif
        win_->request_frame();
    }

    void on_move_size(bool active) override {
        moving_ = active;
        // A drag of the panel is not leaving it: it keeps the keys afterwards.
        if (!active && quick_ && core_.quick_shown && win_ && !win_->focused()) win_->show(true);
    }

    void on_occluded(bool occluded) override {
        if (!occluded && win_) {
            full_next_ = true;
            win_->request_frame();
        }
    }

    void on_expose() override {
        if (!win_) return;
        full_next_ = true;
        win_->request_frame();
    }

    void on_frame(Clock::time_point) override { draw(); }

    void on_key(const platform::KeyEvent& k) override {
        if (uiw_) uiw_->on_key(k);
    }
    void on_text(std::u8string_view text) override {
        if (uiw_ && !text.empty()) uiw_->on_text(as_chars(text));
    }
    void on_composition(const platform::Composition& c) override {
        if (uiw_) uiw_->on_composition(c);
    }
    void on_mouse(const platform::MouseEvent& m) override {
        // The panel's edges show a resize cursor (Windows sets it itself: they are non-client).
        if (quick_ && win_ && m.type == platform::MouseEvent::Type::move) {
            using platform::CursorShape, platform::HitArea;
            CursorShape c = CursorShape::arrow;
            switch (quick_hit(m.pos)) {
                case HitArea::resize_e: c = CursorShape::frame_e; break;
                case HitArea::resize_w: c = CursorShape::frame_w; break;
                case HitArea::resize_n: c = CursorShape::frame_n; break;
                case HitArea::resize_s: c = CursorShape::frame_s; break;
                case HitArea::resize_ne: c = CursorShape::frame_ne; break;
                case HitArea::resize_sw: c = CursorShape::frame_sw; break;
                case HitArea::resize_nw: c = CursorShape::frame_nw; break;
                case HitArea::resize_se: c = CursorShape::frame_se; break;
                default: break;
            }
            if (c != CursorShape::arrow) {
                edge_cursor_ = true;
                win_->set_cursor(c);
            } else if (edge_cursor_) {
                edge_cursor_ = false;
                win_->set_cursor(ui_cursor_);
            }
        }
        if (uiw_) uiw_->on_mouse(m);
    }
    void on_scroll(const platform::ScrollEvent& s) override {
        if (uiw_) uiw_->on_scroll(s);
    }

    // Drag and drop: files onto a pane paste their paths as shell words, text pastes as is;
    // folders onto the sidebar become projects. A read-only pane takes nothing.
    const ws::Pane* drop_pane(ui::Vec2 p, ui::PaneKey& key) const {
        key = mv_ ? mv_->pane_at(p) : 0;
        if (!key) return nullptr;
        const auto id = adapter_->pane_id(key);
        const ws::Pane* mp = id ? const_cast<MainWindow*>(this)->model().pane(*id) : nullptr;
        return mp && !mp->read_only && panes_.count(key) ? mp : nullptr;
    }
    platform::DropEffect on_drag_over(const platform::DragOverEvent& d) override {
        if (!mv_) return platform::DropEffect::none;
        const ui::Vec2 p{d.pos.x, d.pos.y};
        if (mv_->over_sidebar(p)) return d.has_paths ? platform::DropEffect::copy : platform::DropEffect::none;
        ui::PaneKey k = 0;
        return drop_pane(p, k) ? platform::DropEffect::copy : platform::DropEffect::none;
    }
    void on_drop(const platform::DropEvent& e) override {
        if (!mv_) return;
        const ui::Vec2 p{e.pos.x, e.pos.y};
        std::vector<std::string> paths;
        for (const auto& fp : e.paths) paths.push_back(platform::paths::to_utf8(fp));
        if (mv_->over_sidebar(p)) {
            std::size_t added = 0;
            for (const std::string& path : paths) {
                std::error_code ec;
                if (!std::filesystem::is_directory(platform::paths::from_utf8(path), ec)) continue;
                create_project(path);
                ++added;
            }
            if (!added && !paths.empty()) toast(std::string(ui::tr("Drop a folder to add a project")));
            return;
        }
        ui::PaneKey k = 0;
        const ws::Pane* mp = drop_pane(p, k);
        if (!mp) return;
        const std::string text = paths.empty() ? std::string(e.text.begin(), e.text.end()) : app::quote_paths(paths, kWindows);
        if (text.empty()) return;
        (void)model().focus_pane(mp->id);
        mv_->sync();
        if (auto it = panes_.find(k); it != panes_.end() && it->second)
            it->second->paste(std::u8string(text.begin(), text.end()));
    }

    // ------------------------------------------------------------ ICommandTarget: the view's part

    cmd::TargetState state() const override {
        cmd::TargetState st;
        st.has_window = true;
        st.has_terminal = focused_pane() != nullptr;
        st.sidebar_visible = mv_ && mv_->sidebar_visible();
        st.fullscreen = fullscreen_;
        st.history_enabled = cfg_.session_history();
        if (auto pr = shown_real_project()) st.has_layout_file = has_project_file(*pr);
        if (palette_) palette_->fill_state(st);
        return st;
    }

    void notify(std::string_view message) override { toast(std::string(message)); }

    cmd::Status toggle_palette() override {
        if (!palette_) return no("palette");
        palette_->toggle();
        return {};
    }
    cmd::Status open_palette_scope(cmd::PaletteScope sc) override {
        if (!palette_) return no("palette");
        palette_->open(sc);
        return {};
    }
    cmd::Status palette_nav(cmd::PaletteNav n) override {
        if (!palette_ || !palette_->nav(n)) return no("palette navigation");
        return {};
    }
    cmd::Status begin_rename_tab() override {
        const ui::TabItem* t = adapter_->snapshot().shown_tab();
        if (!t) return no("rename tab");
        mv_->rename_tab_dialog(t->id);
        return {};
    }
    cmd::Status begin_rename_project() override {
        const ui::ProjectItem* p = adapter_->snapshot().shown_project();
        if (!p || p->scratch) return no("rename project");
        mv_->rename_project_dialog(p->id);
        return {};
    }
    cmd::Status split_pane(cmd::SplitDir d) override {
        if (d != cmd::SplitDir::Auto) return no("split");
        // Right when the pane is at least as wide as tall (Macterm's automatic split).
        if (ui::TerminalWidget* tw = mv_->terminal_widget(mv_->focused_pane())) {
            const ui::Rect r = tw->frame();
            return r.w >= r.h ? done(model().split_pane(*focused_id(), ws::Direction::Right))
                              : done(model().split_pane(*focused_id(), ws::Direction::Down));
        }
        return no("split");
    }
    // ---- selection, find, copy mode
    cmd::Status copy() override {
        PaneSession* ps = focused_pane();
        if (!ps || !ps->has_selection()) return no("copy: nothing is selected");
        const std::string t = ps->selection_text();
        clipboard_cache_ = t;
        (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(t.begin(), t.end()));
        return {};
    }
    cmd::Status select_all() override {
        PaneSession* ps = focused_pane();
        if (!ps) return no("select all");
        ps->select_all();
        return {};
    }
    cmd::Status open_find() override {
        PaneSession* ps = focused_pane();
        const auto id = focused_id();
        if (!ps || !id || !uiw_) return no("find");
        if (ui::Widget* w = find_bar_ ? uiw_->overlay(find_bar_) : nullptr) {
            auto* bar = dynamic_cast<ui::FindBar*>(w);
            if (bar) uiw_->set_focus(&bar->field(), true);
            return {};
        }
        ui::Rect r{uiw_->size().w - 8, 44, 0, 0};
        if (ui::TerminalWidget* tw = mv_->terminal_widget(mv_->focused_pane())) {
            const ui::Rect b = tw->bounds_in_window();
            r = ui::Rect{b.right() - 10, b.y + 8, 0, 0};
        }
        const ui::PaneKey key = WorkspaceAdapter::key(*id);
        ui::FindBarHooks h;
        h.query = [this, key](const std::string& q) -> std::pair<size_t, std::optional<size_t>> {
            auto it = panes_.find(key);
            if (it == panes_.end()) return {0, std::nullopt};
            const size_t n = it->second->find(q);
            return {n, it->second->find_current()};
        };
        h.step = [this, key](int d) -> std::pair<size_t, std::optional<size_t>> {
            auto it = panes_.find(key);
            if (it == panes_.end()) return {0, std::nullopt};
            it->second->find_step(d);
            return {it->second->find_count(), it->second->find_current()};
        };
        h.closed = [this, key] {
            if (auto it = panes_.find(key); it != panes_.end()) it->second->end_find();
            loop_post([this] {
                if (find_bar_ && uiw_) uiw_->close_overlay(find_bar_);
                find_bar_ = 0;
                if (mv_) mv_->focus_active_pane();
            });
        };
        find_bar_ = ui::show_find_bar(*uiw_, std::move(h), {r.x, r.y}, last_query_for(ps));
        return {};
    }
    cmd::Status find_step(int delta) override {
        PaneSession* ps = focused_pane();
        if (!ps || ps->find_count() == 0) return no("find: no search");
        ps->find_step(-delta);   // "next" goes up toward older output, as the find bar's Enter
        return {};
    }
    cmd::Status toggle_copy_mode() override {
        PaneSession* ps = focused_pane();
        if (!ps) return no("copy mode");
        ps->toggle_copy_mode();
        if (ps->in_copy_mode()) toast(std::string(ui::tr("Copy mode")), std::string(ui::tr("hjkl move, v select, y copy, q leave")));
        return {};
    }
    cmd::Status copy_session_id() override {
        PaneSession* ps = focused_pane();
        if (!ps || ps->session_id().empty()) return no("copy session id: a local pane has no session");
        const std::string& s = ps->session_id();
        clipboard_cache_ = s;
        (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(s.begin(), s.end()));
        toast(std::string(ui::tr("Copied session ID")), s.substr(0, 8));
        return {};
    }
    std::string last_query_for(PaneSession*) const { return {}; }
    void loop_post(std::function<void()> fn) { p_.loop().post(std::move(fn)); }

    cmd::Status paste() override {
        p_.clipboard().read_text(platform::Selection::clipboard, [this, alive = std::weak_ptr<bool>(alive_)](Result<std::u8string> r) {
            if (auto a = alive.lock(); !a || !*a || !uiw_) return;
            const bool field = dynamic_cast<ui::TextField*>(uiw_->focused()) != nullptr;
            if (r && !r->empty()) {
                if (field) {
                    uiw_->on_text(as_chars(*r));
                } else if (PaneSession* ps = focused_pane()) {
                    ps->paste(*r);
                }
                return;
            }
            if (!field) paste_image();
        });
        return {};
    }

    // No text in the clipboard: an image is saved as a private PNG and its path is pasted
    // (agents such as Claude Code attach an image path typed or pasted into their prompt).
    void paste_image() {
        PaneSession* ps = focused_pane();
        if (!ps || ps->read_only()) return;
        p_.clipboard().read_image([this, alive = std::weak_ptr<bool>(alive_)](Result<std::vector<uint8_t>> img) {
            if (auto a = alive.lock(); !a || !*a) return;
            if (!img || img->empty()) return;
            PaneSession* target = focused_pane();
            if (!target || target->read_only()) return;
            auto dir = platform::paths::get(platform::paths::Kind::cache, cfg_.profile());
            if (!dir) return;
            auto saved = app::save_clip_image(*dir / "clipboard", *img);
            if (!saved) {
                toast(std::string(ui::tr("The image could not be saved")), saved.error().what);
                return;
            }
            const std::string text = app::quote_paths({platform::paths::to_utf8(*saved)}, kWindows);
            target->paste(std::u8string(text.begin(), text.end()));
        });
    }
    cmd::Status clear_screen_and_scrollback() override {
        PaneSession* ps = focused_pane();
        if (!ps) return no("clear");
        ps->clear_scrollback();
        return {};
    }
    cmd::Status change_font_size(int delta) override {
        set_font_size(delta == 0 ? default_pt_ : font_pt_ + static_cast<float>(delta));
        return {};
    }
    cmd::Status scroll(cmd::ScrollTo to) override {
        PaneSession* ps = focused_pane();
        if (!ps) return no("scroll");
        switch (to) {
            case cmd::ScrollTo::PageUp: ps->scroll_page(1); break;
            case cmd::ScrollTo::PageDown: ps->scroll_page(-1); break;
            case cmd::ScrollTo::Top: ps->scroll_to_top(); break;
            case cmd::ScrollTo::Bottom: ps->scroll_to_bottom(); break;
        }
        return {};
    }
    cmd::Status jump_to_prompt(int delta) override {
        PaneSession* ps = focused_pane();
        if (!ps) return no("prompt marks");
        if (!ps->jump_to_prompt(delta))
            return make_error(Errc::not_supported, "no prompt marks in this pane (needs shell integration, OSC 133)");
        return {};
    }
    cmd::Status send_text(std::string_view text) override {
        PaneSession* ps = focused_pane();
        if (!ps) return no("send text");
        ps->send(text);
        return {};
    }
    cmd::Status open_project_dialog() override {
        new_project_dialog();
        return {};
    }

    // ------------------------------------------------------------ workspace save, presets, layouts

    std::string config_subdir(const char* name) const {
        auto d = platform::paths::get(platform::paths::Kind::config, cfg_.profile());
        return d ? platform::paths::to_utf8(*d / name) : std::string();
    }
    ws::IFileSystem& preset_fs() const {
        if (!preset_fs_) preset_fs_ = ws::make_os_file_system();
        return *preset_fs_;
    }
    ws::PresetStore preset_store() const { return ws::PresetStore(preset_fs(), config_subdir("workspaces")); }
    // Project files: keepterm's <config>/projects, and on macOS Macterm's folder
    // read only.
    ws::ProjectFileStore project_files() const {
        std::vector<std::string> ro;
#if defined(__APPLE__)
        if (cfg_.general_read_macterm_projects()) ro.push_back(ws::path_join(home_dir(), ".config/macterm/projects"));
#endif
        return ws::ProjectFileStore(preset_fs(), config_subdir("projects"), std::move(ro), home_dir());
    }
    std::optional<ws::ProjectFileStore::Found> find_project_file(ws::ProjectId id) const {
        const ws::Project* p = model().project(id);
        if (!p) return std::nullopt;
        return project_files().find(p->path, p->name);
    }
    // The menu asks often: the answer is kept for a second per project.
    bool has_project_file(ws::ProjectId id) const {
        const auto now = std::chrono::steady_clock::now();
        if (has_file_cache_.first != id || now - has_file_at_ > std::chrono::seconds(1)) {
            has_file_cache_ = {id, find_project_file(id).has_value()};
            has_file_at_ = now;
        }
        return has_file_cache_.second;
    }
    void forget_project_file_cache() const { has_file_at_ = {}; }
    // A pane that took over another's session knows what the old one knew until the monitor's
    // next sample (a Save Layout right after an Apply keeps the commands). The old panes' facts
    // are taken before the apply: their removal drops them.
    std::map<ws::PaneId, PaneStats> facts_of_kept(const ws::LayoutPlan& plan) const {
        std::map<ws::PaneId, PaneStats> out;
        for (const auto& t : plan.tabs)
            for (const auto& k : t.keep)
                if (k)
                    if (auto it = facts_.find(WorkspaceAdapter::key(*k)); it != facts_.end()) out.emplace(*k, it->second);
        return out;
    }
    void carry_facts(std::map<ws::PaneId, PaneStats> saved, const std::vector<std::pair<ws::PaneId, ws::PaneId>>& adopted) {
        for (const auto& [from, to] : adopted) {
            auto it = saved.find(from);
            if (it != saved.end() && model().pane(to)) facts_[WorkspaceAdapter::key(to)] = std::move(it->second);
        }
    }
    // What Apply Layout matches panes by: the monitor's last sample of each pane.
    ws::LivePaneFactsFn live_facts_fn() const {
        return [this](const ws::Pane& p) {
            ws::LivePaneFacts f;
            f.cwd = p.last_cwd;
            auto it = facts_.find(WorkspaceAdapter::key(p.id));
            if (it == facts_.end()) return f;
            if (!it->second.cwd.empty()) f.cwd = it->second.cwd;
            if (!it->second.command.empty()) f.command = it->second.command;
            f.shell = it->second.shell;
            return f;
        };
    }

    std::optional<ws::ProjectId> shown_real_project() const {
        const ws::WindowState* w = model().window(window_id_);
        if (!w || ws::is_fixed_workspace(w->shown) || !model().project(w->shown)) return std::nullopt;
        return w->shown;
    }

    // The host's live folder of a pane (what the header shows), for presets and layouts.
    std::function<std::string(ws::PaneId)> live_cwd_fn() const {
        return [this](ws::PaneId id) {
            auto it = facts_.find(WorkspaceAdapter::key(id));
            return it == facts_.end() ? std::string() : it->second.cwd;
        };
    }

    // ------------------------------------------------------------ the pane menu and agent sessions

    const ws::Pane* pane_of_key(ui::PaneKey k) const {
        auto id = adapter_ ? adapter_->pane_id(k) : std::nullopt;
        return id ? model().pane(*id) : nullptr;
    }

    // SET_OPTION to the pane's host on a worker thread (a connect can take a moment), then a
    // fresh look at the pane's facts.
    void send_pane_option(ui::PaneKey k, std::string key, std::string value, std::string done_text) {
        const ws::Pane* p = pane_of_key(k);
        if (!p || p->endpoint.empty()) {
            toast(std::string(ui::tr("The pane has no live session")));
            return;
        }
#ifndef KT_HAVE_SESSIONS
        (void)key;
        (void)value;
        (void)done_text;
#else
        run_worker([this, endpoint = p->endpoint, key = std::move(key), value = std::move(value), done_text = std::move(done_text)] {
            auto r = session::send_option(endpoint, key, value, 2000);
            post_alive([this, ok = static_cast<bool>(r), err = r ? std::string() : r.error().what, done_text] {
                if (!ok) toast(std::string(ui::tr("The session did not answer")), err);
                else if (!done_text.empty()) toast(done_text);
                if (monitor_) monitor_->poke();
            });
        });
#endif
    }

    // A worker for one short blocking request; finished ones are reaped on the next call.
    void run_worker(std::function<void()> fn) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers_.emplace_back([fn = std::move(fn), done] {
            fn();
            done->store(true);
        });
        finished_.push_back(done);
        for (size_t i = 0; i < workers_.size() && i < finished_.size();) {
            if (finished_[i]->load()) {
                workers_[i].join();
                workers_.erase(workers_.begin() + static_cast<std::ptrdiff_t>(i));
                finished_.erase(finished_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
    // Runs `fn` on the UI thread unless the window is gone by then.
    void post_alive(std::function<void()> fn) {
        p_.loop().post([fn = std::move(fn), alive = std::weak_ptr<bool>(alive_)] {
            if (auto a = alive.lock(); a && *a) fn();
        });
    }

    std::vector<ui::MenuItem> pane_menu_items(ui::PaneKey k, bool full) {
        std::vector<ui::MenuItem> items;
        const ui::PaneItem* pi = mv_ ? mv_->view().snapshot().pane(k) : nullptr;
        if (!pi) return items;
        auto cmd_item = [this, k](const char* label, const char* id) {
            ui::MenuItem m;
            m.label = std::string(ui::tr(label));
            m.shortcut = caps(id);
            m.action = [this, k, cid = std::string(id)] {
                if (auto pid = adapter_->pane_id(k)) (void)model().focus_pane(*pid);
                mv_->sync();
                invoke(cid, cmd::InvokeSource::Menu);
            };
            return m;
        };
        if (full) {
            items.push_back(cmd_item("Copy", "terminal.copy"));
            items.push_back(cmd_item("Paste", "terminal.paste"));
            items.push_back(cmd_item("Select All", "terminal.select_all"));
            items.push_back(cmd_item("Find\xE2\x80\xA6", "terminal.find"));
            items.push_back(ui::MenuItem::sep());
            items.push_back(cmd_item("Split Right", "pane.split_right"));
            items.push_back(cmd_item("Split Down", "pane.split_down"));
            items.push_back(cmd_item("Zoom Pane", "pane.zoom"));
            ui::MenuItem ro = cmd_item("Read-Only Mode", "pane.read_only");
            ro.id = "read_only";
            ro.checked = pi->read_only;
            items.push_back(ro);
            items.push_back(ui::MenuItem::sep());
            ui::MenuItem pr;
            pr.label = std::string(ui::tr("Processes\xE2\x80\xA6"));
            pr.action = [this, k] {
                ui::Rect at{};
                if (ui::TerminalWidget* tw = mv_->terminal_widget(k)) at = tw->bounds_in_window();
                show_resources(ui::ResourceScope{ui::ResourceScope::Kind::pane, k}, ui::Rect{at.x + 8, at.y + 8, 1, 1}, false);
            };
            items.push_back(pr);
            ui::MenuItem cp;
            cp.label = std::string(ui::tr("Copy Path"));
            cp.enabled = !pi->cwd.empty();
            cp.action = [this, path = pi->cwd] { (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(path.begin(), path.end())); };
            items.push_back(cp);
            ui::MenuItem of;
#if defined(__APPLE__)
            of.label = std::string(ui::tr("Show in Finder"));
#elif defined(_WIN32)
            of.label = std::string(ui::tr("Show in Explorer"));
#else
            of.label = std::string(ui::tr("Open in File Manager"));
#endif
            of.enabled = !pi->cwd.empty();
            of.action = [this, path = pi->cwd] {
                if (!desktop::open_folder(path)) toast(std::string(ui::tr("The folder could not be opened")), path);
            };
            items.push_back(of);
            items.push_back(ui::MenuItem::sep());
        }
        // The agent session: what the host would resume after a respawn.
        const bool saved = !pi->saved_agent.empty();
        if (saved || !pi->agent.empty()) {
            ui::MenuItem rn;
            rn.label = std::string(ui::tr("Resume Agent Now"));
            rn.id = "resume_agent";
            rn.enabled = saved && pi->at_prompt;
            rn.action = [this, k] { send_pane_option(k, "agent.resume_now", "1", std::string(ui::tr("Resuming the agent session"))); };
            items.push_back(rn);
            ui::MenuItem fg;
            fg.label = std::string(ui::tr("Forget Agent Session"));
            fg.id = "forget_agent";
            fg.enabled = saved;
            fg.action = [this, k] { send_pane_option(k, "agent.forget", "1", std::string(ui::tr("Agent session forgotten"))); };
            items.push_back(fg);
            ui::MenuItem rr;
            rr.label = std::string(ui::tr("Resume Agent on Restore"));
            rr.id = "agent_restore";
            rr.checked = pi->agent_restore;
            rr.action = [this, k, on = pi->agent_restore] {
                send_pane_option(k, "agent.restore", on ? "0" : "1",
                                 std::string(on ? ui::tr("The agent stays closed after a restore") : ui::tr("The agent comes back after a restore")));
            };
            items.push_back(rr);
            // A new shell in the same folder; the agent is not resumed (binder rule 5).
            ui::MenuItem rs = cmd_item("Restart Without Resume", "pane.restart_session");
            rs.id = "restart_no_resume";
            items.push_back(rs);
        }
        if (full) {
            if (saved || !pi->agent.empty()) items.push_back(ui::MenuItem::sep());
            else {
                items.push_back(cmd_item("Restart Session", "pane.restart_session"));
            }
            items.push_back(cmd_item("Close Pane", "pane.close"));
        }
        return items;
    }

    // ------------------------------------------------------------ the control channel

#ifdef KT_HAVE_SESSIONS
    void start_control() {
        auto ep = control::endpoint();
        if (!ep) return;
        control::Server::Info info{KT_VERSION, {"status",        "pane.list",          "pane.run",       "pane.dump",      "pane.inspect", "pane.split",
                                          "pane.focus",    "pane.close",         "pane.key",       "pane.zoom",      "pane.resize_split",
                                          "pane.equalize", "pane.private", "pane.read_only",       "pane.restart",   "project.list",   "project.create",
                                          "project.select", "project.rename",    "project.color",  "project.remove", "tab.list",
                                          "tab.new",       "tab.select",         "tab.move",       "tab.move_to_project", "tab.rename",
                                          "tab.close",     "window.list",        "window.focus",   "window.new",     "window.close",   "quick_terminal.toggle", "quick_terminal.show", "quick_terminal.hide", "quick_terminal.status", "session.kill",   "agent.list",
                                          "agent.forget",  "layout.save",        "layout.apply",   "app.exit",       "app.restart",
                                          "command.run"}};
        auto s = control::Server::start(*ep, std::move(info), [core = &core_](const session::json::Value& req) {
            // On the UI thread, by the key window; the connection waits (and gives up when the
            // windows go).
            auto done = std::make_shared<std::promise<session::json::Value>>();
            auto answer = done->get_future();
            core->p.loop().post([core, req, done] {
                MainWindow* w = core->route();
                if (!w) {
                    done->set_value(control::error_response(req.str_or("id"), control::kNotRunning, "the window is closing"));
                    return;
                }
                done->set_value(w->handle_control(req));
            });
            for (int waited = 0; waited < 100; ++waited) {
                if (answer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) return answer.get();
                if (core->control_stopping) break;
            }
            return control::error_response(req.str_or("id"), control::kTimeout, "the window did not answer");
        });
        if (!s) {
            // Another keepterm window of this user and profile owns the endpoint.
            log::warn("gui", "control endpoint: " + s.error().what);
            return;
        }
        control_ = std::move(*s);
    }

    // The pane a request names: its pane id (KEEPTERM_PANE), its session id, or a unique
    // prefix of either.
    std::optional<ws::PaneId> control_pane(std::string_view want) const {
        if (want.empty()) return std::nullopt;
        std::optional<ws::PaneId> hit;
        int hits = 0;
        for (const auto& [id, pane] : model().state().panes) {
            const std::string pid = id.to_string(), sid = pane.session.str();
            if (pid == want || sid == want) return id;
            if (pid.starts_with(want) || sid.starts_with(want)) {
                hit = id;
                ++hits;
            }
        }
        return hits == 1 ? hit : std::nullopt;
    }

    session::json::Value handle_control(const session::json::Value& req) {
        namespace json = session::json;
        const std::string id = req.str_or("id");
        const std::string cmd_name = req.str_or("command");
        const json::Value* argp = req.get("args");
        const json::Value args = argp ? *argp : json::Value::object();
        log::info("gui", "control: " + cmd_name);   // never the text or the dump
        const ui::WorkspaceSnapshot& snap = adapter_->snapshot();
        if (cmd_name == "status") {
            json::Value d = json::Value::object();
            d.set("app_version", json::Value::string(KT_VERSION));
            d.set("pid", json::Value::integer(session::os::current_pid()));
            std::size_t tabs = 0;
            for (const ui::ProjectItem& p : snap.projects) tabs += p.tabs.size();
            d.set("projects", json::Value::integer(static_cast<int64_t>(snap.projects.size())));
            d.set("tabs", json::Value::integer(static_cast<int64_t>(tabs)));
            d.set("panes", json::Value::integer(static_cast<int64_t>(model().state().panes.size())));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "pane.list") {
            json::Value list = json::Value::array();
            const ui::PaneKey focused = mv_ ? mv_->focused_pane() : 0;
            for (const ui::ProjectItem& pr : snap.projects) {
                for (const ui::TabItem& t : pr.tabs) {
                    for (ui::PaneKey k : t.panes) {
                        const ws::Pane* mp = pane_of_key(k);
                        const ui::PaneItem* pi = snap.pane(k);
                        if (!mp || !pi) continue;
                        json::Value p = json::Value::object();
                        p.set("id", json::Value::string(mp->id.to_string()));
                        p.set("session", json::Value::string(mp->session.str()));
                        p.set("project", json::Value::string(pr.scratch ? std::string("Scratch") : pr.name));
                        p.set("tab", json::Value::string(t.title));
                        std::string title = !pi->title.empty() ? pi->title : t.title;
                        if (title.empty()) {   // the sidebar's fallback: the folder's name
                            const auto cut = pi->cwd.find_last_of("/\\");
                            title = cut == std::string::npos ? pi->cwd : pi->cwd.substr(cut + 1);
                        }
                        p.set("title", json::Value::string(title));
                        p.set("cwd", json::Value::string(pi->cwd));
                        p.set("program", json::Value::string(!pi->agent.empty() ? pi->agent : !pi->process.empty() ? pi->process : "shell"));
                        p.set("focused", json::Value::boolean(k == focused));
                        p.set("read_only", json::Value::boolean(mp->read_only));
                        list.push(std::move(p));
                    }
                }
            }
            json::Value d = json::Value::object();
            d.set("panes", std::move(list));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "pane.run" || cmd_name == "pane.dump") {
            const std::string want = args.str_or("pane");
            if (want.empty()) return control::error_response(id, control::kTargetRequired, "name the pane (--pane ID)");
            auto pid = control_pane(want);
            if (!pid) return control::error_response(id, control::kNotFound, "no pane " + want);
            auto it = panes_.find(WorkspaceAdapter::key(*pid));
            if (it == panes_.end() || !it->second) return control::error_response(id, control::kNotFound, "the pane has no terminal yet");
            PaneSession& ps = *it->second;
            if (cmd_name == "pane.run") {
                if (model().pane(*pid) && model().pane(*pid)->read_only)
                    return control::error_response(id, "read_only", "the pane is read-only (keepterm pane read-only --off)");
                std::string text = args.str_or("text");
                for (char& c : text)
                    if (c == '\n') c = '\r';
                if (args.bool_or("submit", true)) text += '\r';
                ps.send(text);
                json::Value d = json::Value::object();
                d.set("pane", json::Value::string(pid->to_string()));
                return control::ok_response(id, std::move(d));
            }
            const bool scrollback = args.bool_or("scrollback", false);
            const auto lines = static_cast<std::size_t>(std::max<int64_t>(0, args.int_or("lines", 0)));
            json::Value d = json::Value::object();
            d.set("pane", json::Value::string(pid->to_string()));
            d.set("text", json::Value::string(ps.dump_text(scrollback, lines != 0 ? lines : (scrollback ? 10000 : 0))));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "app.exit" || cmd_name == "app.restart") {
            const bool restart = cmd_name == "app.restart";
            if (!rt_) return control::error_response(id, control::kNotRunning, "this window has no sessions (--local)");
            if (!exiting_ && !args.bool_or("force", false)) {
                json::Value busy = json::Value::array();
                std::string names;
                for (const LostProgram& l : lost_programs()) {
                    json::Value b = json::Value::object();
                    b.set("id", json::Value::string(l.pane.to_string()));
                    b.set("program", json::Value::string(l.program));
                    b.set("tab", json::Value::string(l.tab));
                    busy.push(std::move(b));
                    if (!names.empty()) names += ", ";
                    names += l.program;
                }
                if (!names.empty()) {
                    json::Value r = control::error_response(id, control::kBusy, "running programs would be lost: " + names +
                                                                                    " (--force ends them)");
                    json::Value d = json::Value::object();
                    d.set("panes", std::move(busy));
                    r.set("data", std::move(d));
                    return r;
                }
            }
            json::Value d = json::Value::object();
            d.set("pid", json::Value::integer(session::os::current_pid()));
            d.set("restart", json::Value::boolean(restart));
            if (!exiting_) {
                // After the answer has left: the request's connection closes with the window.
                p_.loop().add_timer(Clock::now() + std::chrono::milliseconds(100), [this, alive = std::weak_ptr<bool>(alive_), restart] {
                    if (auto a = alive.lock(); a && *a) begin_exit(restart);
                });
                exiting_ = true;
            }
            return control::ok_response(id, std::move(d));
        }
        if (auto r = handle_control_ws(id, cmd_name, args)) return *r;
        return control::error_response(id, control::kUnknownCommand, "unknown command " + cmd_name);
    }

    // A program that ending the sessions would lose: anything but an idle shell or an agent whose
    // session resumes.
    struct LostProgram {
        ws::PaneId pane;
        std::string program, project, tab, folder;
    };
    std::vector<LostProgram> lost_programs() const {
        std::vector<LostProgram> out;
        if (!adapter_) return out;
        const ui::WorkspaceSnapshot& snap = adapter_->snapshot();
        for (const ui::ProjectItem& pr : snap.projects) {
            for (const ui::TabItem& t : pr.tabs) {
                for (ui::PaneKey k : t.panes) {
                    const ui::PaneItem* pi = snap.pane(k);
                    const ws::Pane* mp = pane_of_key(k);
                    if (!pi || !mp) continue;
                    const bool agent_ok = !pi->agent.empty() && pi->agent_restore && !pi->saved_agent_session.empty();
                    if (agent_ok || (pi->agent.empty() && pi->process.empty())) continue;
                    out.push_back({mp->id, !pi->agent.empty() ? pi->agent : pi->process, pr.name, t.title, pi->cwd});
                }
            }
        }
        return out;
    }

    // Quit and End All Sessions: the running programs that would be lost in a table sorted by a
    // click on a column title (Macterm's quit table); nothing to lose ends at once.
    void quit_ending_sessions() {
        if (!rt_) {
            on_close_requested();
            return;
        }
        auto rows = std::make_shared<std::vector<LostProgram>>(lost_programs());
        if (rows->empty()) {
            begin_exit(false);
            return;
        }
        struct Sort {
            int col = 0;
            bool desc = false;
        };
        auto sort = std::make_shared<Sort>();
        static constexpr std::array<const char*, 4> kCols{"Program", "Project", "Tab", "Folder"};
        static constexpr std::array<float, 4> kWidth{100, 110, 130, 150};
        auto table = ui::column("quit_table", 2);
        ui::Box* traw = table.get();
        auto fill = std::make_shared<std::function<void()>>();
        *fill = [this, traw, rows, sort, weak_fill = std::weak_ptr<std::function<void()>>(fill)] {
            auto self = weak_fill.lock();
            if (!self) return;
            traw->clear_children();
            auto field = [](const LostProgram& l, int c) -> const std::string& {
                return c == 0 ? l.program : c == 1 ? l.project : c == 2 ? l.tab : l.folder;
            };
            std::stable_sort(rows->begin(), rows->end(), [&](const LostProgram& a, const LostProgram& b) {
                const std::string x = lower(field(a, sort->col)), y = lower(field(b, sort->col));
                return sort->desc ? y < x : x < y;
            });
            auto head = ui::row("header", 8);
            for (int c = 0; c < 4; ++c) {
                std::string label(ui::tr(kCols[static_cast<std::size_t>(c)]));
                if (c == sort->col) label += sort->desc ? " \xE2\x96\xBE" : " \xE2\x96\xB4";
                auto b = std::make_unique<ui::Button>(std::move(label), [this, c, sort, weak = std::weak_ptr<std::function<void()>>(self)] {
                    sort->desc = sort->col == c ? !sort->desc : false;
                    sort->col = c;
                    p_.loop().post([weak] {
                        if (auto f = weak.lock()) (*f)();
                    });
                }, ui::Button::Kind::plain, std::string("sort_") + kCols[static_cast<std::size_t>(c)]);
                b->style().width = kWidth[static_cast<std::size_t>(c)];
                b->set_align_start(true);
                head->add(std::move(b));
            }
            traw->add(std::move(head));
            for (const LostProgram& l : *rows) {
                auto r = ui::row("program", 8);
                for (int c = 0; c < 4; ++c) {
                    std::string text = c == 3 ? ui::folder_label(l.folder) : field(l, c);
                    auto lab = std::make_unique<ui::Label>(std::move(text), ui::TextStyle::small,
                                                           c == 0 ? ui::Tone::normal : ui::Tone::muted);
                    lab->style().width = kWidth[static_cast<std::size_t>(c)];
                    lab->set_elide(c == 3 ? ui::Label::Elide::middle : ui::Label::Elide::end);
                    r->add(std::move(lab));
                }
                traw->add(std::move(r));
            }
            if (uiw_) uiw_->request_frame();
        };
        (*fill)();
        const std::size_t n = rows->size();
        ask(std::string(ui::tr("End all sessions and quit?")),
            std::to_string(n) + " " + std::string(n == 1 ? ui::tr("program runs and ends with its session.") : ui::tr("programs run and end with their sessions.")) +
                " " + std::string(ui::tr("Idle shells and agents whose sessions resume are not listed.")),
            {{std::string(ui::tr("End All and Quit")), [this, fill] { begin_exit(false); }, ui::Button::Kind::danger}}, std::move(table), 560);
    }

    // ------------------------------------------------------------ control: projects, tabs, panes, windows

    using CJ = session::json::Value;

    static CJ cs(std::string s) { return CJ::string(std::move(s)); }
    static std::string lower(std::string_view s) {
        std::string o(s);
        for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return o;
    }
    // A 1-based place ("2"), or 0.
    static std::size_t place_of(std::string_view s) {
        if (s.empty() || s.size() > 6) return 0;
        std::size_t n = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return 0;
            n = n * 10 + static_cast<std::size_t>(c - '0');
        }
        return n;
    }

    CJ ws_fail(const std::string& id, const ws::WsError& e) const {
        std::string_view code = "bad_request";
        switch (e.code) {
            case ws::WsErrc::NotFound: code = control::kNotFound; break;
            case ws::WsErrc::Busy: code = control::kBusy; break;
            case ws::WsErrc::LayoutParse: code = "layout_parse"; break;
            case ws::WsErrc::LayoutNoTabs: code = "layout_no_tabs"; break;
            case ws::WsErrc::LayoutNoFile: code = "layout_no_file"; break;
            case ws::WsErrc::StateReadOnly: code = "read_only"; break;
            case ws::WsErrc::Io: code = "io"; break;
            default: break;
        }
        return control::error_response(id, code, e.message);
    }

    // `busy` for the panes that run a program, unless forced: the answer names the programs.
    std::optional<CJ> busy_check(const std::string& id, std::span<const ws::PaneId> panes, bool force) const {
        if (force) return std::nullopt;
        std::string names;
        CJ list = CJ::array();
        for (ws::PaneId p : panes) {
            const ws::Pane* mp = model().pane(p);
            if (!mp) continue;
            const std::string prog = running_program(*mp);
            if (prog.empty()) continue;
            CJ b = CJ::object();
            b.set("id", cs(p.to_string()));
            b.set("program", cs(prog));
            list.push(std::move(b));
            if (!names.empty()) names += ", ";
            names += prog;
        }
        if (names.empty()) return std::nullopt;
        CJ r = control::error_response(id, control::kBusy, "running programs would end: " + names + " (--force ends them)");
        CJ d = CJ::object();
        d.set("panes", std::move(list));
        r.set("data", std::move(d));
        return r;
    }

    struct CtlPick {
        std::string_view code;   // empty: found
        std::string message;
    };

    // "scratch", a project id or its unique prefix, a name (any case), or the 1-based place in
    // the sidebar. Empty: the project this window shows.
    std::optional<ws::ProjectId> ctl_project(std::string_view want, CtlPick& why) const {
        if (want.empty()) {
            if (const ws::WindowState* w = model().window(window_id_)) return w->shown;
            why = {control::kNotFound, "no window"};
            return std::nullopt;
        }
        const std::string lw = lower(want);
        if (lw == "scratch") return ws::kScratchProject;
        const auto projects = model().projects();
        std::vector<ws::ProjectId> hits;
        for (const ws::Project& p : projects)
            if (p.id.to_string() == want) return p.id;
        for (const ws::Project& p : projects)
            if (lower(p.name) == lw) hits.push_back(p.id);
        if (hits.empty())
            if (const std::size_t n = place_of(want); n >= 1 && n <= projects.size()) return projects[n - 1].id;
        if (hits.empty() && want.size() >= 4)
            for (const ws::Project& p : projects)
                if (p.id.to_string().starts_with(lw)) hits.push_back(p.id);
        if (hits.size() == 1) return hits.front();
        why = hits.empty() ? CtlPick{control::kNotFound, "no project " + std::string(want)}
                           : CtlPick{"ambiguous", "more than one project matches " + std::string(want)};
        return std::nullopt;
    }

    const ui::TabItem* tab_item(ws::TabId t) const {
        const ui::WorkspaceSnapshot& snap = adapter_->snapshot();
        const uint64_t k = WorkspaceAdapter::key(t);
        for (const ui::ProjectItem& pr : snap.projects)
            for (const ui::TabItem& ti : pr.tabs)
                if (ti.id == k) return &ti;
        return nullptr;
    }
    std::string tab_title(const ws::Tab& t) const {
        if (t.title_override) return *t.title_override;
        const ui::TabItem* ti = tab_item(t.id);
        return ti ? ti->title : std::string();
    }

    // A tab id or its unique prefix (any project), or inside `project`: the 1-based place or a
    // title. Empty: the tab the window shows when it shows `project`, else the project's active one.
    std::optional<ws::TabId> ctl_tab(std::string_view want, ws::ProjectId project, CtlPick& why) const {
        const ws::ProjectWorkspace* w = model().workspace_of(project);
        if (want.empty()) {
            const ws::WindowState* win = model().window(window_id_);
            if (win && win->shown == project)
                if (const ws::Tab* t = model().shown_tab(window_id_)) return t->id;
            if (w && w->active_tab) return *w->active_tab;
            if (w && !w->tabs.empty()) return w->tabs.front().id;
            why = {control::kNotFound, "the project has no tabs"};
            return std::nullopt;
        }
        const std::string lw = lower(want);
        std::vector<ws::TabId> hits;
        for (const ws::ProjectWorkspace& pw : model().state().workspaces)
            for (const ws::Tab& t : pw.tabs)
                if (t.id.to_string() == want) return t.id;
        for (const ws::Tab& t : model().quick_workspace().tabs)
            if (t.id.to_string() == want) return t.id;
        if (w) {
            if (const std::size_t n = place_of(want); n >= 1 && n <= w->tabs.size()) return w->tabs[n - 1].id;
            for (const ws::Tab& t : w->tabs)
                if (lower(tab_title(t)) == lw) hits.push_back(t.id);
        }
        if (hits.empty() && want.size() >= 4)
            for (const ws::ProjectWorkspace& pw : model().state().workspaces)
                for (const ws::Tab& t : pw.tabs)
                    if (t.id.to_string().starts_with(lw)) hits.push_back(t.id);
        if (hits.size() == 1) return hits.front();
        why = hits.empty() ? CtlPick{control::kNotFound, "no tab " + std::string(want)}
                           : CtlPick{"ambiguous", "more than one tab matches " + std::string(want)};
        return std::nullopt;
    }

    // --pane (KEEPTERM_PANE), else the focused pane of the window.
    std::optional<ws::PaneId> ctl_target_pane(const CJ& args, CtlPick& why) const {
        const std::string want = args.str_or("pane");
        if (want.empty()) {
            if (auto f = focused_id()) return f;
            why = {control::kTargetRequired, "name the pane (--pane ID)"};
            return std::nullopt;
        }
        if (auto p = control_pane(want)) return p;
        why = {control::kNotFound, "no pane " + want};
        return std::nullopt;
    }

    static std::optional<ws::Direction> direction_of(std::string_view s) {
        if (s == "right") return ws::Direction::Right;
        if (s == "left") return ws::Direction::Left;
        if (s == "down") return ws::Direction::Down;
        if (s == "up") return ws::Direction::Up;
        return std::nullopt;
    }

    // Brings a tab to the window: shows its project, selects it.
    void ctl_show_tab(ws::TabId t) {
        const auto loc = model().locate_tab(t);
        const ws::WindowState* w = model().window(window_id_);
        if (!loc || !w || loc->project == ws::kQuickProject) return;
        ws::Transaction tx = model().begin();
        if (w->shown != loc->project) (void)model().show_project(window_id_, loc->project);
        (void)model().select_tab(window_id_, t);
    }

    CJ project_json(ws::ProjectId pid) const {
        CJ p = CJ::object();
        const ws::Project* pr = model().project(pid);
        const ws::ProjectWorkspace* w = model().workspace_of(pid);
        const ws::WindowState* win = model().window(window_id_);
        p.set("id", cs(pid == ws::kScratchProject ? std::string("scratch") : pid.to_string()));
        p.set("name", cs(model().display_name(pid)));
        p.set("path", cs(pr ? pr->path : std::string()));
        p.set("color", cs(std::string(ws::to_string(pr ? pr->color : ws::ColorTag::None))));
        std::size_t panes = 0;
        if (w)
            for (const ws::Tab& t : w->tabs) panes += t.tree.panes().size();
        p.set("tabs", CJ::integer(static_cast<int64_t>(w ? w->tabs.size() : 0)));
        p.set("panes", CJ::integer(static_cast<int64_t>(panes)));
        p.set("shown", CJ::boolean(win && win->shown == pid));
        p.set("scratch", CJ::boolean(pid == ws::kScratchProject));
        return p;
    }

    CJ tab_json(const ws::Tab& t, std::size_t place, ws::ProjectId pid) const {
        CJ j = CJ::object();
        const ws::Tab* shown = model().shown_tab(window_id_);
        j.set("id", cs(t.id.to_string()));
        j.set("index", CJ::integer(static_cast<int64_t>(place)));
        j.set("project", cs(model().display_name(pid)));
        j.set("title", cs(tab_title(t)));
        j.set("renamed", CJ::boolean(t.title_override.has_value()));
        j.set("panes", CJ::integer(static_cast<int64_t>(t.tree.panes().size())));
        j.set("pinned", CJ::boolean(t.pinned));
        j.set("zoomed", CJ::boolean(t.zoomed.has_value()));
        j.set("selected", CJ::boolean(shown && shown->id == t.id));
        return j;
    }

    std::optional<CJ> handle_control_ws(const std::string& id, const std::string& cmd_name, const CJ& args) {
        CtlPick why;
        auto fail = [&] { return control::error_response(id, why.code, why.message); };
        auto ok = [&](CJ d) {
            after_command();
            return control::ok_response(id, std::move(d));
        };
        const bool force = args.bool_or("force", false);

        // ---------------------------------------------------------------- projects
        if (cmd_name == "project.list") {
            CJ list = CJ::array();
            for (const ws::Project& p : model().projects()) list.push(project_json(p.id));
            list.push(project_json(ws::kScratchProject));
            CJ d = CJ::object();
            d.set("projects", std::move(list));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "project.create") {
            std::string path = args.str_or("path");
            while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
            if (path.starts_with("~")) path = home_dir() + path.substr(1);
            std::error_code ec;
            if (path.empty() || !std::filesystem::is_directory(platform::paths::from_utf8(path), ec))
                return control::error_response(id, control::kNotFound, "not a folder: " + path);
            std::string name = args.str_or("name");
            if (name.empty()) name = ws::folder_name(path);
            auto r = model().add_project(std::move(name), path);
            if (!r) return ws_fail(id, r.error());
            if (args.bool_or("select", false)) {
                (void)model().show_project(window_id_, *r);
                if (const ws::ProjectWorkspace* w = model().workspace_of(*r); w && w->tabs.empty()) {
                    ws::NewTabOptions o;
                    o.window = window_id_;
                    (void)model().new_tab(*r, {}, o);
                }
            }
            return ok(project_json(*r));
        }
        if (cmd_name == "project.select" || cmd_name == "project.rename" || cmd_name == "project.color" ||
            cmd_name == "project.remove") {
            const std::string want = args.str_or("project");
            if (want.empty()) return control::error_response(id, control::kTargetRequired, "name the project");
            auto pid = ctl_project(want, why);
            if (!pid) return fail();
            if (cmd_name == "project.select") {
                if (auto r = model().show_project(window_id_, *pid); !r) return ws_fail(id, r.error());
                if (const ws::ProjectWorkspace* w = model().workspace_of(*pid); w && w->tabs.empty()) {
                    ws::NewTabOptions o;
                    o.window = window_id_;
                    (void)model().new_tab(*pid, {}, o);
                }
                return ok(project_json(*pid));
            }
            if (cmd_name == "project.rename") {
                if (auto r = model().rename_project(*pid, args.str_or("name")); !r) return ws_fail(id, r.error());
                return ok(project_json(*pid));
            }
            if (cmd_name == "project.color") {
                const auto c = ws::color_from_string(lower(args.str_or("color")));
                if (!c) return control::error_response(id, control::kUsage, "colors: none red orange yellow green teal blue purple pink gray");
                if (auto r = model().set_project_color(*pid, *c); !r) return ws_fail(id, r.error());
                return ok(project_json(*pid));
            }
            std::vector<ws::PaneId> panes;
            if (const ws::ProjectWorkspace* w = model().workspace_of(*pid))
                for (const ws::Tab& t : w->tabs)
                    for (ws::PaneId p : t.tree.panes()) panes.push_back(p);
            if (auto b = busy_check(id, panes, force)) return b;
            CJ d = project_json(*pid);
            if (auto r = model().remove_project(*pid, true); !r) return ws_fail(id, r.error());
            return ok(std::move(d));
        }

        // ---------------------------------------------------------------- tabs
        if (cmd_name.starts_with("tab.")) {
            auto pid = ctl_project(args.str_or("project"), why);
            if (!pid) return fail();
            if (cmd_name == "tab.list") {
                CJ list = CJ::array();
                auto add = [&](ws::ProjectId p) {
                    if (const ws::ProjectWorkspace* w = model().workspace_of(p))
                        for (std::size_t i = 0; i < w->tabs.size(); ++i) list.push(tab_json(w->tabs[i], i + 1, p));
                };
                if (args.bool_or("all", false)) {
                    for (const ws::Project& p : model().projects()) add(p.id);
                    add(ws::kScratchProject);
                } else {
                    add(*pid);
                }
                CJ d = CJ::object();
                d.set("tabs", std::move(list));
                return control::ok_response(id, std::move(d));
            }
            if (cmd_name == "tab.new") {
                ws::NewPaneSpec spec;
                if (auto c = args.str_or("cwd"); !c.empty()) spec.cwd = c.starts_with("~") ? home_dir() + c.substr(1) : c;
                if (auto sh = args.str_or("shell"); !sh.empty()) spec.shell = ws::ShellSpec{sh, {}};
                if (auto run = args.str_or("run"); !run.empty()) spec.run = run;
                ws::NewTabOptions o;
                o.focus = !args.bool_or("no_focus", false);
                if (o.focus) {
                    o.window = window_id_;
                    if (const ws::WindowState* w = model().window(window_id_); w && w->shown != *pid)
                        (void)model().show_project(window_id_, *pid);
                }
                auto t = model().new_tab(*pid, std::move(spec), o);
                if (!t) return ws_fail(id, t.error());
                const ws::Tab* tt = model().tab(*t);
                CJ d = tt ? tab_json(*tt, 0, *pid) : CJ::object();
                if (tt) d.set("pane", cs(tt->focused.to_string()));
                return ok(std::move(d));
            }
            const std::string want = args.str_or("tab");
            if (want.empty() && cmd_name != "tab.rename" && cmd_name != "tab.close")
                return control::error_response(id, control::kTargetRequired, "name the tab");
            auto tid = ctl_tab(want, *pid, why);
            if (!tid) return fail();
            const ws::Tab* tab = model().tab(*tid);
            const auto loc = model().locate_tab(*tid);
            if (!tab || !loc) return control::error_response(id, control::kNotFound, "no tab " + want);
            auto place = [&] {
                const ws::ProjectWorkspace* w = model().workspace_of(loc->project);
                if (w)
                    for (std::size_t i = 0; i < w->tabs.size(); ++i)
                        if (w->tabs[i].id == *tid) return i + 1;
                return std::size_t{0};
            };
            auto tab_now = [&] {
                const ws::Tab* t = model().tab(*tid);
                const auto l = model().locate_tab(*tid);
                return t && l ? tab_json(*t, place(), l->project) : CJ::object();
            };
            if (cmd_name == "tab.select") {
                ctl_show_tab(*tid);
                return ok(tab_now());
            }
            if (cmd_name == "tab.move") {
                const std::size_t to = static_cast<std::size_t>(std::max<int64_t>(1, args.int_or("slot", 1)));
                const ws::ProjectWorkspace* w = model().workspace_of(loc->project);
                const std::size_t n = w ? w->tabs.size() : 1;
                if (auto r = model().move_tab(*tid, std::min(to, n) - 1); !r) return ws_fail(id, r.error());
                return ok(tab_now());
            }
            if (cmd_name == "tab.move_to_project") {
                CtlPick why2;
                auto to = ctl_project(args.str_or("to"), why2);
                if (args.str_or("to").empty()) return control::error_response(id, control::kTargetRequired, "name the project to move to");
                if (!to) return control::error_response(id, why2.code, why2.message);
                std::optional<std::size_t> at;
                if (args.int_or("slot", 0) > 0) at = static_cast<std::size_t>(args.int_or("slot", 1) - 1);
                if (auto r = model().move_tab_to_project(*tid, *to, at); !r) return ws_fail(id, r.error());
                return ok(tab_now());
            }
            if (cmd_name == "tab.rename") {
                std::optional<std::string> title;
                if (!args.bool_or("reset", false)) {
                    title = args.str_or("title");
                    if (title->empty()) return control::error_response(id, control::kUsage, "give a title, or --reset");
                }
                if (auto r = model().rename_tab(*tid, std::move(title)); !r) return ws_fail(id, r.error());
                return ok(tab_now());
            }
            if (cmd_name == "tab.close") {
                const std::vector<ws::PaneId> panes = tab->tree.panes();
                if (auto b = busy_check(id, panes, force)) return b;
                CJ d = tab_now();
                if (auto r = model().close_tab(*tid, true); !r) return ws_fail(id, r.error());
                return ok(std::move(d));
            }
            return std::nullopt;
        }

        // ---------------------------------------------------------------- app commands
        if (cmd_name == "command.run") {
            const std::string cid = args.str_or("id");
            const cmd::Command* c = cmd::builtin_registry().find(cid);
            if (!c) return control::error_response(id, control::kNotFound, "unknown command " + cid);
            std::vector<std::string> words;
            if (const CJ* w = args.get("args"))
                if (const auto* list = w->as_array())
                    for (const CJ& v : *list)
                        if (const std::string* s = v.as_string()) words.push_back(*s);
            auto ca = cmd::parse_run_args(*c, words);
            if (!ca) return control::error_response(id, control::kUsage, ca.error().what);
            if (c->destructive && !force) return control::error_response(id, control::kUsage, cid + " is destructive; add --force");
            if (!args.str_or("pane").empty()) {   // the command acts on the focused pane: focus it first
                auto pid = ctl_target_pane(args, why);
                if (!pid) return fail();
                const auto loc = model().locate(*pid);
                if (!loc) return control::error_response(id, control::kNotFound, "no pane " + args.str_or("pane"));
                ctl_show_tab(loc->tab);
                if (auto r = model().focus_pane(*pid); !r) return ws_fail(id, r.error());
                mv_->sync();
            }
            cmd::CommandContext ctx{*target_, &cfg_, key_context(false), cmd::InvokeSource::Cli, cmd::current_os(), force};
            if (const cmd::Availability av = cmd::builtin_registry().availability(*c, ctx); !av.enabled)
                return control::error_response(id, control::kUnavailable,
                                               cid + " is not available here" + (av.reason.empty() ? "" : ": " + av.reason));
            if (auto st = cmd::builtin_registry().invoke(c->id, ctx, *ca); !st)
                return control::error_response(id, control::kInternal, st.error().what);
            after_command();
            CJ d = CJ::object();
            d.set("command", cs(cid));
            if (auto f = focused_id()) d.set("pane", cs(f->to_string()));
            return ok(std::move(d));
        }

        // ---------------------------------------------------------------- panes
        if (cmd_name == "pane.inspect" || cmd_name == "pane.split" || cmd_name == "pane.focus" || cmd_name == "pane.close" ||
            cmd_name == "pane.key" || cmd_name == "pane.zoom" || cmd_name == "pane.resize_split" || cmd_name == "pane.equalize" ||
            cmd_name == "pane.private" || cmd_name == "pane.read_only" || cmd_name == "pane.restart") {
            auto pid = ctl_target_pane(args, why);
            if (!pid) return fail();
            const ws::Pane* mp = model().pane(*pid);
            const auto loc = model().locate(*pid);
            if (!mp || !loc) return control::error_response(id, control::kNotFound, "no pane " + args.str_or("pane"));
            const ui::PaneKey key = WorkspaceAdapter::key(*pid);
            CJ d = CJ::object();
            d.set("pane", cs(pid->to_string()));
            if (cmd_name == "pane.inspect") {
                const ui::PaneItem* pi = adapter_->snapshot().pane(key);
                const ws::Tab* tab = model().tab(loc->tab);
                d.set("session", cs(mp->session.str()));
                d.set("project", cs(model().display_name(loc->project)));
                d.set("tab", cs(loc->tab.to_string()));
                d.set("tab_title", cs(tab ? tab_title(*tab) : std::string()));
                d.set("focused", CJ::boolean(tab && tab->focused == *pid));
                d.set("zoomed", CJ::boolean(tab && tab->zoomed == *pid));
                d.set("cwd", cs(pi && !pi->cwd.empty() ? pi->cwd : mp->last_cwd));
                d.set("cwd_source", cs(std::string(ws::to_string(mp->last_cwd_source))));
                d.set("title", cs(pi ? pi->title : std::string()));
                d.set("program", cs(running_program(*mp)));
                d.set("foreground_pid", CJ::integer(pi ? pi->pid : 0));
                d.set("processes", CJ::integer(pi ? pi->processes : 0));
                d.set("private", CJ::boolean(mp->is_private));
                d.set("read_only", CJ::boolean(mp->read_only));
                d.set("attention", cs(std::string(ws::to_string(mp->attention))));
                d.set("bind", cs(std::string(ws::to_string(mp->bind))));
                if (!mp->bind_error.empty()) d.set("bind_error", cs(mp->bind_error));
                d.set("restore", cs(std::string(ws::to_string(mp->restore))));
                if (pi) {
                    d.set("agent", cs(pi->agent));
                    d.set("saved_agent", cs(pi->saved_agent));
                    d.set("agent_session", cs(pi->saved_agent_session));
                    d.set("agent_resume", CJ::boolean(pi->agent_restore));
                }
                if (auto it = panes_.find(key); it != panes_.end() && it->second) {
                    const PaneSession::GridInfo g = it->second->grid_info();
                    d.set("cols", CJ::integer(g.cols));
                    d.set("rows", CJ::integer(g.rows));
                    d.set("cursor_col", CJ::integer(g.cursor_col));
                    d.set("cursor_row", CJ::integer(g.cursor_row));
                    d.set("scrollback_lines", CJ::integer(static_cast<int64_t>(g.scrollback_lines)));
                    d.set("alt_screen", CJ::boolean(g.alt_screen));
                    d.set("scroll_offset", CJ::integer(static_cast<int64_t>(g.scroll_offset)));
                    d.set("exited", CJ::boolean(it->second->exited()));
                }
                return control::ok_response(id, std::move(d));
            }
            if (cmd_name == "pane.split") {
                std::string dir = args.str_or("direction", "auto");
                std::optional<ws::Direction> dd = direction_of(dir);
                if (dir == "auto") {
                    // Like pane.split_auto: across the longer side of the pane.
                    dd = ws::Direction::Right;
                    if (auto it = panes_.find(key); it != panes_.end() && it->second) {
                        const PaneSession::GridInfo g = it->second->grid_info();
                        if (g.cols && g.rows && g.cols < g.rows * 2) dd = ws::Direction::Down;
                    }
                }
                if (!dd) return control::error_response(id, control::kUsage, "--direction right|left|down|up|auto");
                ws::NewPaneSpec spec;
                if (auto c = args.str_or("cwd"); !c.empty()) spec.cwd = c.starts_with("~") ? home_dir() + c.substr(1) : c;
                if (auto run = args.str_or("run"); !run.empty()) spec.run = run;
                auto r = model().split_pane(*pid, *dd, std::move(spec), !args.bool_or("no_focus", false));
                if (!r) return ws_fail(id, r.error());
                d.set("pane", cs(r->to_string()));
                d.set("session", cs(model().pane(*r) ? model().pane(*r)->session.str() : std::string()));
                return ok(std::move(d));
            }
            if (cmd_name == "pane.focus") {
                if (const std::string dir = args.str_or("direction"); !dir.empty()) {
                    const auto dd = direction_of(dir);
                    if (!dd) return control::error_response(id, control::kUsage, "--direction right|left|down|up");
                    if (auto r = model().focus_direction(*pid, *dd); !r) return ws_fail(id, r.error());
                } else {
                    ctl_show_tab(loc->tab);
                    if (auto r = model().focus_pane(*pid); !r) return ws_fail(id, r.error());
                }
                if (const ws::Tab* t = model().tab(loc->tab)) d.set("pane", cs(t->focused.to_string()));
                return ok(std::move(d));
            }
            if (cmd_name == "pane.close") {
                const ws::PaneId one[1] = {*pid};
                if (auto b = busy_check(id, one, force)) return b;
                if (auto r = model().close_pane(*pid, true); !r) return ws_fail(id, r.error());
                return ok(std::move(d));
            }
            if (cmd_name == "pane.key") {
                if (mp->read_only) return control::error_response(id, "read_only", "the pane is read-only (keepterm pane read-only --off)");
                auto it = panes_.find(key);
                if (it == panes_.end() || !it->second) return control::error_response(id, "not_live", "the pane has no terminal yet");
                view::KeyInput k;
                std::string bad;
                if (!parse_key_chord(args.str_or("key"), k, bad)) return control::error_response(id, control::kUsage, bad);
                const int64_t repeat = std::clamp<int64_t>(args.int_or("repeat", 1), 1, 100);
                for (int64_t i = 0; i < repeat; ++i)
                    if (!it->second->send_key(k)) return control::error_response(id, control::kUsage, "the key sends nothing here");
                return control::ok_response(id, std::move(d));
            }
            if (cmd_name == "pane.zoom") {
                const ws::Tab* t = model().tab(loc->tab);
                const bool on = t && t->zoomed == *pid;
                const bool want = args.get("on") ? args.bool_or("on", true) : !on;
                if (want != on) {
                    if (want && t && t->tree.panes().size() < 2)
                        return control::error_response(id, "bad_request", "the tab has one pane");
                    if (auto r = model().toggle_zoom(*pid); !r) return ws_fail(id, r.error());
                }
                d.set("zoomed", CJ::boolean(want));
                return ok(std::move(d));
            }
            if (cmd_name == "pane.resize_split") {
                const std::string axis = args.str_or("axis", "horizontal");
                if (axis != "horizontal" && axis != "vertical")
                    return control::error_response(id, control::kUsage, "--axis horizontal|vertical");
                const double ratio = args.get("ratio") ? args.get("ratio")->as_number().value_or(-1) : 0.5;
                if (ratio < 0.15 || ratio > 0.85) return control::error_response(id, control::kUsage, "--ratio is 0.15..0.85");
                if (auto r = model().set_split_ratio(*pid, axis == "horizontal" ? ws::Axis::Horizontal : ws::Axis::Vertical, ratio); !r)
                    return ws_fail(id, r.error());
                return ok(std::move(d));
            }
            if (cmd_name == "pane.equalize") {
                if (auto r = model().equalize(loc->tab); !r) return ws_fail(id, r.error());
                return ok(std::move(d));
            }
            if (cmd_name == "pane.private") {
                const bool want = args.get("on") ? args.bool_or("on", true) : !mp->is_private;
                if (auto r = model().set_pane_private(*pid, want); !r) return ws_fail(id, r.error());
                d.set("private", CJ::boolean(want));
                return ok(std::move(d));
            }
            if (cmd_name == "pane.read_only") {
                const bool want = args.get("on") ? args.bool_or("on", true) : !mp->read_only;
                if (auto r = model().set_pane_read_only(*pid, want); !r) return ws_fail(id, r.error());
                d.set("read_only", CJ::boolean(want));
                return ok(std::move(d));
            }
            if (cmd_name == "pane.restart") {
                if (!rt_) return control::error_response(id, control::kNotRunning, "this window has no sessions (--local)");
                const ws::PaneId one[1] = {*pid};
                if (auto b = busy_check(id, one, force)) return b;
                rt_->binder().restart(*pid);
                return ok(std::move(d));
            }
        }

        // ---------------------------------------------------------------- windows
        if (cmd_name == "window.list") {
            CJ list = CJ::array();
            for (const ws::WindowState& w : model().windows()) {
                if (w.quick) continue;   // quick-terminal status shows it
                CJ j = CJ::object();
                j.set("id", cs(w.id.to_string()));
                j.set("project", cs(model().display_name(w.shown)));
                const ws::Tab* t = model().shown_tab(w.id);
                j.set("tab", cs(t ? tab_title(*t) : std::string()));
                j.set("this", CJ::boolean(w.id == window_id_));
                bool open = false;
                for (const MainWindow* mw : core_.windows) open |= mw->window_id_ == w.id;
                j.set("open", CJ::boolean(open));
                j.set("fullscreen", CJ::boolean(w.fullscreen));
                list.push(std::move(j));
            }
            CJ d = CJ::object();
            d.set("windows", std::move(list));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "quick_terminal.toggle" || cmd_name == "quick_terminal.show" || cmd_name == "quick_terminal.hide") {
            if (!rt_) return control::error_response(id, control::kUnavailable, "the quick terminal needs sessions (--local)");
            if (cmd_name == "quick_terminal.toggle") core_.toggle_quick();
            else if (cmd_name == "quick_terminal.show") {
                if (!core_.quick_shown && !core_.show_quick())
                    return control::error_response(id, control::kUnavailable, "the quick terminal could not be opened");
            } else if (core_.quick_shown) {
                core_.hide_quick();
            }
        }
        if (cmd_name.starts_with("quick_terminal.")) {
            CJ d = CJ::object();
            d.set("shown", CJ::boolean(core_.quick_shown));
            d.set("focused", CJ::boolean(core_.quick && core_.quick->win_ && core_.quick->win_->focused()));
            d.set("hotkey", cs(core_.hotkey ? core_.hotkey_chord : std::string()));
            d.set("hotkey_error", cs(core_.hotkey_error));
            if (core_.quick) {
                d.set("window", cs(core_.quick->window_id_.to_string()));
                if (core_.quick->win_) {
                    const platform::DipRect r = core_.quick->win_->placement().rect;
                    CJ f = CJ::object();
                    f.set("x", CJ::number(r.x));
                    f.set("y", CJ::number(r.y));
                    f.set("w", CJ::number(r.w));
                    f.set("h", CJ::number(r.h));
                    d.set("frame", std::move(f));
                }
                const ws::ProjectWorkspace& q = model().quick_workspace();
                d.set("tabs", CJ::number(static_cast<double>(q.tabs.size())));
                if (const ws::Tab* t = model().shown_tab(core_.quick->window_id_)) d.set("pane", cs(t->focused.to_string()));
            }
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "window.new") {
            if (const std::string pr = args.str_or("project"); !pr.empty()) {
                auto pid = ctl_project(pr, why);
                if (!pid) return fail();
                if (auto r = model().show_project(window_id_, *pid); !r) return ws_fail(id, r.error());
            }
            const std::size_t before = core_.windows.size();
            if (auto st = new_window(); !st) return control::error_response(id, control::kUnavailable, st.error().what);
            CJ d = CJ::object();
            if (core_.windows.size() > before) d.set("id", cs(core_.windows.back()->window_id_.to_string()));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "window.close") {
            MainWindow* target = this;
            if (const std::string want = args.str_or("window"); !want.empty()) {
                target = nullptr;
                for (MainWindow* mw : core_.windows)
                    if (mw->window_id_.to_string().starts_with(want)) target = mw;
                if (!target) return control::error_response(id, control::kNotFound, "no open window " + want);
            }
            CJ d = CJ::object();
            d.set("id", cs(target->window_id_.to_string()));
            // After the answer has left: the last window takes the control channel with it.
            p_.loop().post([core = &core_, target] {
                for (MainWindow* mw : core->windows)
                    if (mw == target) {
                        mw->on_close_requested();
                        return;
                    }
            });
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "window.focus") {
            MainWindow* target = this;
            if (const std::string want = args.str_or("window"); !want.empty()) {
                target = nullptr;
                for (MainWindow* mw : core_.windows)
                    if (mw->window_id_.to_string() == want || mw->window_id_.to_string().starts_with(want)) target = mw;
                if (!target) return control::error_response(id, control::kNotFound, "no open window " + want);
            }
            if (target->win_) target->win_->show(true);
            (void)model().set_key_window(target->window_id_);
            CJ d = CJ::object();
            d.set("id", cs(target->window_id_.to_string()));
            return control::ok_response(id, std::move(d));
        }

        // ---------------------------------------------------------------- sessions, agents, layouts
        if (cmd_name == "session.kill") {
            // A session a pane shows closes with its pane; the CLI ends the others itself.
            const std::string sid = args.str_or("session");
            for (const auto& [p, pane] : model().state().panes) {
                if (pane.session.str() != sid) continue;
                const ws::PaneId one[1] = {p};
                if (auto b = busy_check(id, one, force)) return b;
                CJ d = CJ::object();
                d.set("pane", cs(p.to_string()));
                if (auto r = model().close_pane(p, true); !r) return ws_fail(id, r.error());
                return ok(std::move(d));
            }
            return control::error_response(id, control::kNotFound, "no pane shows session " + sid);
        }
        if (cmd_name == "agent.list") {
            CJ list = CJ::array();
            const ui::WorkspaceSnapshot& snap = adapter_->snapshot();
            for (const auto& [p, pane] : model().state().panes) {
                const ui::PaneItem* pi = snap.pane(WorkspaceAdapter::key(p));
                if (!pi || (pi->agent.empty() && pi->saved_agent.empty())) continue;
                CJ j = CJ::object();
                j.set("pane", cs(p.to_string()));
                j.set("session", cs(pane.session.str()));
                j.set("running", cs(pi->agent));
                j.set("agent", cs(!pi->saved_agent.empty() ? pi->saved_agent : pi->agent));
                j.set("agent_session", cs(pi->saved_agent_session));
                j.set("resume", CJ::boolean(pi->agent_restore));
                j.set("pending", CJ::boolean(pane.agent_resume_pending));
                j.set("cwd", cs(pi->cwd));
                list.push(std::move(j));
            }
            CJ d = CJ::object();
            d.set("agents", std::move(list));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "agent.forget") {
            std::vector<ws::PaneId> targets;
            if (args.bool_or("all", false)) {
                for (const auto& [p, pane] : model().state().panes) targets.push_back(p);
            } else {
                auto pid = ctl_target_pane(args, why);
                if (!pid) return fail();
                targets.push_back(*pid);
            }
            CJ list = CJ::array();
            for (ws::PaneId p : targets) {
                const ui::PaneItem* pi = adapter_->snapshot().pane(WorkspaceAdapter::key(p));
                if (!pi || pi->saved_agent_session.empty()) continue;
                send_pane_option(WorkspaceAdapter::key(p), "agent.forget", "1", {});
                list.push(cs(p.to_string()));
            }
            CJ d = CJ::object();
            d.set("panes", std::move(list));
            return control::ok_response(id, std::move(d));
        }
        if (cmd_name == "layout.save" || cmd_name == "layout.apply") {
            auto pid = ctl_project(args.str_or("project"), why);
            if (!pid) return fail();
            const ws::Project* pr = model().project(*pid);
            if (ws::is_fixed_workspace(*pid) || !pr)
                return control::error_response(id, "bad_request", "layouts belong to projects, not to Scratch");
            CJ d = CJ::object();
            d.set("project", cs(model().display_name(*pid)));
            if (cmd_name == "layout.save") {
                auto file = ws::capture_project_file(model(), *pid, home_dir(), live_facts_fn());
                if (!file) return control::error_response(id, "bad_request", "the project has nothing to save");
                auto r = project_files().save(*file, pr->name);
                if (!r) return ws_fail(id, r.error());
                forget_project_file_cache();
                d.set("file", cs(*r));
                return control::ok_response(id, std::move(d));
            }
            auto found = find_project_file(*pid);
            if (!found) return ws_fail(id, ws::WsError{ws::WsErrc::LayoutNoFile, "no project file declares " + pr->path, {}});
            d.set("file", cs(found->file));
            auto file = ws::parse_project_file(found->text);
            if (!file) return ws_fail(id, file.error());
            if (!file->tabs || file->tabs->empty())
                return ws_fail(id, ws::WsError{ws::WsErrc::LayoutNoTabs, "the project file declares no tabs", {}});
            const ws::LayoutPlan plan = ws::plan_layout(model(), *pid, *file->tabs, pr->path, home_dir(), live_facts_fn());
            const bool keep = args.bool_or("keep_sessions", false);
            CJ tabs = CJ::array();
            for (const auto& t : plan.tabs) {
                CJ o = CJ::object();
                o.set("title", cs(t.title.value_or(std::string())));
                o.set("existing", CJ::boolean(t.existing.has_value()));
                std::int64_t kept = 0;
                for (const auto& k : t.keep) kept += k.has_value();
                o.set("panes", CJ::integer(static_cast<std::int64_t>(t.keep.size())));
                o.set("kept", CJ::integer(kept));
                tabs.push(std::move(o));
            }
            d.set("tabs", std::move(tabs));
            d.set("kept_panes", CJ::integer(static_cast<std::int64_t>(plan.kept())));
            d.set("closes_panes", CJ::integer(keep ? 0 : static_cast<std::int64_t>(plan.destroy.size())));
            d.set("detaches_panes", CJ::integer(keep ? static_cast<std::int64_t>(plan.destroy.size()) : 0));
            d.set("closes_tabs", CJ::integer(static_cast<std::int64_t>(plan.close.size())));
            if (args.bool_or("dry_run", false)) return control::ok_response(id, std::move(d));
            if (!keep)
                if (auto b = busy_check(id, plan.destroy, force)) return b;
            auto saved = facts_of_kept(plan);
            auto r = ws::apply_layout_plan(model(), *pid, plan, keep);
            if (!r) return ws_fail(id, r.error());
            carry_facts(std::move(saved), r->adopted);
            return ok(std::move(d));
        }
        return std::nullopt;
    }

    // "ctrl+c", "enter", "shift+tab", "alt+x", "up", "f5", "a": a key for `pane key`.
    static bool parse_key_chord(std::string_view text, view::KeyInput& k, std::string& bad) {
        std::string s = lower(text);
        if (s.empty()) {
            bad = "name a key (ctrl+c, enter, up, f5, ...)";
            return false;
        }
        uint16_t mods = 0;
        for (;;) {
            const auto plus = s.find('+');
            if (plus == std::string::npos || plus + 1 == s.size()) break;
            const std::string m = s.substr(0, plus);
            if (m == "ctrl" || m == "control") mods |= view::kPmCtrl;
            else if (m == "alt" || m == "opt" || m == "option") mods |= view::kPmAlt;
            else if (m == "shift") mods |= view::kPmShift;
            else if (m == "super" || m == "cmd" || m == "win" || m == "meta") mods |= view::kPmSuper;
            else {
                bad = "unknown modifier " + m;
                return false;
            }
            s = s.substr(plus + 1);
        }
        static const std::pair<std::string_view, vt::Key> named[] = {
            {"enter", vt::Key::enter},       {"return", vt::Key::enter},     {"esc", vt::Key::escape},
            {"escape", vt::Key::escape},     {"tab", vt::Key::tab},          {"space", vt::Key::space},
            {"backspace", vt::Key::backspace}, {"bs", vt::Key::backspace},   {"delete", vt::Key::del},
            {"del", vt::Key::del},           {"insert", vt::Key::insert},    {"home", vt::Key::home},
            {"end", vt::Key::end},           {"pageup", vt::Key::page_up},   {"pgup", vt::Key::page_up},
            {"pagedown", vt::Key::page_down}, {"pgdn", vt::Key::page_down},  {"up", vt::Key::up},
            {"down", vt::Key::down},         {"left", vt::Key::left},        {"right", vt::Key::right},
        };
        for (const auto& [n, key] : named) {
            if (s != n) continue;
            k.key = key;
            if (key == vt::Key::space) k.logical = U' ';
            k.mods = mods;
            if (key == vt::Key::space && !(mods & (view::kPmCtrl | view::kPmAlt | view::kPmSuper))) k.text = u8" ";
            return true;
        }
        if (s.size() >= 2 && s[0] == 'f') {
            const std::size_t n = place_of(std::string_view(s).substr(1));
            if (n >= 1 && n <= 12) {
                k.key = static_cast<vt::Key>(static_cast<uint16_t>(vt::Key::f1) + n - 1);
                k.mods = mods;
                return true;
            }
        }
        if (s.size() == 1) {
            const char c = s[0];
            k.mods = mods;
            if (c >= 'a' && c <= 'z') k.key = static_cast<vt::Key>(static_cast<uint16_t>(vt::Key::a) + (c - 'a'));
            else if (c >= '1' && c <= '9') k.key = static_cast<vt::Key>(static_cast<uint16_t>(vt::Key::d1) + (c - '1'));
            else if (c == '0') k.key = vt::Key::d0;
            static const std::pair<char, vt::Key> punct[] = {
                {'-', vt::Key::minus},     {'=', vt::Key::equal},     {'[', vt::Key::left_bracket}, {']', vt::Key::right_bracket},
                {'\\', vt::Key::backslash}, {';', vt::Key::semicolon}, {'\'', vt::Key::apostrophe}, {'`', vt::Key::grave},
                {',', vt::Key::comma},     {'.', vt::Key::period},    {'/', vt::Key::slash},
            };
            for (const auto& [pc, key] : punct)
                if (c == pc) k.key = key;
            if (k.key == vt::Key::unknown) {
                bad = std::string("unknown key ") + c;
                return false;
            }
            k.logical = static_cast<char32_t>(c);
            const bool shifted = (mods & view::kPmShift) != 0;
            if (!(mods & (view::kPmCtrl | view::kPmAlt | view::kPmSuper))) {
                std::string t(1, shifted && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
                k.text.assign(t.begin(), t.end());
            }
            return true;
        }
        bad = "unknown key " + s;
        return false;
    }

    // keepterm exit / restart: the state is saved, every session host saves
    // its history and agent record and ends with its programs, then the window quits; a restart
    // first starts the next GUI, which waits for this process to end and restores everything.
    // Bounded: the hosts get a fixed budget and a fallback timer quits regardless.
    void begin_exit(bool restart) {
        if (exit_started_) return;
        exit_started_ = true;
        log::info("gui", restart ? "restart: ending the sessions" : "exit: ending the sessions");
        if (rt_) {
            rt_->binder().stop();
            rt_->flush();
        }
        auto finished = std::make_shared<std::atomic<bool>>(false);
        run_worker([this, finished, restart, alive = std::weak_ptr<bool>(alive_)] {
            if (auto paths = session::os::paths()) {
                (void)session::suspend_all_hosts(*paths);
                // A spawn that was in flight may have added a host meanwhile.
                (void)session::suspend_all_hosts(*paths, {}, std::chrono::seconds(3));
            }
            p_.loop().post([this, finished, restart, alive] {
                if (auto a = alive.lock(); a && *a && !finished->exchange(true)) finish_exit(restart);
            });
        });
        p_.loop().add_timer(Clock::now() + std::chrono::seconds(20), [this, finished, restart, alive = std::weak_ptr<bool>(alive_)] {
            if (auto a = alive.lock(); a && *a && !finished->exchange(true)) {
                log::warn("gui", "the sessions did not end in time: quitting anyway");
                finish_exit(restart);
            }
        });
    }

    void finish_exit(bool restart) {
        if (restart) {
            std::vector<std::string> args{"--gui", "--after-pid",
                                          std::to_string(session::os::current_pid()) + ":" +
                                              std::to_string(session::os::process_start_time(session::os::current_pid()))};
            for (std::string& f : opts_.gui.config_flags()) args.push_back(std::move(f));
            // The same window options: the next GUI starts as this one did.
            if (opts_.gui.verbose) args.emplace_back("--verbose");
            if (opts_.gui.allow_software_adapter) args.emplace_back("--allow-software-adapter");
            if (!opts_.gui.renderer.empty()) {
                args.emplace_back("--renderer");
                args.push_back(opts_.gui.renderer);
            }
            if (opts_.gui.font_pt) {
                args.emplace_back("--font-size");
                args.push_back(std::to_string(*opts_.gui.font_pt));
            }
            if (auto r = control::launch_gui(args); !r) log::error("gui", "restart: " + r.error().what);
        }
        if (rt_) rt_->flush();
        core_.exit_code = exit_code_;
        core_.quit_all();
    }
#endif

    // ------------------------------------------------------------ the sessions overview

    // The tab and pane key of a session a pane of this window shows.
    std::optional<std::pair<ui::TabKey, ui::PaneKey>> pane_of_session(std::string_view sid) const {
        if (!adapter_) return std::nullopt;
        const ui::WorkspaceSnapshot& snap = adapter_->snapshot();
        for (const auto& [id, pane] : model().state().panes) {
            if (pane.session.str() != sid) continue;
            const ui::PaneKey k = WorkspaceAdapter::key(id);
            for (const ui::ProjectItem& pr : snap.projects)
                for (const ui::TabItem& t : pr.tabs)
                    if (std::find(t.panes.begin(), t.panes.end(), k) != t.panes.end()) return std::pair{t.id, k};
        }
        return std::nullopt;
    }

    void reveal_pane(ui::TabKey t, ui::PaneKey k) {
        adapter_->select_tab(t);
        adapter_->focus_pane(k);
        after_command();
    }

    // The session ids every pane of the model claims (other windows' too).
    bool claimed(std::string_view sid) const {
        for (const auto& c : model().claimed_sessions())
            if (c.str() == sid) return true;
        return false;
    }

    const DetachedSession* detached_session(std::string_view sid) const {
        for (const auto& d : detached_)
            if (d.id == sid && !claimed(sid)) return &d;
        return nullptr;
    }

    // Attach: a pane of this window shows it already (focus that pane), or a new tab of the
    // shown project takes the live session over.
    cmd::Status attach_session(std::string_view sid) override {
        if (auto at = pane_of_session(sid)) {
            reveal_pane(at->first, at->second);
            return {};
        }
        const DetachedSession* d = detached_session(sid);
        auto id = session::SessionId::parse(sid);
        if (!d || !id) return make_error(Errc::invalid_argument, "no such session: " + std::string(sid));
        const ui::ProjectItem* shown = adapter_->snapshot().shown_project();
        auto pid = shown ? adapter_->project_id(shown->id) : std::nullopt;
        if (!pid) return make_error(Errc::invalid_argument, "no project to attach the session to");
        ws::NewPaneSpec spec;
        if (!d->cwd.empty()) spec.cwd = d->cwd;
        spec.adopt = *id;
        auto tab = model().new_tab(*pid, std::move(spec));
        if (!tab) return make_error(Errc::invalid_argument, tab.error().message);
        detached_.erase(std::remove_if(detached_.begin(), detached_.end(), [&](const DetachedSession& x) { return x.id == sid; }),
                        detached_.end());
        after_command();
        return {};
    }

    // pane.restart_session: a new shell in the pane's folder; the history stays, the agent is not
    // resumed. Asks first when a program runs.
    cmd::Status restart_session() override {
        const auto id = focused_id();
        const ws::Pane* p = id ? model().pane(*id) : nullptr;
        if (!p || !rt_) return no("restart session");
        auto run = [this, pid = *id] {
            if (rt_) rt_->binder().restart(pid);
        };
        const std::string prog = running_program(*p);
        if (prog.empty()) {
            run();
            return {};
        }
        ask(std::string(ui::tr("Restart the session?")), prog + std::string(ui::tr(" is still running in it and will be terminated.")),
            {{std::string(ui::tr("Restart")), run, ui::Button::Kind::danger}});
        return {};
    }

    // Kill: a pane's session closes with its pane; a detached host is hung up.
    cmd::Status kill_session(std::string_view sid) override {
        if (auto at = pane_of_session(sid)) {
            adapter_->close_pane(at->second);
            after_command();
            return {};
        }
        const DetachedSession* d = detached_session(sid);
        if (!d) return make_error(Errc::invalid_argument, "no such session: " + std::string(sid));
#ifdef KT_HAVE_SESSIONS
        run_worker([this, endpoint = d->endpoint, id = d->id] {
            session::proto::Kill k;
            auto r = session::request_kill(endpoint, k, 3000);
            post_alive([this, ok = static_cast<bool>(r), id] {
                if (!ok) toast(std::string(ui::tr("The session did not end")), id);
                detached_.erase(std::remove_if(detached_.begin(), detached_.end(), [&](const DetachedSession& x) { return x.id == id; }),
                                detached_.end());
                if (sessions_dialog_) refresh_sessions_dialog();
                if (monitor_) monitor_->poke(true);
            });
        });
#endif
        return {};
    }

    static std::string session_line(const std::string& agent, const std::string& program, const std::string& cwd) {
        std::string s = !agent.empty() ? agent : program;
        if (!cwd.empty()) s += (s.empty() ? "" : " \xC2\xB7 ") + cwd;
        return s;
    }

    void fill_palette_sessions(cmd::palette::PaletteData& d) const {
        auto list = preset_store().list();
        std::stable_partition(list.begin(), list.end(), [](const ws::PresetInfo& i) { return i.is_default; });
        for (const auto& info : list) {
            if (!info.error.empty()) continue;
            d.workspaces.push_back({info.name, preset_summary(info), info.is_default});
        }
        const ui::WorkspaceSnapshot& snap = adapter_->snapshot();
        for (const auto& [id, pane] : model().state().panes) {
            const ui::PaneItem* pi = snap.pane(WorkspaceAdapter::key(id));
            if (!pi) continue;
            cmd::palette::SessionInfo s;
            s.id = pane.session.str();
            s.title = !pi->title.empty() ? pi->title : !pi->agent.empty() ? pi->agent : !pi->process.empty() ? pi->process : "shell";
            s.program = session_line(pi->agent, pi->process, pi->cwd);
            if (pi->res.valid) s.memory_bytes = pi->res.memory;
            d.sessions.push_back(std::move(s));
        }
        for (const DetachedSession& x : detached_) {
            if (claimed(x.id)) continue;
            cmd::palette::SessionInfo s;
            s.id = x.id;
            s.title = !x.agent.empty() ? x.agent : !x.program.empty() ? x.program : "shell";
            s.program = session_line(x.agent, x.program, x.cwd);
            s.state = cmd::palette::SessionInfo::State::Orphaned;
            d.sessions.push_back(std::move(s));
        }
    }

    cmd::Status show_sessions() override {
        if (monitor_) monitor_->poke(true);
        open_sessions_dialog();
        return {};
    }

    // What the dialog shows; a refresh rebuilds it only when this changed.
    std::string sessions_signature() const {
        std::string sig;
        for (const auto& [id, pane] : model().state().panes) {
            const ui::PaneItem* pi = adapter_->snapshot().pane(WorkspaceAdapter::key(id));
            sig += pane.session.str() + static_cast<char>('0' + static_cast<int>(pane.bind));
            if (pi) sig += pi->agent + "|" + pi->saved_agent + "|" + pi->cwd + "|" + (pi->at_prompt ? "p" : "");
            sig += ";";
        }
        for (const auto& d : detached_)
            if (!claimed(d.id)) sig += d.id + d.agent + d.cwd + ";";
        return sig;
    }

    void refresh_sessions_dialog() {
        if (!sessions_dialog_ || sessions_signature() == sessions_sig_) return;
        uiw_->close_overlay(sessions_dialog_);
        sessions_dialog_ = 0;
        open_sessions_dialog();
    }

    void open_sessions_dialog() {
        if (sessions_dialog_) uiw_->close_overlay(sessions_dialog_);
        sessions_sig_ = sessions_signature();
        const ui::WorkspaceSnapshot& snap = adapter_->snapshot();
        auto body = ui::column("sessions", 10);
        auto close_then = [this](std::function<void()> fn) {
            return [this, fn = std::move(fn)] {
                if (sessions_dialog_) uiw_->close_overlay(sessions_dialog_);
                sessions_dialog_ = 0;
                p_.loop().post(fn);
            };
        };
        auto line = [](std::string text, ui::TextStyle st, ui::Tone tone) {
            auto l = std::make_unique<ui::Label>(std::move(text), st, tone);
            l->style().shrink = 1;
            l->set_elide(ui::Label::Elide::middle);
            return l;
        };
        for (const ui::ProjectItem& pr : snap.projects) {
            if (pr.tabs.empty()) continue;
            auto sec = ui::column("project", 4);
            sec->add(line(pr.scratch ? std::string(ui::tr("Scratch")) : pr.name, ui::TextStyle::body_bold, ui::Tone::normal));
            for (const ui::TabItem& t : pr.tabs) {
                for (ui::PaneKey k : t.panes) {
                    const ui::PaneItem* pi = snap.pane(k);
                    const ws::Pane* mp = pane_of_key(k);
                    if (!pi || !mp) continue;
                    auto r = ui::row("pane", 8);
                    r->style().align_items = ui::LayoutStyle::Align::Center;
                    auto texts = ui::column("text", 2);
                    texts->style().grow = 1;
                    texts->style().shrink = 1;
                    std::string head = t.title.empty() ? pi->title : t.title;
                    if (head.empty()) head = std::string(ui::tr("Shell"));
                    texts->add(line(head, ui::TextStyle::body, ui::Tone::normal));
                    std::string sub = session_line(pi->agent, pi->process, pi->cwd);
                    std::string state;
                    switch (mp->bind) {
                        case ws::BindState::Failed: state = std::string(ui::tr("not running")); break;
                        case ws::BindState::Probing:
                        case ws::BindState::Spawning:
                        case ws::BindState::Unbound: state = std::string(ui::tr("starting")); break;
                        case ws::BindState::Ended: state = std::string(ui::tr("ended")); break;
                        default: break;
                    }
                    if (pi->agent.empty() && !pi->saved_agent.empty())
                        sub += std::string(" \xC2\xB7 ") + std::string(ui::tr("saved")) + " " + pi->saved_agent +
                               (pi->agent_restore ? "" : std::string(" (") + std::string(ui::tr("no resume")) + ")");
                    if (!state.empty()) sub += " \xC2\xB7 " + state;
                    texts->add(line(sub, ui::TextStyle::small, ui::Tone::muted));
                    r->add(std::move(texts));
                    if (mp->bind == ws::BindState::Failed) {
                        r->emplace<ui::Button>(std::string(ui::tr("Restore")), close_then([this, id = mp->id] {
                                                   rt_->binder().retry(id);
                                                   after_command();
                                               }), ui::Button::Kind::primary, "restore");
                    } else if (pi->agent.empty() && !pi->saved_agent.empty() && pi->at_prompt) {
                        r->emplace<ui::Button>(std::string(ui::tr("Resume Agent")), close_then([this, k] {
                                                   send_pane_option(k, "agent.resume_now", "1", std::string(ui::tr("Resuming the agent session")));
                                               }), ui::Button::Kind::secondary, "resume");
                    }
                    r->emplace<ui::Button>(std::string(ui::tr("Show")), close_then([this, tk = t.id, k] { reveal_pane(tk, k); }),
                                           ui::Button::Kind::secondary, "show");
                    sec->add(std::move(r));
                }
            }
            body->add(std::move(sec));
        }
        std::vector<const DetachedSession*> loose;
        for (const auto& d : detached_)
            if (!claimed(d.id)) loose.push_back(&d);
        if (!loose.empty()) {
            auto sec = ui::column("detached", 4);
            sec->add(line(std::string(ui::tr("Running, not in a tab")), ui::TextStyle::body_bold, ui::Tone::normal));
            for (const DetachedSession* d : loose) {
                auto r = ui::row("session", 8);
                r->style().align_items = ui::LayoutStyle::Align::Center;
                auto texts = ui::column("text", 2);
                texts->style().grow = 1;
                texts->style().shrink = 1;
                texts->add(line(session_line(d->agent, d->program, d->cwd), ui::TextStyle::body, ui::Tone::normal));
                std::string sub = d->id;
                if (d->gui_clients > 0) sub += std::string(" \xC2\xB7 ") + std::string(ui::tr("shown in another window"));
                if (d->agent.empty() && !d->saved_agent.empty()) sub += std::string(" \xC2\xB7 ") + std::string(ui::tr("saved")) + " " + d->saved_agent;
                texts->add(line(sub, ui::TextStyle::small, ui::Tone::muted));
                r->add(std::move(texts));
                r->emplace<ui::Button>(std::string(ui::tr("Attach")), close_then([this, id = d->id] {
                                           if (auto st = attach_session(id); !st) toast(st.error().what);
                                       }), ui::Button::Kind::primary, "attach");
                r->emplace<ui::Button>(std::string(ui::tr("Kill")), [this, id = d->id] {
                                           if (auto st = kill_session(id); !st) toast(st.error().what);
                                       }, ui::Button::Kind::danger, "kill");
                sec->add(std::move(r));
            }
            body->add(std::move(sec));
        }
        ui::DialogSpec d;
        d.title = std::string(ui::tr("Sessions"));
        d.message = loose.empty() ? std::string(ui::tr("Every pane's session. Live sessions no tab shows appear here too."))
                                  : std::string();
        d.body = std::move(body);
        d.width = 560;
        d.buttons.push_back({std::string(ui::tr("Close")), [this] { sessions_dialog_ = 0; }, ui::Button::Kind::secondary, true, true});
        sessions_dialog_ = ui::show_dialog(*uiw_, std::move(d));
    }

    cmd::Status save_workspace() override {
        if (rt_) rt_->flush();
        toast(std::string(ui::tr("Workspace saved")));
        return {};
    }

    cmd::Status save_workspace_as() override {
        auto store = std::make_shared<ws::PresetStore>(preset_store());
        auto name = std::make_shared<std::string>();
        auto agents_on = std::make_shared<bool>(true);
        if (auto pr = shown_real_project()) *name = store->free_name(model().project(*pr)->name);
        else *name = store->free_name("Workspace");
        auto body = ui::column("body", 10);
        auto field = std::make_unique<ui::TextField>(std::string(ui::tr("Name")), "name");
        field->set_text(*name);
        field->select_all();
        field->on_change = [name](const std::string& v) { *name = v; };
        ui::TextField* raw = field.get();
        body->add(std::move(field));
        auto r = ui::row("agents", 8);
        r->style().align_items = ui::LayoutStyle::Align::Center;
        r->emplace<ui::Toggle>(true, [agents_on](bool on) { *agents_on = on; }, "include_agents");
        r->emplace<ui::Label>(std::string(ui::tr("Include agent sessions (resume them on open)")), ui::TextStyle::body, ui::Tone::normal);
        body->add(std::move(r));
        ui::DialogSpec d;
        d.title = std::string(ui::tr("Save Workspace As"));
        d.message = std::string(ui::tr("The projects, tabs, splits and folders, saved under a name to open later."));
        d.body = std::move(body);
        d.width = 440;
        d.buttons.push_back({std::string(ui::tr("Cancel")), {}, ui::Button::Kind::secondary, false, true});
        d.buttons.push_back({std::string(ui::tr("Save")), [this, store, name, agents_on] {
                                 auto write = [this, store, n = *name, a = *agents_on](bool overwrite) {
                                     ws::CaptureOptions o;
                                     o.include_agents = a;
                                     o.live_cwd = live_cwd_fn();
                                     const ws::Preset p = ws::capture_preset(model(), n, o);
                                     if (auto r2 = store->save(p, overwrite); !r2) toast(r2.error().message);
                                     else toast(std::string(ui::tr("Workspace saved as")), n);
                                 };
                                 if (!ws::valid_preset_name(*name)) {
                                     toast(std::string(ui::tr("Not a valid workspace name")), *name);
                                 } else if (store->exists(*name)) {
                                     ask(std::string(ui::tr("Replace the workspace \"")) + *name + "\"?", {},
                                         {{std::string(ui::tr("Replace")), [write] { write(true); }, ui::Button::Kind::danger}});
                                 } else {
                                     write(false);
                                 }
                             },
                             ui::Button::Kind::primary, true, false});
        ui::show_dialog(*uiw_, std::move(d));
        uiw_->set_focus(raw, true);
        return {};
    }

    struct AskButton {
        std::string label;
        std::function<void()> action;
        ui::Button::Kind kind = ui::Button::Kind::secondary;
    };
    // A question with Cancel and the given buttons (the last one is the default).
    void ask(std::string title, std::string message, std::vector<AskButton> buttons, std::unique_ptr<ui::Widget> body = {},
             float width = 440) {
        ui::DialogSpec d;
        d.title = std::move(title);
        d.message = std::move(message);
        d.body = std::move(body);
        d.width = width;
        d.buttons.push_back({std::string(ui::tr("Cancel")), {}, ui::Button::Kind::secondary, false, true});
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            auto act = std::move(buttons[i].action);
            d.buttons.push_back({buttons[i].label, [this, act] {
                                     if (act) act();
                                     mv_->sync();
                                     mv_->focus_active_pane();
                                 },
                                 buttons[i].kind, i + 1 == buttons.size(), false});
        }
        ui::show_dialog(*uiw_, std::move(d));
    }

    static std::string preset_summary(const ws::PresetInfo& i) {
        if (!i.error.empty()) return std::string(ui::tr("cannot be read: ")) + i.error;
        std::string s = std::to_string(i.projects) + " " + std::string(i.projects == 1 ? ui::tr("project") : ui::tr("projects")) + ", " +
                        std::to_string(i.tabs) + " " + std::string(i.tabs == 1 ? ui::tr("tab") : ui::tr("tabs"));
        if (i.agents) s += ", " + std::string(ui::tr("agent sessions"));
        if (i.is_default) s += " \xC2\xB7 " + std::string(ui::tr("default"));
        return s;
    }

    // A list of the saved workspaces; `pick` gets the chosen one (the dialog closes first).
    void preset_list_dialog(std::string title, std::string message,
                            std::function<void(const ws::PresetInfo&, ui::Rect)> pick, bool include_broken) {
        auto list = preset_store().list();
        // The default first.
        std::stable_partition(list.begin(), list.end(), [](const ws::PresetInfo& i) { return i.is_default; });
        if (!include_broken) std::erase_if(list, [](const ws::PresetInfo& i) { return !i.error.empty(); });
        if (list.empty()) {
            toast(std::string(ui::tr("No saved workspaces")), std::string(ui::tr("File > Save Workspace As\xE2\x80\xA6 saves one")));
            return;
        }
        auto id = std::make_shared<ui::WidgetId>(0);
        auto body = ui::column("presets", 4);
        ui::Button* first = nullptr;
        for (const auto& info : list) {
            auto r = ui::row("preset", 8);
            r->style().align_items = ui::LayoutStyle::Align::Center;
            auto b = std::make_unique<ui::Button>(info.name, std::function<void()>{}, ui::Button::Kind::secondary, "open");
            ui::Button* braw = b.get();
            b->set_on_click([this, id, info, pick, braw] {
                const ui::Rect at = braw->bounds_in_window();
                if (*id) uiw_->close_overlay(*id);
                p_.loop().post([info, pick, at] { pick(info, at); });
            });
            b->style().min_w = 140;
            if (!first) first = braw;
            r->add(std::move(b));
            auto l = std::make_unique<ui::Label>(preset_summary(info), ui::TextStyle::small, ui::Tone::muted);
            l->style().shrink = 1;
            r->add(std::move(l));
            body->add(std::move(r));
        }
        ui::DialogSpec d;
        d.title = std::move(title);
        d.message = std::move(message);
        d.body = std::move(body);
        d.width = 480;
        d.buttons.push_back({std::string(ui::tr("Close")), {}, ui::Button::Kind::secondary, true, true});
        *id = ui::show_dialog(*uiw_, std::move(d));
        // Enter opens the first (the default) workspace; arrows and Tab move between them.
        if (first) uiw_->set_focus(first, true);
    }

    cmd::Status open_workspace() override {
        preset_list_dialog(std::string(ui::tr("Open Workspace")), std::string(ui::tr("Choose a saved workspace.")),
                           [this](const ws::PresetInfo& info, ui::Rect) { open_preset(info.name); }, false);
        return {};
    }

    // Replace (the current sessions keep running detached, or end) or merge.
    void open_preset(const std::string& name) {
        auto p = preset_store().load(name);
        if (!p) {
            toast(p.error().message);
            return;
        }
        auto preset = std::make_shared<ws::Preset>(std::move(*p));
        auto keep = std::make_shared<bool>(true);
        auto agents_on = std::make_shared<bool>(preset->has_agents());
        auto body = ui::column("options", 8);
        auto toggle_row = [&](const char* id, std::string label, std::shared_ptr<bool> flag) {
            auto r = ui::row(id, 8);
            r->style().align_items = ui::LayoutStyle::Align::Center;
            r->emplace<ui::Toggle>(*flag, [flag](bool on) { *flag = on; }, "toggle");
            r->emplace<ui::Label>(std::move(label), ui::TextStyle::body, ui::Tone::normal);
            body->add(std::move(r));
        };
        toggle_row("keep_sessions", std::string(ui::tr("Replace: keep the current sessions running, detached")), keep);
        if (preset->has_agents()) toggle_row("agents", std::string(ui::tr("Resume the agent sessions of the workspace")), agents_on);
        auto run = [this, preset, keep, agents_on](ws::PresetLoad::Mode mode) {
            ws::PresetLoad how;
            how.mode = mode;
            how.keep_sessions = *keep;
            how.include_agents = *agents_on;
            auto r = ws::apply_preset(model(), *preset, how);
            if (!r) {
                toast(r.error().message);
                return;
            }
            std::string sub = std::to_string(r->tabs) + " " + std::string(r->tabs == 1 ? ui::tr("tab") : ui::tr("tabs"));
            if (r->detached) sub += ", " + std::to_string(r->detached) + " " + std::string(ui::tr("sessions detached"));
            if (r->closed) sub += ", " + std::to_string(r->closed) + " " + std::string(ui::tr("sessions ended"));
            toast(std::string(ui::tr("Opened workspace ")) + preset->name, sub);
        };
        ask(std::string(ui::tr("Open \"")) + name + "\"",
            std::string(ui::tr("Replace the current projects with the workspace, or add its projects next to them.")),
            {{std::string(ui::tr("Merge")), [run] { run(ws::PresetLoad::Mode::Merge); }},
             {std::string(ui::tr("Replace")), [run] { run(ws::PresetLoad::Mode::Replace); }, ui::Button::Kind::primary}},
            std::move(body));
    }

    cmd::Status open_workspace_named(std::string_view name) override {
        if (!preset_store().exists(std::string(name))) return make_error(Errc::invalid_argument, "no workspace named " + std::string(name));
        open_preset(std::string(name));
        return {};
    }

    // ------------------------------------------------------------ start clean, export, import

    static constexpr const char* kLastWorkspace = "Last Workspace";

    // A preset of a whole state (the default one when null): a throwaway model loaded with it.
    ws::Preset preset_of_state(const ws::StateSnapshot* state, std::string name) const {
        return ws::preset_from_state(state, std::move(name), ws::system_random(), ws::system_clock(), home_dir());
    }

    // Replace the workspace keeping the current sessions running, detached (the Sessions
    // overview lists them for attaching).
    bool replace_workspace(const ws::Preset& p, const std::string& done) {
        ws::PresetLoad how;
        how.mode = ws::PresetLoad::Mode::Replace;
        how.keep_sessions = true;
        how.include_agents = true;
        auto r = ws::apply_preset(model(), p, how);
        if (!r) {
            toast(r.error().message);
            return false;
        }
        std::string sub;
        if (r->detached) sub = std::to_string(r->detached) + " " + std::string(ui::tr("sessions keep running (Window > Sessions\xE2\x80\xA6)"));
        toast(done, sub);
        after_command();
        if (monitor_) monitor_->poke(true);
        return true;
    }

    // The current workspace as the "Last Workspace" preset (agents included), then a clean one,
    // or the saved workspace `name` in its place.
    bool do_start_clean(const std::string& name = {}) {
        std::optional<ws::Preset> next;
        if (!name.empty()) {
            auto p = preset_store().load(name);
            if (!p) {
                toast(std::string(ui::tr("The workspace could not be opened")), p.error().message);
                return false;
            }
            next = std::move(*p);
        }
        ws::CaptureOptions o;
        o.include_agents = true;
        o.live_cwd = live_cwd_fn();
        if (auto r = preset_store().save(ws::capture_preset(model(), kLastWorkspace, o), true); !r) {
            toast(std::string(ui::tr("The current workspace could not be kept")), r.error().message);
            return false;
        }
        if (next) return replace_workspace(*next, std::string(ui::tr("Opened workspace ")) + name);
        return replace_workspace(preset_of_state(nullptr, "clean"), std::string(ui::tr("Started clean")));
    }

    // The default saved workspace that the launch prompt offers (not "Last Workspace", which is
    // what Restore brings back anyway).
    std::optional<std::string> launch_default_workspace() const {
        auto d = preset_store().default_name();
        if (!d || *d == kLastWorkspace || !preset_store().exists(*d)) return std::nullopt;
        return d;
    }

    // Something to choose between: a project, more than one pane, or an agent session to resume.
    // A lone scratch shell restores without a question.
    bool worth_asking_at_launch() const {
        const ws::StateSnapshot& st = rt_->model().state();
        if (st.panes.size() > 1 || !st.projects.empty() || launch_default_workspace()) return true;
        for (const auto& [id, p] : st.panes)
            if (p.agent_resume) return true;
        return false;
    }

    // general.startup = "ask": the workspace is loaded but not bound until the answer. Escape
    // restores (the default).
    void ask_at_launch() {
        auto go = [this, alive = alive_](bool clean, std::string name = {}) {
            if (!*alive || !rt_ || launch_answered_) return;
            launch_answered_ = true;
            if (clean) do_start_clean(name);
            rt_->start();
            if (mv_) {
                mv_->sync();
                mv_->focus_active_pane();
            }
            sync_now();
        };
        const ws::StateSnapshot& st = rt_->model().state();
        std::size_t tabs = 0;
        for (const auto& w : st.workspaces) tabs += w.tabs.size();
        ui::DialogSpec d;
        d.title = std::string(ui::tr("Restore the last workspace?"));
        std::string what;
        if (!st.projects.empty())
            what = std::to_string(st.projects.size()) + " " + std::string(st.projects.size() == 1 ? ui::tr("project") : ui::tr("projects")) + ", ";
        what += std::to_string(tabs) + " " + std::string(tabs == 1 ? ui::tr("tab") : ui::tr("tabs")) + ". ";
        d.message = what +
                    std::string(ui::tr("A clean start keeps it as \"Last Workspace\"; its sessions keep running, detached."));
        d.width = 440;
        d.buttons.push_back({std::string(ui::tr("Start Clean")), [go] { go(true); }, ui::Button::Kind::secondary, false, false});
        if (auto def = launch_default_workspace()) {
            d.message += " " + std::string(ui::tr("Or open the default workspace \"")) + *def + "\".";
            d.buttons.push_back({std::string(ui::tr("Open ")) + "\"" + *def + "\"", [go, n = *def] { go(true, n); },
                                 ui::Button::Kind::secondary, false, false});
        }
        d.buttons.push_back({std::string(ui::tr("Restore")), [go] { go(false); }, ui::Button::Kind::primary, true, true});
        ui::show_dialog(*uiw_, std::move(d));
    }

    cmd::Status start_clean() override {
        ask(std::string(ui::tr("Start clean?")),
            std::string(ui::tr("The projects and tabs go; their sessions keep running, detached. The current workspace is kept as "
                               "\"Last Workspace\" (File > Restore Last Workspace).")),
            {{std::string(ui::tr("Start Clean")), [this] { do_start_clean(); }, ui::Button::Kind::primary}});
        return {};
    }

    cmd::Status restore_last_workspace() override {
        if (!preset_store().exists(kLastWorkspace)) return make_error(Errc::invalid_argument, "no last workspace (Start Clean keeps one)");
        open_preset(kLastWorkspace);
        return {};
    }

    // A typed path (no native file dialog yet); `done` runs with it.
    void path_dialog(std::string title, std::string message, std::string initial, std::string action, std::function<void(std::string)> done) {
        auto value = std::make_shared<std::string>(initial);
        auto field = std::make_unique<ui::TextField>(std::string(ui::tr("File")), "path");
        field->set_text(initial);
        field->select_all();
        field->on_change = [value](const std::string& v) { *value = v; };
        ui::TextField* raw = field.get();
        ui::DialogSpec d;
        d.title = std::move(title);
        d.message = std::move(message);
        d.body = std::move(field);
        d.width = 520;
        d.buttons.push_back({std::string(ui::tr("Cancel")), {}, ui::Button::Kind::secondary, false, true});
        d.buttons.push_back({std::move(action), [value, done] {
                                 std::string pth = *value;
                                 if (!pth.empty() && pth.front() == '~') pth = home_dir() + pth.substr(1);
                                 if (!pth.empty()) done(pth);
                             },
                             ui::Button::Kind::primary, true, false});
        ui::show_dialog(*uiw_, std::move(d));
        uiw_->set_focus(raw, true);
    }

    cmd::Status export_workspace() override {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string initial = "~/keepterm-workspace-" + ws::file_stamp(now) + ".json";
        path_dialog(std::string(ui::tr("Export Workspace State")),
                    std::string(ui::tr("A copy of the workspace state file: projects, tabs, splits, folders and the agent sessions to resume.")),
                    initial, std::string(ui::tr("Export")), [this, now](const std::string& path) {
                        ws::WriterInfo w;
                        w.version = KT_VERSION;
                        const std::string text = ws::serialize_state(model().state(), w, now);
                        if (preset_fs().exists(path)) {
                            ask(std::string(ui::tr("Replace the file?")), path,
                                {{std::string(ui::tr("Replace")), [this, path, text] { write_export(path, text); }, ui::Button::Kind::danger}});
                        } else {
                            write_export(path, text);
                        }
                    });
        return {};
    }

    void write_export(const std::string& path, const std::string& text) {
        if (auto r = preset_fs().write_atomic(path, text); !r) toast(std::string(ui::tr("The state could not be written")), r.error().message);
        else toast(std::string(ui::tr("Workspace state exported")), path);
    }

    cmd::Status import_workspace() override {
        path_dialog(std::string(ui::tr("Import Workspace State")),
                    std::string(ui::tr("A workspace state file (exported, or a workspace.json) takes the place of the current workspace; "
                                       "the current sessions keep running, detached.")),
                    "~/", std::string(ui::tr("Import")), [this](const std::string& path) {
                        auto rd = preset_fs().read(path);
                        if (rd.status != ws::ReadStatus::Ok) {
                            toast(std::string(ui::tr("The file could not be read")), rd.status == ws::ReadStatus::Missing ? path : rd.error);
                            return;
                        }
                        auto parsed = ws::parse_state(rd.data, ws::system_random());
                        if (!parsed) {
                            toast(std::string(ui::tr("Not a workspace state file")), parsed.error().message);
                            return;
                        }
                        const ws::Preset p = preset_of_state(&parsed->state, "import");
                        if (p.projects.empty()) {
                            toast(std::string(ui::tr("The file has no projects or tabs")), path);
                            return;
                        }
                        replace_workspace(p, std::string(ui::tr("Workspace state imported")));
                    });
        return {};
    }

    cmd::Status manage_workspaces() override {
        preset_list_dialog(std::string(ui::tr("Manage Workspaces")), std::string(ui::tr("Choose a workspace to rename, duplicate, delete or make the default.")),
                           [this](const ws::PresetInfo& info, ui::Rect) { manage_preset(info); }, true);
        return {};
    }

    void manage_preset(const ws::PresetInfo& info) {
        const std::string name = info.name;
        auto report = [this](const ws::WsResult<void>& r, std::string ok) {
            if (!r) toast(r.error().message);
            else toast(std::move(ok));
        };
        std::vector<AskButton> b;
        b.push_back({std::string(ui::tr("Delete")), [this, name, report] {
                         ask(std::string(ui::tr("Delete the workspace \"")) + name + "\"?", std::string(ui::tr("Its file is removed. Running sessions are not touched.")),
                             {{std::string(ui::tr("Delete")), [this, name, report] { report(preset_store().remove(name), std::string(ui::tr("Workspace deleted"))); },
                               ui::Button::Kind::danger}});
                     },
                     ui::Button::Kind::danger});
        b.push_back({std::string(ui::tr("Duplicate")), [this, name, report] {
                         auto st = preset_store();
                         report(st.duplicate(name, st.free_name(name + " copy")), std::string(ui::tr("Workspace duplicated")));
                     }});
        if (info.error.empty()) {
            b.push_back({std::string(info.is_default ? ui::tr("Clear Default") : ui::tr("Make Default")), [this, name, report, d = info.is_default] {
                             report(preset_store().set_default(d ? std::nullopt : std::optional<std::string>(name)), std::string(ui::tr("Default workspace changed")));
                         }});
        }
        b.push_back({std::string(ui::tr("Rename\xE2\x80\xA6")), [this, name] { rename_preset_dialog(name); }, ui::Button::Kind::primary});
        ask(name, preset_summary(info), std::move(b));
    }

    void rename_preset_dialog(const std::string& from) {
        auto value = std::make_shared<std::string>(from);
        auto field = std::make_unique<ui::TextField>(std::string(ui::tr("Name")), "name");
        field->set_text(from);
        field->select_all();
        field->on_change = [value](const std::string& v) { *value = v; };
        ui::TextField* raw = field.get();
        ask(std::string(ui::tr("Rename Workspace")), {},
            {{std::string(ui::tr("Rename")), [this, from, value] {
                  if (auto r = preset_store().rename(from, *value); !r) toast(r.error().message);
                  else toast(std::string(ui::tr("Workspace renamed")), *value);
              },
              ui::Button::Kind::primary}},
            std::move(field));
        uiw_->set_focus(raw, true);
    }

    cmd::Status save_layout() override {
        auto pr = shown_real_project();
        if (!pr) return no("save layout");
        const ws::Project* p = model().project(*pr);
        auto file = p ? ws::capture_project_file(model(), *pr, home_dir(), live_facts_fn()) : std::nullopt;
        if (!file) return no("save layout");
        auto r = project_files().save(*file, p->name);
        forget_project_file_cache();
        if (!r) toast(r.error().message);
        else toast(std::string(ui::tr("Layout saved")), ws::folder_name(*r));
        return {};
    }

    cmd::Status apply_layout() override {
        auto pr = shown_real_project();
        if (!pr) return no("apply layout");
        const ws::Project* p = model().project(*pr);
        auto found = find_project_file(*pr);
        if (!p || !found) {
            toast(std::string(ui::tr("No layout file for this project")));
            return {};
        }
        auto file = ws::parse_project_file(found->text);
        if (!file) {
            toast(std::string(ui::tr("The layout file has an error")), file.error().message);
            return {};
        }
        if (!file->tabs || file->tabs->empty()) {
            toast(std::string(ui::tr("The layout file declares no tabs")));
            return {};
        }
        const ws::ProjectId id = *pr;
        auto plan = std::make_shared<ws::LayoutPlan>(ws::plan_layout(model(), id, *file->tabs, p->path, home_dir(), live_facts_fn()));
        auto run = [this, plan, id](bool keep) {
            auto saved = facts_of_kept(*plan);
            auto r = ws::apply_layout_plan(model(), id, *plan, keep);
            if (!r) toast(r.error().message);
            else carry_facts(std::move(saved), r->adopted);
        };
        if (!plan->destructive()) {
            run(false);
            return {};
        }
        const std::string what = std::to_string(plan->destroy.size()) + " " +
                                 std::string(ui::tr(plan->destroy.size() == 1 ? "pane is not in the layout" : "panes are not in the layout")) +
                                 (plan->destroy_busy.empty() ? std::string()
                                                             : ", " + std::to_string(plan->destroy_busy.size()) + " " + std::string(ui::tr("running a program")));
        ask(std::string(ui::tr("Apply the layout file?")), what,
            {{std::string(ui::tr("Apply, End Sessions")), [run] { run(false); }, ui::Button::Kind::danger},
             {std::string(ui::tr("Apply, Keep Sessions")), [run] { run(true); }, ui::Button::Kind::primary}});
        return {};
    }
    cmd::Status toggle_sidebar() override {
        mv_->toggle_sidebar();
        return {};
    }
    cmd::Status toggle_fullscreen() override {
        fullscreen_ = !fullscreen_;
        win_->set_fullscreen(fullscreen_);
        return {};
    }
    cmd::Status close_window() override {
        on_close_requested();
        return {};
    }
    cmd::Status toggle_quick_terminal() override {
        if (!rt_) return no("the quick terminal (--local)");
        core_.toggle_quick();
        return {};
    }
    cmd::Status new_window() override {
        if (!rt_) return no("new windows (--local)");
        const ws::WindowState* w = model().window(window_id_);
        const ws::ProjectId shown = w ? w->shown : ws::kScratchProject;
        const ws::WindowId nw = model().open_window(shown);
        ws::NewTabOptions o;
        o.window = nw;
        (void)model().new_tab(shown, {}, o);
        // Cascaded from this window.
        std::optional<platform::DipRect> place;
        if (win_) {
            const platform::DipRect f = win_->placement().rect;
            place = platform::DipRect{f.x + 28, f.y + 28, f.w, f.h};
        }
        if (!core_.open(nw, place)) {
            (void)model().close_window(nw);
            return make_error(Errc::unavailable, "the window could not be opened");
        }
        return {};
    }
    cmd::Status open_settings() override {
        show_settings_sheet();
        return {};
    }

    // Settings (src/app/settings_controller.h): a window of its own (Macterm's Settings window),
    // or a modal sheet over this window when the second window cannot be created.
    void show_settings_sheet() {
        if (!uiw_) return;
        if (settings_win_) {
            settings_win_->activate();
            return;
        }
        if (settings_ && settings_->view()) return;
        SettingsEnv env;
        env.config_file = config::ConfigWriter::target_file(opts_.gui.load_options());
        env.config = [this] { return cfg_svc_ ? cfg_svc_->current() : cfg_; };
        env.written = [this] {
            if (cfg_svc_) {
                cfg_svc_->reload_now();
            } else if (settings_) {
                cfg_ = config::Config::load(opts_.gui.load_options());
                settings_->refresh(cfg_);
            }
        };
        env.themes = &library_;
        env.os_dark = os_appearance_ != platform::Appearance::light;
        env.font_families = [this] { return source_ ? text::monospace_families(*source_) : std::vector<std::string>{}; };
        env.renderer = [this] { return r_ ? std::string(render::backend_name(r_->active())) : std::string(); };
        env.storage_usage = [this]() -> Result<store::Usage> {
            const auto root = store_root();
            std::error_code ec;
            if (root.empty() || !std::filesystem::exists(root / store::kCatalogFile, ec))
                return make_error(Errc::unavailable, "No history has been recorded yet.");
            if (!settings_catalog_) {
                auto c = store::Catalog::open(root / store::kCatalogFile);
                if (!c) return make_error(Errc::unavailable, c.error().what);
                settings_catalog_ = std::move(*c);
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
            return store::usage(*settings_catalog_, root, store::policy_from_config(cfg_), static_cast<uint64_t>(now.count()));
        };
        env.pin_session = [this](const std::string& id, bool pin) -> Result<void> {
            if (!settings_catalog_) return make_error(Errc::unavailable, "The session store is not open.");
            return settings_catalog_->set_pinned(id, pin);
        };
        env.forget_session = [this](const std::string& id) -> Result<void> {
            if (!settings_catalog_) return make_error(Errc::unavailable, "The session store is not open.");
            auto r = store::forget_session(*settings_catalog_, store_root(), id);
            if (!r) return make_error(r.error().code, r.error().what);
            return {};
        };
        env.open_config_file = [this, file = env.config_file] {
            if (auto r = open_with_default_app(file); !r) toast("Cannot open the config file", r.error().what);
        };
#ifdef KT_HAVE_SESSIONS
        env.statusline_hook = [this] {
            return session::claude_statusline_hook((cfg_svc_ ? cfg_svc_->current() : cfg_).agents_claude_config_dir());
        };
#endif
        settings_ = std::make_unique<SettingsController>(std::move(env));
        if (cfg_svc_) {
            settings_sub_ = cfg_svc_->on_reload([this](const config::ReloadReport&) {
                p_.loop().post([this] {
                    if (settings_ && cfg_svc_) settings_->refresh(cfg_svc_->current());
                });
            });
        }
        if (open_settings_window()) return;
        settings_->on_close = [this] {
            // Not from inside the close button's handler: the sheet is destroyed on close.
            p_.loop().post([this] {
                if (settings_id_ && uiw_) uiw_->close_overlay(settings_id_);
            });
        };
        settings_id_ = ui::show_settings(*uiw_, settings_->make_view(), [this] {
            settings_id_ = 0;
            settings_sub_ = {};
            if (settings_) settings_->closed();
            // The controller may be on the stack (its close button): drop it on the next turn.
            p_.loop().post([this] {
                if (settings_ && !settings_->view()) settings_.reset();
            });
        });
    }
    // The Settings window: its own renderer and text stack (src/app/ui_window_host.h), the
    // theme and the interface fonts of this window.
    // The macOS menu bar takes Cmd chords before any view and runs them on this window. While
    // the Settings window is the key window, its own chords go there: Close closes it, and Copy,
    // Paste, Select All and Find act on its fields.
    bool route_menu_to_settings(const std::string& id) {
        if (!settings_win_ || !settings_win_->focused()) return false;
        if (id == "pane.close" || id == "tab.close" || id == "window.close") {
            settings_win_->close();
            return true;
        }
        if (id == "terminal.paste") {
            p_.clipboard().read_text(platform::Selection::clipboard, [this, alive = alive_](Result<std::u8string> r) {
                if (!*alive || !r || r->empty() || !settings_win_) return;
                clipboard_cache_.assign(r->begin(), r->end());
                if (dynamic_cast<ui::TextField*>(settings_win_->ui().focused())) settings_win_->ui().on_text(as_chars(*r));
            });
            return true;
        }
        platform::Key key = platform::Key::unknown;
        char32_t ch = 0;
        if (id == "terminal.copy") key = platform::Key::c, ch = U'c';
        else if (id == "terminal.select_all") key = platform::Key::a, ch = U'a';
        else if (id == "terminal.find") key = platform::Key::f, ch = U'f';
        else return false;
        platform::KeyEvent k;
        k.key = key;
        k.logical = ch;
        k.mods = platform::mod_super;
        settings_win_->ui().on_key(k);
        k.action = platform::KeyAction::release;
        settings_win_->ui().on_key(k);
        return true;
    }

    void close_settings_window() {
        if (!settings_win_) return;
        settings_win_.reset();
        settings_sub_ = {};
        if (settings_) {
            settings_->closed();
            settings_.reset();
        }
    }

    bool open_settings_window() {
        if (!source_ || !r_ || !win_) return false;
        UiWindowSpec spec;
        spec.title = u8"Settings";
        const platform::WindowPlacement pl = win_->placement();
        if (!pl.maximized && !pl.fullscreen) spec.center_over = pl.rect;
        spec.renderer = std::string(render::backend_name(r_->active()));
        spec.render_config = rcfg_;
        spec.font_source = source_.get();
        spec.ui_font = cfg_.ui_font();
        spec.ui_mono_font = cfg_.ui_mono_font();
        spec.theme = uiw_->theme();
        spec.motion = motion_of(cfg_.ui_motion());
        spec.get_clipboard = [this] { return clipboard_cache_; };
        spec.set_clipboard = [this](const std::string& s) {
            clipboard_cache_ = s;
            (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(s.begin(), s.end()));
        };
        spec.closed = [this] { close_settings_window(); };
        // Text fields paste from the cached clipboard: refresh it when the window comes forward.
        spec.focus_changed = [this, alive = alive_](bool focused) {
            if (!focused) return;
            p_.clipboard().read_text(platform::Selection::clipboard, [this, alive](Result<std::u8string> r) {
                if (*alive && r) clipboard_cache_.assign(r->begin(), r->end());
            });
        };
        auto host = UiWindowHost::create(p_, std::move(spec));
        if (!host) {
            log::warn("gui", "the Settings window could not be opened, showing it over the main window: " + host.error().what);
            return false;
        }
        settings_win_ = std::move(*host);
        auto view = settings_->make_view();
        view->set_windowed(true);
        ui::SettingsView* raw = view.get();
        settings_->on_close = [this] {
            if (settings_win_) settings_win_->close();
        };
        settings_win_->set_root(std::move(view));
        settings_win_->show();
        settings_win_->ui().set_focus(&raw->sections(), false);
        return true;
    }

    cmd::Status reload_config() override {
        if (!cfg_svc_) return no("reload config");
        cfg_svc_->reload_now();
        return {};
    }
    cmd::Status quit(bool end_sessions) override {
        if (end_sessions) quit_ending_sessions();
        else core_.quit_all();
        return {};
    }

private:
    // ------------------------------------------------------------ model access

    ws::Workspace& model() { return rt_ ? rt_->model() : *local_ws_; }
    const ws::Workspace& model() const { return rt_ ? rt_->model() : *local_ws_; }

    std::optional<ws::PaneId> focused_id() const {
        const ws::Tab* t = model().shown_tab(window_id_);
        if (!t) return std::nullopt;
        return t->focused;
    }

    PaneSession* focused_pane() const {
        const auto id = focused_id();
        if (!id) return nullptr;
        auto it = panes_.find(WorkspaceAdapter::key(*id));
        return it == panes_.end() ? nullptr : it->second.get();
    }

    template <class T>
    cmd::Status done(const ws::WsResult<T>& r) {
        if (r) return {};
        return make_error(Errc::unavailable, r.error().message);
    }

    void toast(std::string title, std::string subtitle = {}) {
        if (!uiw_) return;
        ui::ToastSpec t;
        t.title = std::move(title);
        t.subtitle = std::move(subtitle);
        ui::show_toast(*uiw_, t);
    }

    // ------------------------------------------------------------ start-up

    bool init() {
        const bool first = core_.windows.empty();
        load_config();
        auto fs = text::FontSystem::create();
        if (!fs) return fail("fonts", fs.error());
        fonts_ = std::move(*fs);
        source_ = text::create_font_source();
        if (!resolve_fonts()) return false;
        fonts_->set_font_source(source_.get());
        auto uf = ui::resolve_ui_fonts(*source_, cfg_.ui_font(), cfg_.ui_mono_font());
        if (!uf) return fail("no interface font", uf.error());
        text_ = std::make_unique<ui::FontTextService>(*fonts_, std::move(*uf));
        library_ = theme::ThemeLibrary::load(theme::user_theme_dirs(cfg_.profile()));
        os_appearance_ = platform::system_appearance();

        if (!start_workspace(first)) return false;

        // The window: the saved frame, else 1100 x 700 DIPs.
        platform::WindowDesc wd;
        wd.title = u8"keepterm";
        wd.min_size = platform::DipSize{480, 300};
        wd.placement.rect = platform::DipRect{0, 0, 1100, 700};
        if (const ws::WindowState* w = model().window(window_id_); w && w->frame.w >= 480 && w->frame.h >= 300) {
            wd.placement.rect = platform::DipRect{static_cast<float>(w->frame.x), static_cast<float>(w->frame.y),
                                                  static_cast<float>(w->frame.w), static_cast<float>(w->frame.h)};
        }
        if (place_) wd.placement.rect = *place_;
        if (quick_) {
            wd.kind = platform::WindowKind::quick_terminal;
            wd.title = u8"keepterm quick terminal";
            wd.min_size = platform::DipSize{400, 200};
            if (auto q = quick_place()) wd.placement.rect = q->second;
        }
        auto w = p_.create_window(wd, *this);
        if (!w) return fail("window", w.error());
        win_ = std::move(*w);
        if (quick_) win_->set_hit_test([this](platform::DipPoint pt) { return quick_hit(pt); });
        scale_ = win_->scale();
        env_.scale = scale_;
        if (!load_font()) return false;
        auto surf = win_->create_surface(platform::SurfaceRequest{});
        if (!surf) return fail("surface", surf.error());
        surface_view_ = surf->handle;
        px_ = win_->client_size_px();
        if (!start_renderer(*surf)) return false;
        tex_ = std::make_unique<view::AtlasTextures>(fonts_->atlas(), *r_);
        text_->set_textures(tex_.get());

        build_ui();
        ws_sub_ = model().subscribe([this](std::span<const ws::Change>) { schedule_sync(); });
        appearance_ = platform::watch_appearance(p_.loop(), [this](platform::Appearance a) {
            os_appearance_ = a;
            apply_theme();
        });
        // The launch choice (--clean, general.startup): a clean start puts the restored workspace
        // aside before any pane binds, so its hosts are neither attached nor respawned (live ones
        // stay listed under Sessions). "ask" holds the binding until the user answers.
        const std::string startup = opts_.clean ? std::string("clean") : cfg_.general_startup();
        if (first) {
            if (rt_ && startup == "clean") do_start_clean();
            if (rt_ && startup == "ask" && worth_asking_at_launch()) ask_at_launch();
            else if (rt_) rt_->start();
#ifdef KT_HAVE_SESSIONS
            if (rt_) start_control();
#endif
            if (rt_) core_.sync_hotkey(cfg_, keymap_);
        }
        if (!quick_) core_.windows.push_back(this);
        sync_now();
        limits_ = std::make_unique<LimitsRefresher>(
            [this](std::function<void()> fn) { p_.loop().post(std::move(fn)); },
            [this, alive = alive_](const std::string& agent, std::vector<agents::RateLimit> l, std::int64_t at) {
                if (*alive) on_probe_limits(agent, std::move(l), at);
            });
        if (first) {
            // One monitor for the process: every window gets its reports.
            monitor_ = std::make_unique<Monitor>([pp = &p_](std::function<void()> fn) { pp->loop().post(std::move(fn)); },
                                                 [core = &core_](MonitorReport r) {
                                                     for (MainWindow* mw : std::vector<MainWindow*>(core->windows)) mw->on_monitor(r);
                                                 },
                                                 store_root(), store::policy_from_config(cfg_));
        } else {
            monitor_->poke(false);
        }
        if (!rt_ && !opts_.program.empty()) log::info("gui", "local mode: " + opts_.program.front());
        win_->set_ime_enabled(true);
        if (!quick_) win_->show(true);   // AppCore shows the panel
        // A window shown without activation (launched from a background task, focus stolen)
        // never gets a focus-lost event: start from the platform's actual state.
        if (uiw_) uiw_->on_focus(win_->focused());
        if (first && rt_ && !rt_->load_outcome().notice.empty()) toast(rt_->load_outcome().notice);
        full_next_ = true;
        win_->request_frame();
        tick_timer();
        // The other windows of the saved workspace open again.
        if (first && rt_) {
            const ws::WindowId mine = window_id_;
            std::vector<ws::WindowId> others;
            for (const ws::WindowState& ww : model().windows())
                if (ww.id != mine && !ww.quick) others.push_back(ww.id);
            p_.loop().post([core = &core_, others, alive = std::weak_ptr<bool>(alive_)] {
                if (alive.expired()) return;
                for (const ws::WindowId& o : others) (void)core->open(o);
            });
        }
        return true;
    }

    // The panel's frame on the chosen screen: fractions of the work area, the
    // origin placed within the spare room of each axis.
    std::optional<std::pair<platform::ScreenArea, platform::DipRect>> quick_place() const {
        const std::string sp = cfg_.quick_terminal_screen();
        const auto pick = sp == "primary" ? platform::ScreenPick::primary
                          : sp == "active" ? platform::ScreenPick::active
                                           : platform::ScreenPick::mouse;
        auto a = p_.screen_area(pick);
        if (!a) return std::nullopt;
        const platform::DipRect& r = a->area;
        float w = std::max(400.0f, static_cast<float>(cfg_.quick_terminal_width()) * r.w);
        float h = std::max(200.0f, static_cast<float>(cfg_.quick_terminal_height()) * r.h);
        // Not set by the user: a fraction of a large screen would be huge, so the default is
        // capped at the size of a usual terminal window.
        if (cfg_.origin(config::KeyId::quick_terminal_width).layer == config::Layer::Default) w = std::min(w, kQuickDefaultMax.w);
        if (cfg_.origin(config::KeyId::quick_terminal_height).layer == config::Layer::Default) h = std::min(h, kQuickDefaultMax.h);
        w = std::min(r.w, w);
        h = std::min(r.h, h);
        float x = r.x + static_cast<float>(cfg_.quick_terminal_x()) * (r.w - w);
        float y = r.y + static_cast<float>(cfg_.quick_terminal_y()) * (r.h - h);
        platform::DipRect out{x, y, w, h};
        // Dynamic mode: where the user last left it on this monitor, kept at least 60 DIP on screen.
        const bool dyn_pos = cfg_.quick_terminal_position_mode() == "dynamic";
        const bool dyn_size = cfg_.quick_terminal_size_mode() == "dynamic";
        if (dyn_pos || dyn_size) {
            const auto& frames = model().quick_frames();
            if (auto it = frames.find(a->key); it != frames.end() && it->second.w > 0 && it->second.h > 0) {
                const ws::RectFraction& f = it->second;
                if (dyn_size) {
                    out.w = std::clamp(static_cast<float>(f.w) * r.w, std::min(400.0f, r.w), r.w);
                    out.h = std::clamp(static_cast<float>(f.h) * r.h, std::min(200.0f, r.h), r.h);
                }
                if (dyn_pos) {
                    out.x = r.x + static_cast<float>(f.x) * r.w;
                    out.y = r.y + static_cast<float>(f.y) * r.h;
                } else {
                    out.x = r.x + static_cast<float>(cfg_.quick_terminal_x()) * (r.w - out.w);
                    out.y = r.y + static_cast<float>(cfg_.quick_terminal_y()) * (r.h - out.h);
                }
                constexpr float keep = 60.0f;
                out.x = std::clamp(out.x, r.x - out.w + keep, r.x + r.w - keep);
                out.y = std::clamp(out.y, r.y, r.y + r.h - keep);   // the top edge never goes above the area
            }
        }
        return std::make_pair(*a, platform::DipRect{std::round(out.x), std::round(out.y), std::round(out.w), std::round(out.h)});
    }

    // Where a press on the borderless panel moves or resizes it: the title bar's
    // empty space and a 6 DIP band at the top move it; the other edges and the corners resize it.
    platform::HitArea quick_hit(platform::DipPoint p) const {
        using platform::HitArea;
        if (!win_) return HitArea::client;
        const platform::DipSize s = win_->client_size();
        constexpr float band = 6.0f, corner = 12.0f;
        const bool left = p.x < band, right = p.x >= s.w - band, bottom = p.y >= s.h - band, top = p.y < band;
        if (bottom && p.x < corner) return HitArea::resize_sw;
        if (bottom && p.x >= s.w - corner) return HitArea::resize_se;
        if (top && p.x < corner) return HitArea::resize_nw;
        if (top && p.x >= s.w - corner) return HitArea::resize_ne;
        if (left) return HitArea::resize_w;
        if (right) return HitArea::resize_e;
        if (bottom) return HitArea::resize_s;
        if (top || (mv_ && mv_->is_caption(ui::Vec2{p.x, p.y}))) return HitArea::caption;
        return HitArea::client;
    }

    bool start_workspace(bool first) {
        if (!first) {
            // The model is the first window's.
        } else if (opts_.local) {
            ws::WorkspaceOptions wo;
            wo.home_dir = home_dir();
            local_ws_ = std::make_unique<ws::Workspace>(ws::system_random(), ws::system_clock(), wo);
            local_ws_->reset_to_default();
        } else {
            ws::RuntimeOptions ro;
            ro.post = [pp = &p_](std::function<void()> fn) { pp->loop().post(std::move(fn)); };
            ro.config = &cfg_;
            ro.hosts.host_args = opts_.gui.config_flags();
            ro.writer.version = KT_VERSION;
#ifdef KT_HAVE_SESSIONS
            if (auto ep = control::endpoint()) ro.control_endpoint = *ep;   // KEEPTERM_CONTROL in shells
#endif
            rt_ = ws::WorkspaceRuntime::create(std::move(ro));
            if (!rt_) {
                std::fprintf(stderr, "keepterm: the workspace could not be loaded\n");
                return false;
            }
        }
        ws::Workspace& m = model();
        if (quick_) {
            window_id_ = m.open_quick_window();
        } else if (want_window_ && m.window(*want_window_)) window_id_ = *want_window_;
        else if (auto k = m.key_window(); k && m.window(*k) && !m.window(*k)->quick) window_id_ = *k;
        else if (auto it = std::find_if(m.windows().begin(), m.windows().end(), [](const ws::WindowState& w) { return !w.quick; });
                 it != m.windows().end())
            window_id_ = it->id;
        else window_id_ = m.open_window(ws::kScratchProject);
        // A window that shows nothing opens one tab.
        if (!m.shown_tab(window_id_)) {
            const ws::WindowState* ww = m.window(window_id_);
            ws::NewTabOptions o;
            o.window = window_id_;
            (void)m.new_tab(ww ? ww->shown : ws::kScratchProject, {}, o);
        }
        adapter_ = std::make_unique<WorkspaceAdapter>(m, window_id_);
        WorkspaceAdapterHooks h;
        h.pane_facts = [this](const ws::Pane& p, ui::PaneItem& out) { pane_facts(p, out); };
        h.new_project = [this] { new_project_dialog(); };
        h.copy_text = [this](const std::string& s) {
            (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(s.begin(), s.end()));
        };
        h.confirm = [this](const std::string& q, std::function<void()> force) { confirm(q, std::move(force)); };
        h.report = [this](const std::string& m2) { toast(m2); };
        h.running = [this](const ws::Pane& p) { return running_program(p); };
        h.open_folder = [this](const std::string& path) {
            if (desktop::open_folder(path)) return true;
            toast(std::string(ui::tr("The folder could not be opened")), path);
            return false;
        };
        adapter_->set_hooks(std::move(h));
        // Close of a pane that runs a program asks first (PROCESS_INFO foreground, FR-WS busy).
        m.set_busy_probe([core = &core_](const ws::Pane& p) {
            MainWindow* w = core->owner();
            return w && !w->running_program(p).empty();
        });
        target_ = std::make_unique<cmd::WorkspaceTarget>(m, window_id_, this);
        auto km = cmd::Keymap::from_config(cfg_, cmd::current_os());
        keymap_ = std::move(km.keymap);
        resolver_ = std::make_unique<cmd::KeymapResolver>(keymap_);
        return true;
    }

    void build_ui() {
        uiw_ = std::make_unique<ui::Window>(*text_);
        uiw_->set_motion(motion_of(cfg_.ui_motion()));
        uiw_->request_frame = [this] {
            if (win_) win_->request_frame();
        };
        uiw_->set_ime_rect = [this](ui::Rect r) {
            if (win_) win_->set_ime_cursor_rect(platform::DipRect{r.x, r.y, r.w, r.h});
        };
        uiw_->set_cursor = [this](platform::CursorShape c) {
            ui_cursor_ = c;
            if (win_ && !edge_cursor_) win_->set_cursor(c);
        };
        uiw_->keymap = [this](const ui::KeyEvent& k) { return on_keymap(k); };
        uiw_->timer_changed = [this](ui::TimePoint at) { arm_ui_timer(at); };
        uiw_->get_clipboard = [this] { return clipboard_cache_; };
        uiw_->set_clipboard = [this](const std::string& s) {
            clipboard_cache_ = s;
            (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(s.begin(), s.end()));
        };
        ui::MainViewOptions mo;
        const bool custom_titlebar = cfg_.window_titlebar() != "native";
        mo.mac_titlebar = kMac && custom_titlebar;
        mo.in_window_menu = !kMac;
        mo.show_status = true;
        ui::MainViewHooks hooks;
        hooks.terminal = [this](ui::PaneKey k) -> ui::ITerminalSurface* {
            auto it = panes_.find(k);
            return it == panes_.end() ? nullptr : it->second.get();
        };
        hooks.app_menu = [this](ui::Rect anchor) { open_app_menu(anchor); };
        hooks.caption_press = [this](ui::Vec2, int clicks) {
#ifdef __APPLE__
            if (clicks >= 2) mac::title_bar_double_click(surface_view_);
            else mac::drag_window(surface_view_);
#else
            (void)this;   // the window is moved by the platform elsewhere
            (void)clicks;
#endif
        };
        hooks.show_resources = [this](ui::ResourceScope sc, ui::Rect anchor, bool above) { show_resources(sc, anchor, above); };
        hooks.pane_menu_items = [this](ui::PaneKey k, bool full) { return pane_menu_items(k, full); };
        hooks.pane_focused = [this](ui::PaneKey) {
            if (resolver_) resolver_->reset();
        };
        auto mv = std::make_unique<ui::MainView>(*adapter_, std::move(hooks), mo);
        mv_ = mv.get();
        uiw_->set_root(std::move(mv));
        PaletteEnv pe;
        pe.window = uiw_.get();
        pe.target = target_.get();
        pe.keymap = &keymap_;
        pe.config = &cfg_;
        pe.model = &model();
        pe.window_id = window_id_;
        if (rt_) {
            usage_ = std::make_unique<cmd::WorkspaceUsage>(rt_->usage());
            pe.usage = usage_.get();
        }
        pe.key_context = [this] { return key_context(false); };
        pe.tab_title = [this](const ws::Tab& t) {
            const ui::TabItem* ti = adapter_->snapshot().tab(WorkspaceAdapter::key(t.id));
            return ti ? ti->title : std::string();
        };
        pe.after_run = [this] { after_command(); };
        pe.fill = [this](cmd::palette::PaletteData& d) { fill_palette_sessions(d); };
        pe.now_ms = [] {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        };
        palette_ = std::make_unique<PaletteController>(std::move(pe));
        apply_theme();
#ifdef __APPLE__
        if (mo.mac_titlebar) mac::use_unified_title_bar(surface_view_);
        install_mac_menu();
#endif
        const platform::DipSize d = win_->client_size();
        px_ = win_->client_size_px();
        r_->resize(render::PxSize{px_.w, px_.h}, scale_);
        uiw_->resize(ui::Size{d.w, d.h}, scale_);
        mv_->focus_active_pane();
    }

    // ------------------------------------------------------------ config

    void load_config() {
        config::ConfigService::Options so;
        so.load = opts_.gui.load_options();
        so.post = [this](std::function<void()> fn) { p_.loop().post(std::move(fn)); };
        if (auto svc = config::ConfigService::open(std::move(so))) {
            cfg_svc_ = std::move(*svc);
            cfg_ = cfg_svc_->current();
            cfg_subs_.push_back(cfg_svc_->subscribe(config::Apply::Live, [this](const config::Config& c, const config::ConfigDelta& d) {
                apply_live(c, d);
            }));
            cfg_subs_.push_back(cfg_svc_->subscribe(config::Apply::NewPanes, [this](const config::Config& c, const config::ConfigDelta&) {
                cfg_ = c;
                if (rt_) rt_->apply_config(c);
            }));
        } else {
            log::warn("config", "live reload off: " + svc.error().what);
            cfg_ = config::Config::load(opts_.gui.load_options());
        }
        for (const auto& d : cfg_.diagnostics()) log::warn("config", config::format(d));
        default_pt_ = opts_.gui.font_pt ? *opts_.gui.font_pt : static_cast<float>(cfg_.font_size());
        font_pt_ = std::clamp(default_pt_, kMinFontPt, kMaxFontPt);
        pad_dip_x_ = static_cast<float>(cfg_.window_padding_x());
        pad_dip_y_ = static_cast<float>(cfg_.window_padding_y());
        term_opts_.scrollback_lines = static_cast<uint32_t>(cfg_.terminal_scrollback_lines());
        term_opts_.scrollback_max_bytes =
            static_cast<uint32_t>(std::clamp<int64_t>(cfg_.terminal_scrollback_max_bytes(), 1, int64_t{1} << 30));
        rcfg_ = render_config(cfg_);
#ifdef KT_HAVE_SESSIONS
        // Claude Code statusLine relay: a hook of ours whose keepterm moved is repaired, once per
        // start (session/agent_statusline.h); a foreign statusLine is never touched.
        static std::once_flag hook_checked;
        std::call_once(hook_checked, [dir = cfg_.agents_claude_config_dir()] { session::statusline_self_check(dir); });
#endif
    }

    render::RenderConfig render_config(const config::Config& c) const {
        render::RenderConfig rc;
        rc.renderer = opts_.gui.renderer.empty() ? c.renderer() : opts_.gui.renderer;
        rc.renderer_remote = c.renderer_remote();
        rc.backend.allow_software_adapter = opts_.gui.allow_software_adapter;
        rc.state_dir = render::default_state_dir();
        return rc;
    }

    text::FontSpec font_spec() const {
        text::FontSpec spec;
        spec.size_pt = font_pt_;
        if (!cfg_.font_family().empty()) spec.families.push_back(cfg_.font_family());
        spec.fallback = cfg_.font_fallback();
        spec.line_height = static_cast<float>(cfg_.font_line_height());
        return spec;
    }

    bool resolve_fonts() {
        std::vector<std::string> missing;
        auto files = text::resolve_font_files(*source_, font_spec(), &missing);
        if (!files) return fail("no monospace font", files.error());
        for (const auto& m : missing) log::warn("gui", "font family not found: " + m);
        files_ = std::move(*files);
        return true;
    }

    void apply_live(const config::Config& c, const config::ConfigDelta& d) {
        const config::Config before = cfg_;
        cfg_ = c;
        for (const auto& diag : c.diagnostics()) log::warn("config", config::format(diag));
        if (!uiw_) return;
        bool fonts = false;
        if (d.contains(config::KeyId::font_size) && !opts_.gui.font_pt) {
            default_pt_ = std::clamp(static_cast<float>(c.font_size()), kMinFontPt, kMaxFontPt);
            font_pt_ = default_pt_;
            fonts = true;
        }
        const bool families = d.contains(config::KeyId::font_family) || d.contains(config::KeyId::font_fallback);
        if (families && !resolve_fonts()) {
            cfg_ = before;
            (void)resolve_fonts();
        }
        if (d.contains(config::KeyId::window_padding_x) || d.contains(config::KeyId::window_padding_y)) {
            pad_dip_x_ = static_cast<float>(c.window_padding_x());
            pad_dip_y_ = static_cast<float>(c.window_padding_y());
            fonts = true;
        }
        if (families || fonts || d.contains(config::KeyId::font_line_height)) load_font();
        if (d.contains(config::KeyId::renderer) || d.contains(config::KeyId::renderer_remote)) {
            rcfg_ = render_config(c);
            reselect_renderer("config changed");
        }
        if (d.contains(config::KeyId::ui_font) || d.contains(config::KeyId::ui_mono_font)) {
            if (auto uf = ui::resolve_ui_fonts(*source_, c.ui_font(), c.ui_mono_font())) text_->set_files(std::move(*uf));
            if (settings_win_) settings_win_->set_ui_fonts(c.ui_font(), c.ui_mono_font());
        }
        uiw_->set_motion(motion_of(c.ui_motion()));
        if (settings_win_) settings_win_->set_motion(motion_of(c.ui_motion()));
        apply_smooth_scroll();
        keymap_ = cmd::Keymap::from_config(c, cmd::current_os()).keymap;
        resolver_->set_keymap(keymap_);
        if (owner()) core_.sync_hotkey(c, keymap_);
        if (palette_) palette_->reconfigure();
        apply_theme();
        full_next_ = true;
        win_->request_frame();
    }

    // Terminal colors from the theme module, the chrome derived from them.
    void apply_theme() {
        const theme::Appearance os = os_appearance_ == platform::Appearance::light ? theme::Appearance::light : theme::Appearance::dark;
        ts_ = theme::make_theme_set(cfg_, os, library_);
        view_theme_ = theme::to_view_theme(ts_);
        ui::ThemeSource src;
        src.background = ts_.theme.background;
        src.foreground = ts_.theme.foreground;
        src.accent = ts_.theme.accent;
        for (size_t i = 0; i < 16; ++i) src.palette[i] = ts_.theme.palette[i];
        src.unfocused_split_opacity = static_cast<float>(cfg_.window_unfocused_split_opacity());
        ui::Theme t = ui::Theme::derive(src);
        t.font_scale = static_cast<float>(cfg_.ui_font_scale());
        if (uiw_) uiw_->set_theme(t);
        if (settings_win_) settings_win_->set_theme(t);
        for (auto& [k, ps] : panes_) {
            theme::apply(ts_, ps->view(), ps->effective_colors());
            ps->set_cursor_defaults(ts_.cursor_shape, ts_.cursor_blink);
        }
        full_next_ = true;
        if (win_) win_->request_frame();
    }

    // ------------------------------------------------------------ renderer

    bool start_renderer(const platform::NativeSurface& surf) {
        const bool cheap = render::selection_is_cheap(rcfg_);
        auto r = render::create_renderer(cheap ? rcfg_.renderer : std::string("software"), &surf, render::PxSize{px_.w, px_.h}, scale_,
                                         rcfg_);
        if (!r) return fail("renderer", r.error());
        r_ = std::move(*r);
        r_->on_switch([this](render::BackendKind from, render::BackendKind to, const std::string& why) {
            log::info("render", std::string("renderer ") + render::backend_name(from) + " -> " + render::backend_name(to) + " (" + why + ")");
            full_next_ = true;
            p_.loop().post([this] {
                if (win_ && r_) {
                    apply_smooth_scroll();   // a GPU lost to the software backend: whole rows
                    full_next_ = true;
                    status_dirty_ = true;
                    win_->request_frame();
                }
            });
        });
        log::info("render", std::string("renderer: ") + render::backend_name(r_->active()));
        if (!cheap) probe_in_background("probe finished");
        return true;
    }

    // Smooth scrolling (animations.smooth_scrolling) only where frames are cheap and the motion
    // level allows animation: a GPU backend, not a remote session under ui.motion = auto, not
    // reduced or no motion. Otherwise the viewport moves by whole rows.
    bool smooth_scroll_allowed() const {
        if (!cfg_.animations_smooth_scrolling()) return false;
        const std::string m = cfg_.ui_motion();
        if (m == "reduced" || m == "none") return false;
        if (!r_ || r_->active() == render::BackendKind::software) return false;
        if (m != "full" && render::is_remote_session()) return false;
        return true;
    }

    void apply_smooth_scroll() {
        const bool on = smooth_scroll_allowed(), snap = cfg_.animations_snap_scroll_to_row();
        for (auto& [k, ps] : panes_) ps->set_smooth_scroll(on, snap);
    }

    void reselect_renderer(const std::string& why) {
        if (!r_) return;
        if (!render::selection_is_cheap(rcfg_)) {
            probe_in_background(why);
            return;
        }
        const render::Selection sel = render::select_backends(rcfg_);
        if (auto ok = r_->switch_order(sel.order, why); !ok) fail("renderer", ok.error());
        full_next_ = true;
        status_dirty_ = true;
        if (win_) win_->request_frame();
    }

    void probe_in_background(const std::string& why) {
        if (probe_running_) return;
        if (probe_thread_.joinable()) probe_thread_.join();
        probe_running_ = true;
        probe_thread_ = std::thread([this, c = rcfg_, why] {
            render::Selection sel = render::select_backends(c);
            p_.loop().post([this, sel = std::move(sel), why] {
                probe_running_ = false;
                if (!r_) return;
                log::info("render", "probe: " + sel.why);
                if (auto ok = r_->switch_order(sel.order, why); !ok) fail("renderer", ok.error());
                full_next_ = true;
                status_dirty_ = true;
                if (win_) win_->request_frame();
            });
        });
    }

    // (Re)builds the terminal font set at the current size and scale.
    bool load_font() {
        auto s = fonts_->font_set(font_spec(), files_, scale_, text::RenderMode::gray);
        if (!s) return fail("font", s.error());
        text::FontSet* old = set_;
        set_ = *s;
        for (auto& [k, ps] : panes_) {
            ps->set_scale(scale_);
            ps->set_font_set(*set_, pad_dip_x_, pad_dip_y_);
        }
        if (old && old != set_) fonts_->release(old);
        if (uiw_) uiw_->invalidate_all();
        return true;
    }

    void set_font_size(float pt) {
        pt = std::clamp(pt, kMinFontPt, kMaxFontPt);
        if (pt == font_pt_) return;
        font_pt_ = pt;
        if (!load_font()) return;
        full_next_ = true;
        win_->request_frame();
    }

    void drop_render() {
        close_settings_window();
        if (ui_timer_) {
            p_.loop().cancel_timer(ui_timer_);
            ui_timer_ = 0;
        }
        if (tick_timer_) {
            p_.loop().cancel_timer(tick_timer_);
            tick_timer_ = 0;
        }
        if (text_) text_->set_textures(nullptr);
        uiw_.reset();
        panes_.clear();
        tex_.reset();
        if (r_) r_->detach();
        r_.reset();
    }

    // ------------------------------------------------------------ panes and the model

    void schedule_sync() {
        if (sync_posted_) return;
        sync_posted_ = true;
        p_.loop().post([this] {
            sync_posted_ = false;
            sync_now();
        });
    }

    // Panes follow the model: a PaneSession for every pane, attached once its host is Ready.
    void sync_now() {
        if (!uiw_) return;
        const ws::StateSnapshot& st = model().state();
        std::vector<std::pair<std::string, std::string>> watch;
        for (const auto& [id, pane] : st.panes) {
            const ui::PaneKey k = WorkspaceAdapter::key(id);
            auto it = panes_.find(k);
            if (it == panes_.end()) it = panes_.emplace(k, make_pane(id, k)).first;
            PaneSession& ps = *it->second;
            ps.set_read_only(pane.read_only);
            if (rt_) {
                if (pane.bind == ws::BindState::Ready && !pane.endpoint.empty()) {
                    if (!ps.linked() || ps.session_id() != pane.session.str()) {
                        if (auto r = ps.attach(pane.session.str(), pane.endpoint); !r) {
                            log::warn("gui", "attach " + pane.session.str() + ": " + r.error().what);
                            if (owner()) rt_->binder().on_host_lost(id);
                        }
                    }
                    watch.emplace_back(pane.session.str(), pane.endpoint);
                }
            } else if (!ps.linked() && !ps.exited()) {
                start_local(ps, pane);
            }
        }
        for (auto it = panes_.begin(); it != panes_.end();) {
            const auto id = adapter_->pane_id(it->first);
            if (!id || !st.panes.count(*id)) {
                facts_.erase(it->first);
                it = panes_.erase(it);
            } else {
                ++it;
            }
        }
        if (monitor_) monitor_->set_sessions(std::move(watch));
        mv_->sync();
        update_title();
        status_dirty_ = true;
        win_->request_frame();
    }

    std::unique_ptr<PaneSession> make_pane(ws::PaneId id, ui::PaneKey k) {
        view::ViewOptions vo;
        vo.theme = view_theme_;
        vo.pad_x = static_cast<int32_t>(std::round(pad_dip_x_ * scale_));
        vo.pad_y = static_cast<int32_t>(std::round(pad_dip_y_ * scale_));
        vo.smooth_scroll = smooth_scroll_allowed();
        vo.snap_scroll = cfg_.animations_snap_scroll_to_row();
        vo.encoder.alt_sends_esc = !kMac;   // macOS: Option composes characters (option_as_alt = none)
#ifdef _WIN32
        vo.encoder.console_reader = true;   // every Windows backend reads the keys through a console
#endif
        PaneEvents ev;
        ev.title_changed = [this] {
            adapter_->facts_changed();
            mv_->sync();
            update_title();
        };
        ev.cwd = [this, id](const std::string& path, const std::string& src) {
            if (!owner()) return;
            model().set_pane_cwd(id, path, ws::cwd_source_from_string(src).value_or(ws::CwdSource::LastKnown));
        };
        ev.exited = [this, id](int code) {
            if (!owner()) return;
            if (rt_) rt_->binder().on_exit(id, code);
            else (void)model().pane_exited(id);
            exit_code_ = 0;
        };
        ev.host_lost = [this, id](const std::string& why) {
            log::warn("gui", "session host lost (" + why + ")");
            if (rt_ && owner()) rt_->binder().on_host_lost(id);
        };
        ev.first_snapshot = [this, id] {
            if (!owner()) return;
            if (rt_) rt_->binder().on_first_snapshot(id);
            type_pending_run(id);
        };
        ev.bell = [this, id] {
            if (!owner() || visible_and_focused(id)) return;
            model().set_pane_attention(id, ws::Attention::Bell);
            attention();
        };
        ev.command_finished = [this, id](std::optional<int> code) {
            if (!owner() || visible_and_focused(id)) return;
            model().set_pane_attention(id, code && *code != 0 ? ws::Attention::Failed : ws::Attention::Done);
        };
        // OSC 9 / 777 (agents ask for input or report that they finished).
        ev.notify = [this, id](const std::string&, const std::string&) {
            if (!owner() || visible_and_focused(id)) return;
            model().set_pane_attention(id, ws::Attention::Bell);
            attention();
        };
        ev.resolve_path = [this, id](const std::string& raw) {
            const ws::Pane* mp = model().pane(id);
            return resolve_file(raw, mp ? mp->last_cwd : std::string());
        };
        ev.open_path = [this, id](const std::string& abs, uint32_t line, uint32_t col) { open_in_editor(id, abs, line, col); };
        ev.open_url = [this](const std::string& url, const std::string& shown) {
            if (!desktop::url_allowed(url)) {
                toast(std::string(ui::tr("This link is not opened")), url);
                return;
            }
            auto open = [this, url] {
                if (!desktop::open_url(url)) toast(std::string(ui::tr("The link could not be opened")), url);
            };
            // A terminal link is untrusted text: risky targets ask first (Macterm does too).
            const std::string risk = view::link_risk(url, shown);
            if (risk.empty()) {
                open();
                return;
            }
            ui::DialogSpec d;
            d.title = std::string(ui::tr("Open this link?"));
            d.message = risk + "\n\n" + (url.size() > 300 ? url.substr(0, 300) + "\xE2\x80\xA6" : url);
            d.width = 480;
            d.buttons.push_back({std::string(ui::tr("Copy Link")), [this, url] {
                                     clipboard_cache_ = url;
                                     (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(url.begin(), url.end()));
                                 },
                                 ui::Button::Kind::secondary, false, false});
            d.buttons.push_back({std::string(ui::tr("Cancel")), {}, ui::Button::Kind::secondary, true, true});
            d.buttons.push_back({std::string(ui::tr("Open")), open, ui::Button::Kind::primary, false, false});
            ui::show_dialog(*uiw_, std::move(d));
        };
        ev.copy_mode_changed = [this] { win_->request_frame(); };
        auto ps = std::make_unique<PaneSession>(env_, k, *set_, *tex_, vo, term_opts_, std::move(ev));
        ps->set_font_set(*set_, pad_dip_x_, pad_dip_y_);
        theme::apply(ts_, ps->view(), ps->effective_colors());
        ps->set_cursor_defaults(ts_.cursor_shape, ts_.cursor_blink);
        return ps;
    }

    // The window is in the background: the Dock icon bounces, the taskbar button flashes, the
    // X11 window gets the urgency state. No notification banners; the pane's attention mark
    // in the sidebar and the tab names it.
    void attention() {
        if (win_ && !win_->focused()) win_->request_attention();
    }

    // Pane::pending_run (a layout's or the editor's command) is typed once the pane shows its
    // session; the shell reads it from the terminal's input after its start-up files.
    void type_pending_run(ws::PaneId id) {
        const ws::Pane* mp = model().pane(id);
        if (!mp || !mp->pending_run || mp->pending_run->empty()) return;
        auto it = panes_.find(WorkspaceAdapter::key(id));
        if (it == panes_.end() || !it->second) return;
        std::string text = *mp->pending_run;
        for (char& c : text)
            if (c == '\n') c = '\r';
        model().clear_pending_run(id);
        it->second->send(text + "\r");
    }

    // The pane is in front of the user in some window: no attention mark.
    bool visible_and_focused(ws::PaneId id) const {
        std::vector<const MainWindow*> all(core_.windows.begin(), core_.windows.end());
        if (core_.quick) all.push_back(core_.quick.get());
        for (const MainWindow* w : all) {
            const ws::Tab* t = model().shown_tab(w->window_id_);
            if (t && t->focused == id && w->win_ && w->win_->focused()) return true;
        }
        return false;
    }
    bool owner() const { return core_.owner() == this; }

    void start_local(PaneSession& ps, const ws::Pane& pane) {
        pty::SpawnSpec spec = default_shell_spec();
        const bool custom = !opts_.program.empty() && !local_program_used_;
        if (custom) {
            spec.argv = opts_.program;
            spec.login_shell = false;
            local_program_used_ = true;
        }
        if (!pane.last_cwd.empty()) spec.cwd = pane.last_cwd;
        session::apply_shell(session::pane_settings(cfg_), spec, custom);
        if (auto r = ps.start_local(std::move(spec)); !r) {
            log::error("gui", "cannot start the shell: " + r.error().what);
            toast("Cannot start the shell", r.error().what);
        }
    }

    void pane_facts(const ws::Pane& p, ui::PaneItem& out) {
        const ui::PaneKey k = WorkspaceAdapter::key(p.id);
        if (auto it = panes_.find(k); it != panes_.end()) {
            out.title = it->second->title();
            out.exited = out.exited || it->second->exited();
            if (it->second->exited()) out.exit_code = it->second->exit_code();
        }
        if (auto it = facts_.find(k); it != facts_.end()) {
            apply_sample(it->second, out);
            out.res = it->second.res;
            use_newest_limits(out);
            drop_reset_limits(out.agent_stats);
        }
        if (auto it = trees_.find(p.session.str()); it != trees_.end()) {
            uint64_t shell_ms = 0, agent_ms = 0;
            uint32_t n = 0;
            std::function<void(const ui::ProcItem&)> walk = [&](const ui::ProcItem& x) {
                if (x.killable) ++n;   // the workload: the shell and everything below it
                if (x.role == "shell") shell_ms = x.start_unix_ms;
                if (x.role == "agent") agent_ms = x.start_unix_ms;
                for (const auto& c : x.children) walk(c);
            };
            for (const auto& r0 : it->second.roots) walk(r0);
            out.processes = n;
            const uint64_t since = agent_ms ? agent_ms : shell_ms;
            const auto now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            if (since && now > since) out.uptime_s = (now - since) / 1000;
        }
        out.read_only = p.read_only;
    }

    // The program in front of the shell of a live pane ("" = idle shell, exited or unknown).
    std::string running_program(const ws::Pane& p) const {
        const ui::PaneKey k = WorkspaceAdapter::key(p.id);
        if (auto it = panes_.find(k); it != panes_.end() && it->second->exited()) return {};
        auto it = facts_.find(k);
        if (it == facts_.end()) return {};
        return !it->second.agent.empty() ? it->second.agent : it->second.process;
    }

    // ------------------------------------------------------------ resource popover

    ui::ResourceTrees resource_trees() const {
        ui::ResourceTrees out;
        for (const auto& [id, pane] : model().state().panes) {
            if (auto it = trees_.find(pane.session.str()); it != trees_.end()) out[WorkspaceAdapter::key(id)] = it->second;
        }
        return out;
    }

    void kill_tree(const ui::ProcItem& p, bool tree, int& failed) {
        if (tree) {
            for (const auto& c : p.children) kill_tree(c, true, failed);   // children first
        }
        if (!p.killable) return;
        if (!session::os::kill_process(p.pid, p.start)) ++failed;
    }

    void show_resources(ui::ResourceScope scope, ui::Rect anchor, bool above) {
        if (!uiw_ || !mv_) return;
        if (panel_) uiw_->close_overlay(panel_);
        panel_trees_ = resource_trees();
        ui::ResourcePanelHooks h;
        h.trees = [this]() -> const ui::ResourceTrees& { return panel_trees_; };
        h.kill = [this](const ui::ProcItem& p, bool tree) {
            int failed = 0;
            kill_tree(p, tree, failed);
            if (failed) toast(std::string(ui::tr("Some processes could not be stopped")), p.name);
        };
        h.copy = [this](const std::string& s) {
            clipboard_cache_ = s;
            (void)p_.clipboard().write_text(platform::Selection::clipboard, std::u8string(s.begin(), s.end()));
        };
        h.refresh = [this] {
            if (monitor_) monitor_->poke();
            refresh_agent_limits(std::chrono::seconds(10));
        };
        panel_ = ui::show_resource_panel(*uiw_, *adapter_, std::move(h), scope, anchor, above);
        if (monitor_) monitor_->poke();
        refresh_agent_limits(std::chrono::seconds(10));
    }

    // agents.refresh_limits: ask every agent that runs in a pane for its account's limits, unless
    // its probe ran less than `min_gap` ago.
    void refresh_agent_limits(std::chrono::seconds min_gap) {
        if (!limits_ || !cfg_.agents_refresh_limits()) return;
        std::set<std::string> agents;
        for (const auto& [k, f] : facts_)
            if (!f.agent.empty()) agents.insert(f.agent);
        for (const std::string& a : agents) (void)limits_->request(a, min_gap);
    }

    void on_probe_limits(const std::string& agent, std::vector<agents::RateLimit> limits, std::int64_t at_ms) {
        NewestLimits& n = newest_limits_[agent];
        if (static_cast<uint64_t>(at_ms) < n.at) return;
        n.at = static_cast<uint64_t>(at_ms);
        n.limits.clear();
        for (const auto& l : limits) {
            session::proto::AgentLimit p;
            p.name = l.name;
            p.used_percent_x100 = static_cast<uint32_t>(std::clamp(l.used_percent, 0.0, 10000.0) * 100.0 + 0.5);
            p.window_minutes = l.window_minutes;
            p.resets_at_unix = l.resets_at_unix > 0 ? static_cast<uint64_t>(l.resets_at_unix) : 0;
            n.limits.push_back(std::move(p));
        }
        if (adapter_) adapter_->facts_changed();
        if (ui::ResourcePanel* rp = open_panel()) rp->tick();
    }

    ui::ResourcePanel* open_panel() const {
        if (!panel_ || !uiw_) return nullptr;
        return dynamic_cast<ui::ResourcePanel*>(uiw_->overlay(panel_));
    }

    cmd::Status toggle_resources() override {
        if (open_panel()) {
            uiw_->close_overlay(panel_);
            panel_ = 0;
            return {};
        }
        const ui::Size b = uiw_->size();
        show_resources(ui::ResourceScope{}, ui::Rect{b.w / 2, 40, 0, 0}, false);
        return {};
    }

    // The newest limits any session of the agent reported, for the panes whose own are older.
    struct NewestLimits {
        uint64_t at = 0;
        std::vector<session::proto::AgentLimit> limits;
    };
    std::map<std::string, NewestLimits> newest_limits_;

    void use_newest_limits(ui::PaneItem& out) const {
        if (out.agent.empty() || !out.agent_stats.valid) return;
        auto it = newest_limits_.find(out.agent);
        if (it == newest_limits_.end() || it->second.limits.empty()) return;
        ui::AgentStats& a = out.agent_stats;
        if (!a.limits.empty() && a.limits_unix_ms >= it->second.at) return;
        a.limits.clear();
        for (const auto& l : it->second.limits)
            a.limits.push_back(ui::AgentLimitItem{l.name, static_cast<float>(l.used_percent_x100) / 100.0f, l.window_minutes, l.resets_at_unix});
        a.limits_unix_ms = it->second.at;
    }

    // A window whose reset time has passed is over: its figure no longer says anything.
    static void drop_reset_limits(ui::AgentStats& a) {
        const auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        std::erase_if(a.limits, [now](const ui::AgentLimitItem& l) { return l.resets_at_unix && l.resets_at_unix <= now; });
    }

    void on_monitor(MonitorReport r) {
        if (!uiw_) return;
        bool changed = false;
        for (const auto& [id, pane] : model().state().panes) {
            auto it = r.sessions.find(pane.session.str());
            if (it == r.sessions.end()) continue;
            PaneStats& f = facts_[WorkspaceAdapter::key(id)];
            if (f.saved_agent != it->second.saved_agent || f.agent_restore != it->second.agent_restore ||
                f.at_prompt != it->second.at_prompt || f.cwd != it->second.cwd || f.context != it->second.context) {
                changed = true;
            }
            if (f.process != it->second.process || f.agent != it->second.agent || f.res.memory != it->second.res.memory ||
                f.res.valid != it->second.res.valid || f.res.cpu != it->second.res.cpu || f.pid != it->second.pid) {
                changed = true;
            }
            f = it->second;
        }
        // An account's limits are shared by its sessions: keep the newest report per agent.
        for (const auto& [id, s] : r.sessions) {
            if (!s.context || s.context->limits.empty() || s.agent.empty()) continue;
            NewestLimits& n = newest_limits_[s.agent];
            if (s.context->limits_unix_ms > n.at || (n.at == 0 && n.limits.empty())) {
                n.at = s.context->limits_unix_ms;
                n.limits = s.context->limits;
            }
        }
        refresh_agent_limits(std::chrono::seconds(60));
        if (r.trees.size() != trees_.size()) changed = true;
        trees_ = std::move(r.trees);
        if (r.detached) {
            detached_ = std::move(*r.detached);
            if (sessions_dialog_) refresh_sessions_dialog();
        }
        if (++ticks_ % 15 == 0) changed = true;   // uptime and process counts drift slowly
        if (ui::ResourcePanel* rp = open_panel()) {
            panel_trees_ = resource_trees();
            rp->tick();
        } else {
            panel_ = 0;
        }
        status_.total = r.total;
        if (r.total.valid) {
            status_.mem_history.push_back(static_cast<float>(r.total.memory));
            status_.cpu_history.push_back(r.total.cpu);
            if (status_.mem_history.size() > kHistory) status_.mem_history.erase(status_.mem_history.begin());
            if (status_.cpu_history.size() > kHistory) status_.cpu_history.erase(status_.cpu_history.begin());
        }
        if (r.storage) status_.storage = *r.storage;
        if (changed) {
            adapter_->facts_changed();
            mv_->sync();
        }
        status_dirty_ = true;
        win_->request_frame();
    }

    void update_status() {
        if (!status_dirty_ || !mv_) return;
        status_dirty_ = false;
        status_.renderer = r_ ? render::backend_name(r_->active()) : "";
        status_.attention = model().attention_count();
        if (status_.attention != badge_) {
            badge_ = status_.attention;
            desktop::set_badge(static_cast<int>(badge_));
        }
        if (status_.storage.valid && status_.storage.limit) {
            status_.storage.label = ui::format_bytes(status_.storage.used) + " of " + ui::format_bytes(status_.storage.limit);
        }
        mv_->set_status(status_);
    }

    void update_title() {
        const ui::WorkspaceSnapshot& s = adapter_->snapshot();
        std::string t = "keepterm";
        if (const ui::ProjectItem* p = s.shown_project()) {
            t = p->name;
            if (const ui::TabItem* tab = s.shown_tab(); tab && !tab->title.empty()) t += " \xE2\x80\x94 " + tab->title;
        }
        if (t != title_) {
            title_ = t;
            win_->set_title(std::u8string(t.begin(), t.end()));
        }
    }

    // ------------------------------------------------------------ dialogs

    void confirm(const std::string& question, std::function<void()> yes) {
        ui::DialogSpec d;
        d.title = question;
        d.buttons.push_back({"Cancel", {}, ui::Button::Kind::secondary, false, true});
        d.buttons.push_back({"Close", [this, yes = std::move(yes)] {
                                 yes();
                                 mv_->sync();
                                 mv_->focus_active_pane();
                             },
                             ui::Button::Kind::danger, true, false});
        ui::show_dialog(*uiw_, std::move(d));
    }

    // "New Project…": a folder path (the platform layer has no folder picker yet).
    // The system folder chooser; the typed path dialog where there is none.
    void new_project_dialog() {
        const std::string start = [&] {
            if (const ui::ProjectItem* p = adapter_->snapshot().shown_project(); p && !p->path.empty()) return p->path;
            return home_dir();
        }();
        if (desktop::pick_folder(surface_view_, std::string(ui::tr("Choose the folder of the new project")), start,
                                 [this](std::string path) { create_project(std::move(path)); },
                                 [this](std::function<void()> fn) { post_alive(std::move(fn)); }))
            return;
        typed_project_dialog();
    }

    void typed_project_dialog() {
        auto value = std::make_shared<std::string>(home_dir());
        auto field = std::make_unique<ui::TextField>(std::string("Folder"), "path");
        field->set_text(*value);
        field->select_all();
        field->on_change = [value](const std::string& s) { *value = s; };
        ui::TextField* raw = field.get();
        ui::DialogSpec d;
        d.title = "New Project";
        d.message = "The folder of the project. Its tabs start there.";
        d.body = std::move(field);
        d.width = 420;
        d.buttons.push_back({"Cancel", {}, ui::Button::Kind::secondary, false, true});
        d.buttons.push_back({"Create", [this, value] { create_project(*value); }, ui::Button::Kind::primary, true, false});
        ui::show_dialog(*uiw_, std::move(d));
        uiw_->set_focus(raw, true);
    }

    // A path as a terminal shows it, against the pane's folder: "~/", absolute, relative, and
    // git's "a/" and "b/" prefixes. The absolute path of an existing file or folder, else "".
    static std::string resolve_file(const std::string& raw, const std::string& cwd) {
        if (raw.empty() || raw.size() > 4096) return {};
        std::vector<std::string> tries;
        if (raw == "~" || raw.starts_with("~/") || raw.starts_with("~\\")) {
            tries.push_back(home_dir() + raw.substr(1));
        } else {
            tries.push_back(raw);
            if ((raw.starts_with("a/") || raw.starts_with("b/")) && raw.size() > 2) tries.push_back(raw.substr(2));
        }
        for (const std::string& t : tries) {
            std::filesystem::path p = platform::paths::from_utf8(t);
            if (p.is_relative()) {
                if (cwd.empty()) continue;
                p = platform::paths::from_utf8(cwd) / p;
            }
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) return platform::paths::to_utf8(p.lexically_normal().make_preferred());
        }
        return {};
    }

    // Cmd/Ctrl+click on a path: $VISUAL / $EDITOR at the line (a terminal editor in a new tab,
    // a windowed one detached), else the default application; a folder opens in the file manager.
    void open_in_editor(ws::PaneId pane, const std::string& abs, uint32_t line, uint32_t col) {
        std::error_code ec;
        if (std::filesystem::is_directory(platform::paths::from_utf8(abs), ec)) {
            if (!desktop::open_folder(abs)) toast(std::string(ui::tr("The folder could not be opened")), abs);
            return;
        }
        std::string ed = platform::paths::env_utf8("VISUAL").value_or("");
        if (ed.empty()) ed = platform::paths::env_utf8("EDITOR").value_or("");
        const app::EditorCommand c = app::editor_command(ed, abs, line, col);
        if (!c.argv.empty()) {
            if (c.gui) {
                if (desktop::run_detached(c.argv)) return;
            } else if (auto loc = model().locate(pane)) {
                ws::NewPaneSpec spec;
                std::string dir = platform::paths::to_utf8(platform::paths::from_utf8(abs).parent_path());
                std::replace(dir.begin(), dir.end(), '\\', '/');
                spec.cwd = dir;
                std::string run = app::quote_paths(c.argv, kWindows);
                if (!run.empty()) run.pop_back();
                spec.run = run;
                ws::NewTabOptions o;
                o.window = window_id_;
                if (model().new_tab(loc->project, spec, o)) return;
            }
        }
        if (!desktop::open_file(abs)) toast(std::string(ui::tr("The file could not be opened")), abs);
    }

    void create_project(std::string path) {
        while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
        if (path.starts_with("~")) path = home_dir() + path.substr(1);
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_directory(platform::paths::from_utf8(path), ec)) {
            toast("Not a folder", path);
            return;
        }
        auto r = model().find_or_add_project(path);
        if (!r) {
            toast(r.error().message);
            return;
        }
        (void)model().show_project(window_id_, *r);
        if (const ws::ProjectWorkspace* w = model().workspace_of(*r); w && w->tabs.empty()) {
            ws::NewTabOptions o;
            o.window = window_id_;
            (void)model().new_tab(*r, {}, o);
        }
        if (const ws::WindowState* w = model().window(window_id_)) {
            std::vector<ws::ProjectId> ex = w->expanded;
            if (std::find(ex.begin(), ex.end(), *r) == ex.end()) ex.push_back(*r);
            std::sort(ex.begin(), ex.end());
            (void)model().update_sidebar(window_id_, w->sidebar_width, w->sidebar_visible, std::move(ex));
        }
        sync_now();
        mv_->focus_active_pane();
    }

    // ------------------------------------------------------------ keys and commands

    cmd::KeyContext key_context(bool text_input) const {
        cmd::KeyContext kc;
        kc.bits = cmd::Ctx::Always | cmd::Ctx::Window;
        if (text_input) kc.bits |= cmd::Ctx::TextInput;
        if (palette_ && palette_->is_open()) kc.bits |= cmd::Ctx::Palette;
        else if (dynamic_cast<ui::TerminalWidget*>(uiw_->focused())) kc.bits |= cmd::Ctx::Terminal;
        const ws::WindowState* w = model().window(window_id_);
        if (w && !ws::is_fixed_workspace(w->shown)) kc.bits |= cmd::Ctx::HasProject;
        if (const ws::Tab* t = model().shown_tab(window_id_); t && t->tree.size() > 1) kc.bits |= cmd::Ctx::HasSplit;
        if (const ws::ProjectWorkspace* pw = model().workspace_of(w ? w->shown : ws::kScratchProject))
            kc.tab_count = static_cast<uint32_t>(pw->tabs.size());
        if (auto id = focused_id()) {
            if (auto it = facts_.find(WorkspaceAdapter::key(*id)); it != facts_.end()) {
                kc.foreground_program = it->second.process;
                kc.foreground_known = true;
            }
        }
        return kc;
    }

    bool on_keymap(const ui::KeyEvent& k) {
        const bool text_input = dynamic_cast<ui::TextField*>(uiw_->focused()) != nullptr;
        // Paste into a text field reads the clipboard asynchronously.
        if (text_input && k.action != platform::KeyAction::release && (k.mods & ui::primary_mod()) &&
            (k.logical == U'v' || k.logical == U'V')) {
            (void)paste();
            win_->consume_key();
            return true;
        }
        cmd::CommandContext ctx{*target_, &cfg_, key_context(text_input), cmd::InvokeSource::Keymap};
        const cmd::KeyResult r = resolver_->dispatch(k, ctx, cmd::builtin_registry());
        if (r == cmd::KeyResult::Passthrough) return false;
        win_->consume_key();
        after_command();
        return true;
    }

    void invoke(const std::string& id, cmd::InvokeSource src) {
        const cmd::Command* c = cmd::builtin_registry().find(id);
        if (!c) return;
        cmd::CommandContext ctx{*target_, &cfg_, key_context(dynamic_cast<ui::TextField*>(uiw_->focused()) != nullptr), src};
        if (auto st = cmd::builtin_registry().invoke(c->id, ctx); !st) toast(st.error().what);
        after_command();
    }

    bool available(const std::string& id) const {
        const cmd::Command* c = cmd::builtin_registry().find(id);
        if (!c) return false;
        cmd::CommandContext ctx{*target_, &cfg_, key_context(false), cmd::InvokeSource::Menu};
        return cmd::builtin_registry().availability(*c, ctx).enabled;
    }

    void after_command() {
        if (!uiw_) return;
        mv_->sync();
        if (!uiw_->has_modal() && !dynamic_cast<ui::TextField*>(uiw_->focused())) mv_->focus_active_pane();
        win_->request_frame();
    }

    std::string caps(std::string_view id) const {
        auto s = keymap_.key_caps(id);
        return s ? cmd::format_sequence(*s, keymap_.os(), cmd::Format::Text) : std::string();
    }

    struct MenuDef {
        const char* title;
        const char* command;   // nullptr: separator
    };

    static const std::vector<std::pair<const char*, std::vector<MenuDef>>>& menu_defs() {
        static const std::vector<std::pair<const char*, std::vector<MenuDef>>> defs = {
            {"File",
             {{"New Tab", "tab.new"},
              {"New Scratch Tab", "tab.new_scratch"},
              {"New Project\xE2\x80\xA6", "project.open"},
              {nullptr, nullptr},
              {"Save Workspace", "workspace.save"},
              {"Save Workspace As\xE2\x80\xA6", "workspace.save_as"},
              {"Open Workspace\xE2\x80\xA6", "workspace.open"},
              {"Manage Workspaces\xE2\x80\xA6", "workspace.manage"},
              {"Start Clean\xE2\x80\xA6", "workspace.start_clean"},
              {"Restore Last Workspace", "workspace.restore_last"},
              {"Export Workspace State\xE2\x80\xA6", "workspace.export"},
              {"Import Workspace State\xE2\x80\xA6", "workspace.import"},
              {nullptr, nullptr},
              {"Split Right", "pane.split_right"},
              {"Split Down", "pane.split_down"},
              {nullptr, nullptr},
              {"Close Pane", "pane.close"},
              {"Close Tab", "tab.close"},
              {"Close Window", "window.close"}}},
            {"Edit", {{"Paste", "terminal.paste"}, {nullptr, nullptr}, {"Clear Screen and Scrollback", "terminal.clear"}}},
            {"View",
             {{"Toggle Sidebar", "view.toggle_sidebar"},
              {"Zoom Pane", "pane.zoom"},
              {"Equalize Panes", "pane.equalize"},
              {"Read-Only Mode", "pane.read_only"},
              {nullptr, nullptr},
              {"Bigger Font", "terminal.font_bigger"},
              {"Smaller Font", "terminal.font_smaller"},
              {"Reset Font Size", "terminal.font_reset"}}},
            {"Tab",
             {{"Rename Tab\xE2\x80\xA6", "tab.rename"},
              {"Next Tab", "tab.next"},
              {"Previous Tab", "tab.previous"},
              {nullptr, nullptr},
              {"Next Pane", "pane.next"},
              {"Previous Pane", "pane.previous"},
              {nullptr, nullptr},
              {"Rename Project\xE2\x80\xA6", "project.rename"},
              {"Next Project", "project.next"},
              {"Previous Project", "project.previous"},
              {nullptr, nullptr},
              {"Save Layout", "project.save_layout"},
              {"Apply Layout", "project.apply_layout"}}},
            {"Window",
             {{"Sessions\xE2\x80\xA6", "session.overview"},
              {"Quick Terminal", "quick_terminal.toggle"},
              {nullptr, nullptr},
              {"Reload Config", "app.reload_config"}}},
        };
        return defs;
    }

    // The in-window menu (Windows, Linux): the menu button of the title bar.
    void open_app_menu(ui::Rect anchor) {
        std::vector<ui::MenuItem> items;
        for (const auto& [title, defs] : menu_defs()) {
            ui::MenuItem top;
            top.label = title;
            for (const MenuDef& d : defs) {
                if (!d.command) {
                    top.submenu.push_back(ui::MenuItem::sep());
                    continue;
                }
                ui::MenuItem m;
                m.label = d.title;
                m.shortcut = caps(d.command);
                m.enabled = available(d.command);
                m.action = [this, id = std::string(d.command)] { invoke(id, cmd::InvokeSource::Menu); };
                top.submenu.push_back(std::move(m));
            }
            items.push_back(std::move(top));
        }
        items.push_back(ui::MenuItem::sep());
        ui::MenuItem st;
        st.label = "Settings\xE2\x80\xA6";
        st.shortcut = caps("app.settings");
        st.action = [this] { invoke("app.settings", cmd::InvokeSource::Menu); };
        items.push_back(std::move(st));
        ui::MenuItem q;
        q.label = "Quit";
        q.shortcut = caps("app.quit");
        q.action = [this] { invoke("app.quit", cmd::InvokeSource::Menu); };
        items.push_back(std::move(q));
        ui::MenuItem qe;
        qe.label = "Quit and End All Sessions\xE2\x80\xA6";
        qe.shortcut = caps("app.quit_end_sessions");
        qe.action = [this] { invoke("app.quit_end_sessions", cmd::InvokeSource::Menu); };
        items.push_back(std::move(qe));
        ui::open_menu_below(*uiw_, std::move(items), anchor);
    }

#ifdef __APPLE__
    void install_mac_menu() {
        core_.menu_owner = this;
        std::vector<mac::MenuEntry> menus;
        for (const auto& [title, defs] : menu_defs()) {
            mac::MenuEntry top;
            top.title = title;
            for (const MenuDef& d : defs) {
                if (!d.command) {
                    top.submenu.push_back(mac::MenuEntry::sep());
                    continue;
                }
                mac::MenuEntry e;
                e.title = d.title;
                e.command = d.command;
                // Key equivalents only for Cmd chords: the menu bar takes them before the view.
                if (auto s = keymap_.key_caps(d.command); s && s->length == 1 && (s->front().mods & cmd::Super)) {
                    const cmd::Chord& c = s->front();
                    if (c.key.kind == cmd::Key::Kind::Char && c.key.ch < 0x80) {
                        e.key = std::string(1, static_cast<char>(c.key.ch));
                        e.cmd = true;
                        e.shift = (c.mods & cmd::Shift) != 0;
                        e.alt = (c.mods & cmd::Alt) != 0;
                        e.ctrl = (c.mods & cmd::Ctrl) != 0;
                    }
                }
                top.submenu.push_back(std::move(e));
            }
            menus.push_back(std::move(top));
        }
        mac::install_menu_bar(
            "keepterm", menus,
            [this](const std::string& id) {
                if (!route_menu_to_settings(id)) invoke(id, cmd::InvokeSource::Menu);
            },
            [this](const std::string& id) { return available(id); });
    }
#endif

    // ------------------------------------------------------------ timers and frames

    void arm_ui_timer(ui::TimePoint at) {
        if (ui_timer_) p_.loop().cancel_timer(ui_timer_);
        const auto delay = at - ui::Clock::now();
        ui_timer_ = p_.loop().add_timer(Clock::now() + std::chrono::duration_cast<Clock::duration>(delay), [this] {
            ui_timer_ = 0;
            if (!uiw_) return;
            uiw_->run_timers();
            if (auto next = uiw_->next_deadline()) arm_ui_timer(*next);
            win_->request_frame();
        });
    }

    // Once a second: the runtime's tick (palette usage saves), the status figures.
    void tick_timer() {
        tick_timer_ = p_.loop().add_timer(Clock::now() + std::chrono::seconds(1), [this] {
            tick_timer_ = 0;
            if (rt_) rt_->tick();
            if (status_dirty_ && win_) win_->request_frame();
            tick_timer();
        });
    }

    void draw() {
        if (!r_ || !uiw_ || px_.w <= 0 || px_.h <= 0) return;
        update_status();
        render::DrawList dl;
        const auto t0 = std::chrono::steady_clock::now();
        ui::FrameResult fr = uiw_->frame(dl);
        if (ui::Widget* f = uiw_->focused()) {
            if (ui::ImeClient* ic = f->ime_client()) {
                const ui::Rect c = ic->ime_caret_rect();
                win_->set_ime_cursor_rect(platform::DipRect{c.x, c.y, c.w, c.h});
            }
        }
        const bool full = fr.full || full_next_;
        if (!full && fr.damage.empty()) return;
        auto ok = r_->begin_frame(render::Damage{fr.damage, full});
        if (!ok) {
            fail("render", ok.error());
            return;
        }
        if (!*ok) {
            full_next_ = true;
            uiw_->invalidate_all();
            return;
        }
        r_->submit(dl);
        if (auto e = r_->end_frame(); !e) fail("render", e.error());
        if (auto pr = r_->present(); !pr) fail("present", pr.error());
        full_next_ = false;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (opts_.gui.verbose && us > 8000) log::info("ui", "slow frame: " + std::to_string(us) + " us");
        if (uiw_->needs_frame()) win_->request_frame();
    }

    platform::IPlatform& p_;
    MainWindowOptions opts_;
    AppCore& core_;
    std::optional<ws::WindowId> want_window_;
    std::optional<platform::DipRect> place_;
    const bool quick_;
    platform::CursorShape ui_cursor_ = platform::CursorShape::arrow;   // what the toolkit asked for
    bool moving_ = false;        // the user moves or sizes the window
    bool edge_cursor_ = false;                                          // a panel edge's resize cursor is up                        // the quick terminal panel
    PaneEnv env_;
    float font_pt_ = 12.0f, default_pt_ = 12.0f;
    float pad_dip_x_ = 6.0f, pad_dip_y_ = 4.0f;
    vt::TerminalOptions term_opts_;

    config::Config cfg_;
    std::unique_ptr<config::ConfigService> cfg_svc_;
    std::vector<config::Subscription> cfg_subs_;
    std::unique_ptr<SettingsController> settings_;
    std::unique_ptr<UiWindowHost> settings_win_;   // after settings_: destroyed first
    ui::WidgetId settings_id_ = 0;
    config::Subscription settings_sub_;
    std::unique_ptr<store::Catalog> settings_catalog_;

    std::unique_ptr<platform::IWindow> win_;
    void* surface_view_ = nullptr;
    float scale_ = 1.0f;
    platform::PxSize px_;
    bool fullscreen_ = false;
    std::string title_;

    render::RenderConfig rcfg_;
    std::unique_ptr<render::ManagedRenderer> r_;
    std::thread probe_thread_;
    // Short session requests off the UI thread (the pane menu, the sessions list); joined at the end.
    std::vector<std::thread> workers_;
#ifdef KT_HAVE_SESSIONS
    std::unique_ptr<control::Server>& control_;   // AppCore's
#endif
    std::vector<DetachedSession> detached_;   // the monitor's last look, UI thread
    ui::WidgetId sessions_dialog_ = 0;
    std::string sessions_sig_;
    std::vector<std::shared_ptr<std::atomic<bool>>> finished_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool probe_running_ = false;
    std::unique_ptr<text::FontSystem> fonts_;
    std::unique_ptr<text::IFontSource> source_;
    text::FontFiles files_;
    text::FontSet* set_ = nullptr;
    std::unique_ptr<view::AtlasTextures> tex_;
    bool full_next_ = true;

    theme::ThemeLibrary library_;
    theme::ThemeSet ts_;
    view::Theme view_theme_;
    platform::Appearance os_appearance_ = platform::Appearance::dark;
    std::unique_ptr<platform::AppearanceWatch> appearance_;

    std::unique_ptr<ws::WorkspaceRuntime>& rt_;   // AppCore's
    bool launch_answered_ = false;   // general.startup = "ask" was answered
    mutable std::unique_ptr<ws::IFileSystem> preset_fs_;   // workspace presets and project layouts
    std::unique_ptr<ws::Workspace>& local_ws_;    // AppCore's
    ws::WindowId window_id_;
    ws::Subscription ws_sub_;
    bool sync_posted_ = false;
    bool local_program_used_ = false;
    std::unique_ptr<WorkspaceAdapter> adapter_;
    std::map<ui::PaneKey, std::unique_ptr<PaneSession>> panes_;
    std::map<ui::PaneKey, PaneStats> facts_;
    mutable std::pair<ws::ProjectId, bool> has_file_cache_{};
    mutable std::chrono::steady_clock::time_point has_file_at_{};
    std::map<std::string, ui::SessionTree> trees_;   // by session id, from the monitor
    ui::ResourceTrees panel_trees_;                   // what the open resource popover shows
    ui::WidgetId panel_ = 0;
    size_t badge_ = 0;
    uint64_t ticks_ = 0;
    ui::WidgetId find_bar_ = 0;

    cmd::Keymap keymap_;
    std::unique_ptr<cmd::KeymapResolver> resolver_;
    std::unique_ptr<cmd::WorkspaceTarget> target_;
    std::unique_ptr<cmd::WorkspaceUsage> usage_;
    std::unique_ptr<PaletteController> palette_;

    std::unique_ptr<ui::FontTextService> text_;
    std::unique_ptr<ui::Window> uiw_;
    ui::MainView* mv_ = nullptr;
    std::string clipboard_cache_;
    platform::TimerId ui_timer_ = 0, tick_timer_ = 0;

    std::unique_ptr<Monitor>& monitor_;           // AppCore's

    std::unique_ptr<LimitsRefresher> limits_;   // agents.refresh_limits
    ui::StatusInfo status_;
    bool status_dirty_ = true;
    int exit_code_ = 0;
    bool exiting_ = false;        // app.exit / app.restart accepted
    bool exit_started_ = false;
    bool dying_ = false;   // in the destructor: the native window's last events are ignored
};

AppCore::~AppCore() {
    if (hotkey) p.unregister_global_hotkey(*hotkey);
    quick.reset();
    owned.clear();   // the windows first: they reference everything below
#ifdef KT_HAVE_SESSIONS
    control_stopping = true;
    if (control) control->stop();
    control.reset();
#endif
    monitor.reset();
    if (rt) rt->flush();
}

bool AppCore::open(std::optional<ws::WindowId> wid, std::optional<platform::DipRect> place) {
    auto w = std::make_unique<MainWindow>(*this, wid, place);
    MainWindow* raw = w.get();
    owned.push_back(std::move(w));
    if (raw->init()) return true;
    windows.erase(std::remove(windows.begin(), windows.end(), raw), windows.end());
    // Destroyed later: init may have left callbacks that run on this turn of the loop.
    p.loop().post([this, raw] {
        owned.erase(std::remove_if(owned.begin(), owned.end(), [raw](const auto& o) { return o.get() == raw; }), owned.end());
    });
    return false;
}

void AppCore::closed(MainWindow* w) {
    const auto it = std::find(windows.begin(), windows.end(), w);
    if (it == windows.end()) return;
    windows.erase(it);
#ifdef __APPLE__
    if (menu_owner == w) {
        menu_owner = nullptr;
        if (!windows.empty()) windows.front()->install_mac_menu();
    }
#endif
    if (windows.empty()) {
        p.loop().quit(exit_code);
        return;
    }
    // Not from inside its own callback.
    p.loop().post([this, w] {
        owned.erase(std::remove_if(owned.begin(), owned.end(), [w](const auto& o) { return o.get() == w; }), owned.end());
    });
}

MainWindow* AppCore::route() const {
    if (windows.empty()) return nullptr;
    const ws::Workspace* m = rt ? &rt->model() : local_ws.get();
    if (m) {
        if (const auto k = m->key_window()) {
            for (MainWindow* w : windows)
                if (w->window_id() == *k) return w;
            if (quick && quick_shown && quick->window_id() == *k) return quick.get();   // the panel has the focus
        }
    }
    return windows.front();
}

void AppCore::quit_all() {
    quitting = true;
    if (quick_shown) hide_quick(false);
    for (MainWindow* w : std::vector<MainWindow*>(windows)) w->close_native();
}

void AppCore::sync_hotkey(const config::Config& c, const cmd::Keymap& km) {
    std::optional<platform::GlobalHotkey> want;
    std::string chord;
    if (!c.quick_terminal_enabled()) hotkey_error = "off (quick_terminal.enabled = false)";
    if (c.quick_terminal_enabled() && rt) {
        if (auto s = km.key_caps("quick_terminal.toggle")) {
            chord = cmd::format_sequence(*s, km.os(), cmd::Format::Text);
            want = global_hotkey_of(*s);
            if (!want) {
                hotkey_error = "this shortcut cannot be global";
                chord.clear();
            }
        }
    }
    if (hotkey && chord == hotkey_chord) return;   // unchanged
    if (hotkey) p.unregister_global_hotkey(*hotkey);
    hotkey.reset();
    hotkey_chord.clear();
    if (!want) return;
    auto r = p.register_global_hotkey(*want, [this] { toggle_quick(); });
    if (!r) {
        // The in-window binding still works.
        hotkey_error = r.error().what;
        log::warn("gui", "quick terminal hotkey " + chord + ": " + hotkey_error);
        return;
    }
    hotkey = *r;
    hotkey_chord = chord;
    hotkey_error.clear();
    log::info("gui", "quick terminal hotkey " + chord);
}

bool AppCore::show_quick() {
    if (!owner() || !rt || quitting) return false;
    if (!quick) {
        auto w = std::make_unique<MainWindow>(*this, std::nullopt, std::nullopt, true);
        if (!w->init()) {
            // Destroyed later: init may have left callbacks for this turn of the loop.
            p.loop().post([dead = std::shared_ptr<MainWindow>(std::move(w))] {});
            return false;
        }
        quick = std::move(w);
    }
    quick_prev = p.foreground_token();
    quick_area.reset();
    if (auto q = quick->quick_place()) {
        quick->win_->place_on(q->first, q->second);
        quick_area = q->first;
    }
    quick_shown = true;
    quick->win_->show(true);
    quick->full_next_ = true;
    quick->win_->request_frame();
    return true;
}

void AppCore::hide_quick(bool give_back_focus) {
    if (!quick || !quick_shown) return;
    quick_shown = false;
    // Dynamic mode remembers where the user moved or sized it, per monitor.
    if (quick->win_ && quick_area && rt && quick_area->area.w > 0 && quick_area->area.h > 0 &&
        (quick->cfg_.quick_terminal_position_mode() == "dynamic" || quick->cfg_.quick_terminal_size_mode() == "dynamic")) {
        const platform::DipRect r = quick->win_->placement().rect, a = quick_area->area;
        if (r.w > 0 && r.h > 0)
            rt->model().update_quick_frame(quick_area->key, ws::RectFraction{(r.x - a.x) / a.w, (r.y - a.y) / a.h, r.w / a.w, r.h / a.h});
    }
    if (quick->rt_) quick->rt_->flush();
    if (quick->win_) quick->win_->hide();
    if (give_back_focus) p.restore_foreground(quick_prev);
    quick_prev = 0;
}

void AppCore::quick_closed(MainWindow* w) {
    if (quick.get() != w) return;
    quick_shown = false;
    if (rt) (void)rt->model().close_window(w->window_id());
    // Not from inside its own callback.
    p.loop().post([dead = std::shared_ptr<MainWindow>(quick.release())] {});
}

}  // namespace

int run_main_window(platform::IPlatform& p, MainWindowOptions opts) {
    AppCore core(p, std::move(opts));
    if (!core.open(std::nullopt)) return 127;
    return p.loop().run();
}

}  // namespace kt::app
