// The session host.
//
// Threads (blocking threads rather than one event loop per host):
//   main       setup, then the PTY reader: feed the terminal, append to the journal, fan out
//   pty-write  input and terminal replies to the PTY, in order
//   accept     one per endpoint; one reader and one writer thread per client
//   ticker     journal pump and flush, checkpoints, record.json, cwd queries
//   agents     agent capture into record.json and the agent restore (session/agent_keeper.h)
//   history    the warm store writer (session/history_writer.h), low priority, fed from the
//              journal; it takes mu_ only for short bounded reads of the scrollback
//   janitor    store retention and repacking while this host holds the leader lock
//   shutdown   SIGTERM/SIGHUP (POSIX) or the end-session window (Windows): save and end
// One mutex (mu_) guards the terminal, the client list, the journal order and the record.
// Lock order: mu_, then a client's queue lock, then the PTY lock.

#include "session/host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/log.h"
#include "config/service.h"
#include "platform/paths.h"
#include "pty/pty.h"
#include "pty/shell_integration.h"
#include "resources/containment_setup.h"
#include "session/agent_context.h"
#include "session/agent_keeper.h"
#include "session/client.h"
#include "session/cwd.h"
#include "session/history.h"
#include "session/history_writer.h"
#include "session/ipc.h"
#include "session/journal.h"
#include "session/os.h"
#include "session/pane_config.h"
#include "session/proc_chain.h"
#include "session/protocol.h"
#include "session/registry.h"
#include "store/retention.h"
#include "store/usage.h"
#include "vt/terminal.h"

#ifndef KT_VERSION
#define KT_VERSION "0.0.0"
#endif

namespace kt::session {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kMaxClientQueue = 1u << 20;   // lagging client: RESYNC
constexpr std::size_t kMaxInputQueue = 1u << 20;    // PTY input backpressure
constexpr std::size_t kJournalChunk = 32u << 10;

struct Options {
    SessionId id;
    int64_t ready = -1;
    std::optional<SessionId> restore_from;
    std::string cwd;
    vt::GridSize size{80, 24};
    bool history = true;
    // The warm history store writer. Off by default until the history on/off PTY
    // benchmark has run on every OS; --store turns it on.
    bool store = false;
    bool journal_fallback = false;
    pty::BackendChoice backend = pty::BackendChoice::Auto;
    uint32_t scrollback = 10'000;
    uint32_t ring = journal::kDefaultRing;
    uint64_t checkpoint_bytes = 4u << 20;
    std::chrono::milliseconds checkpoint_interval = 5min;
    std::chrono::milliseconds linger = 3000ms;
    std::vector<std::string> argv;
    // --env NAME=VALUE: the pane's variables for the shell (KEEPTERM_PANE, KEEPTERM_CONTROL,
    // KEEPTERM_PROFILE). They win over terminal.env of the config.
    std::vector<std::pair<std::string, std::string>> env;
    // Config (session/pane_config.h): the same files and --set values as the starting window.
    std::optional<std::string> config_file;
    std::vector<std::string> sets;
    PaneSettings pane;
    AgentSettings agents;
    // --private: a private pane; agent context telemetry withholds its values (also SET_OPTION
    // pane.private).
    bool private_pane = false;
    bool subreaper = false;   // host_process_setup() made this host a subreaper (Linux)
};

std::string backend_name(pty::BackendKind k) {
    switch (k) {
        case pty::BackendKind::Posix: return "posix";
        case pty::BackendKind::ConPty: return "conpty";
        case pty::BackendKind::ConsoleAgent: return "agent";
        case pty::BackendKind::OpenConsole: return "openconsole";
    }
    return "?";
}

std::string color_hex(uint32_t c) {
    char b[16];
    std::snprintf(b, sizeof b, "#%06x", c & 0xFFFFFFu);
    return b;
}

std::optional<uint32_t> parse_color(const std::string& s) {
    std::string t = s;
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    if (t.size() != 6) return std::nullopt;
    char* end = nullptr;
    const unsigned long v = std::strtoul(t.c_str(), &end, 16);
    if (!end || *end) return std::nullopt;
    return static_cast<uint32_t>(v);
}

pty::SpawnSpec shell_spec(const Options& o) {
    pty::SpawnSpec s;
    if (!o.argv.empty()) {
        s.argv = o.argv;
    } else {
#ifdef _WIN32
        std::string shell = os::getenv_utf8("COMSPEC");
        if (shell.empty()) shell = "cmd.exe";
        s.argv.push_back(shell);
#else
        std::string shell = os::getenv_utf8("SHELL");
        if (shell.empty()) shell = "/bin/sh";
        s.argv.push_back(shell);
        s.login_shell = true;
#endif
    }
#ifndef _WIN32
    s.env_edits.push_back({"TERM", std::string("xterm-256color")});
    s.env_edits.push_back({"COLORTERM", std::string("truecolor")});
#ifdef __APPLE__
    if (os::getenv_utf8("LANG").empty() && os::getenv_utf8("LC_ALL").empty() && os::getenv_utf8("LC_CTYPE").empty())
        s.env_edits.push_back({"LC_CTYPE", std::string("UTF-8")});
#endif
#endif
    s.env_edits.push_back({"TERM_PROGRAM", std::string("keepterm")});
    s.env_edits.push_back({"TERM_PROGRAM_VERSION", std::string(KT_VERSION)});
    s.env_edits.push_back({"KEEPTERM_SESSION", o.id.str()});
    // The GUI's own variables must not leak into the shell.
    s.env_edits.push_back({"KEEPTERM_HOST_LOG", std::nullopt});
    s.size = pty::GridSize{o.size.cols, o.size.rows, 0, 0};
    s.containment.session_id = o.id.str();
    apply_shell(o.pane, s, !o.argv.empty());
    for (const auto& [name, value] : o.env) {
        std::erase_if(s.env_edits, [&](const pty::EnvEdit& x) { return x.name == name; });
        s.env_edits.push_back({name, value});
    }
    return s;
}

struct Client {
    uint32_t id = 0;
    std::unique_ptr<Conn> conn;
    proto::Role role = proto::Role::Cli;
    bool attached = false;
    bool live = false;     // the snapshot is queued; output may follow
    proto::Resize size;
    bool exit_acked = false;

    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::vector<std::byte>> q;
    std::size_t qbytes = 0;
    bool closed = false;
};

class Host final : public vt::TerminalSink {
public:
    explicit Host(Options o) : o_(std::move(o)) {}
    int run();

    // TerminalSink, called inside feed() under mu_.
    // While the old history replays (replaying_), the terminal's answers and events belong to a
    // shell that no longer exists: they are dropped; the title is taken from record.json.
    void reply(std::span<const std::byte> bytes) override {
        if (!replaying_) push_input(bytes, true);
    }
    void title_changed(std::u8string_view t) override;
    void cwd_changed(const vt::CwdReport& r) override;
    void bell() override;
    void command_started(std::u8string_view line) override;
    void command_finished(std::optional<int> exit_code) override;

private:
    Result<void> setup();
    void fail_ready(const std::string& why);
    void pty_reader();
    void pty_writer();
    void accept_loop(ipc::Listener* l);
    void client_reader(std::shared_ptr<Client> c);
    void client_writer(std::shared_ptr<Client> c);
    void ticker();
    void integration_loop();
    void on_shutdown(bool final, bool end_tree);
    void make_keeper();
    void agents_loop();
    void capture_agent();
    // PROCESS_INFO with the foreground chain (read outside mu_).
    proto::ProcessInfo process_info_full();

    // Under mu_.
    void enqueue(Client& c, std::vector<std::byte> frame);
    void broadcast(const std::vector<std::byte>& frame);
    void send_snapshot(Client& c, bool resync);
    void apply_resize(const proto::Resize& s);
    void set_leader(const std::shared_ptr<Client>& c);
    void cwd_report(CwdSource src, const std::string& path);
    int gui_clients_locked() const;
    // `fg`: the PTY's foreground_pid(), asked for without mu_ (process_info_full).
    proto::ProcessInfo process_info_locked(const Result<pty::Pid>& fg);
    void journal_append(journal::Kind k, uint64_t seq, std::span<const std::byte> b);
    void mark_record_dirty() { record_dirty_ = true; }

    void push_input(std::span<const std::byte> bytes, bool from_terminal);
    Result<void> write_checkpoint();
    void write_record_now();
    void finish(bool delete_folder);
    void start_store(const std::string& shell);
    void stop_store(std::optional<uint64_t> ended_ms);

    Options o_;
    os::Paths paths_;
    std::unique_ptr<Registry> reg_;
    std::string state_dir_;
    uint64_t start_time_ = 0;

    std::mutex mu_;
    std::unique_ptr<vt::Terminal> term_;
    std::unique_ptr<journal::Writer> journal_;
    std::unique_ptr<HistoryWriter> store_;            // set in setup(), stopped before exit
    std::unique_ptr<store::JanitorThread> janitor_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::weak_ptr<Client> leader_;
    uint32_t next_client_id_ = 1;
    uint32_t next_snapshot_id_ = 1;
    CwdTracker cwd_;
    RecoveryRecord record_;
    bool record_dirty_ = false;
    uint64_t output_since_checkpoint_ = 0;
    uint64_t output_total_ = 0;   // under mu_: all output, for the quiet-time memory release
    Clock::time_point last_checkpoint_ = Clock::now();
    uint64_t last_checkpoint_seq_ = UINT64_MAX;
    std::atomic<bool> ended_{false};   // EXIT_STATUS sent
    bool delete_on_end_ = false;  // KILL from a client: the pane is gone
    bool pane_closed_ = false;    // EXIT_ACK{pane_closed}
    std::atomic<bool> replaying_{false};   // recover_session() feeds the old history
    std::condition_variable exit_cv_;

    std::mutex pty_mu_;
    std::unique_ptr<pty::IPty> pty_;

    std::mutex in_mu_;
    std::condition_variable in_cv_;
    std::deque<std::vector<std::byte>> in_q_;
    std::size_t in_bytes_ = 0;
    bool in_stop_ = false;

    std::unique_ptr<ipc::Listener> listener_, int_listener_;
    std::atomic<bool> stopping_{false};
    // Answers being made: a kill ends the shell before its reply is queued, so finish() waits for
    // these as well as for the queues, or the host exits before the client hears back.
    std::atomic<int> replies_due_{0};

    // Agents: the keeper, the pending restore, the live agents.* keys.
    std::unique_ptr<AgentKeeper> keeper_;
    // Agent context telemetry (session/agent_context.h); ticked by agents_loop.
    std::unique_ptr<AgentContextKeeper> context_;
    std::optional<AgentSettings> context_settings_;   // under agents_mu_: new agents.* keys
    void wake_agents() {
        std::lock_guard lk(agents_mu_);
        agents_wake_ = true;
        agents_cv_.notify_all();
    }
    std::optional<agents::AgentResume> restore_agent_;   // set in setup(), taken by agents_loop
    std::string agent_forgotten_;              // under mu_: the session "Forget agent session" dropped
    std::atomic<bool> resume_now_{false};      // SET_OPTION agent.resume_now, taken by agents_loop
    std::atomic<uint64_t> last_input_ns_{0};
    std::unique_ptr<config::ConfigService> cfg_svc_;
    std::vector<config::Subscription> cfg_subs_;
    std::mutex agents_mu_;
    std::condition_variable agents_cv_;
    bool agents_wake_ = false;   // under agents_mu_: settings changed or shutdown
    std::atomic<int> client_threads_{0};
    std::mutex save_mu_;   // checkpoints and record writes
};

// ------------------------------------------------------------------------------- setup

void Host::fail_ready(const std::string& why) {
    log::error("host", why);
    os::signal_ready(o_.ready, "ERR " + why);
    o_.ready = -1;
}

Result<void> Host::setup() {
    auto p = os::paths();
    if (!p) return Unexpected(p.error());
    paths_ = *p;
    reg_ = std::make_unique<Registry>(paths_);
    state_dir_ = os::join(paths_.state_sessions, o_.id.str());
    start_time_ = os::process_start_time(os::current_pid());

    vt::TerminalOptions topts;
    topts.scrollback_lines = o_.scrollback;
    topts.scrollback_max_bytes = o_.pane.scrollback_bytes;

    record_.id = o_.id;
    record_.created_unix_ms = os::unix_ms();
    record_.history = o_.history;
    record_.restored_from = o_.restore_from;
    std::string start_cwd = o_.cwd;
    std::string separator;

    // the old history before the child starts and before any client.
    std::string old_dir;
    if (o_.restore_from) {
        old_dir = os::join(paths_.state_sessions, o_.restore_from->str());
        if (o_.history) {
            replaying_ = true;
            auto rec = recover_session(old_dir, topts, this);
            replaying_ = false;
            if (!rec) return Unexpected(rec.error());
            log::info("host", "recovered " + o_.restore_from->str() + ": checkpoint " +
                                  (rec->checkpoint_seq ? std::to_string(*rec->checkpoint_seq) : std::string("none")) +
                                  ", " + std::to_string(rec->replayed_records) + " records, " +
                                  std::to_string(rec->replayed_bytes) + " bytes" + (rec->ring_only ? ", ring only" : "") +
                                  (rec->note.empty() ? "" : "; " + rec->note));
            term_ = std::move(rec->term);
            if (rec->record) {
                record_.history_id = rec->record->history_id.empty() ? o_.restore_from->str() : rec->record->history_id;
                if (start_cwd.empty()) start_cwd = rec->record->cwd;
                record_.title = rec->record->title;
                record_.agent = rec->record->agent;
                record_.agent_restore = rec->record->agent_restore;
                record_.theme_fg = rec->record->theme_fg;
                record_.theme_bg = rec->record->theme_bg;
                record_.theme_cursor = rec->record->theme_cursor;
            }
            separator = restored_separator(rec->last_unix_ms ? rec->last_unix_ms : os::unix_ms());
        } else if (auto r = read_recovery_record(old_dir)) {
            record_.history_id = r->history_id.empty() ? o_.restore_from->str() : r->history_id;
            if (start_cwd.empty()) start_cwd = r->cwd;
            record_.agent = r->agent;
            record_.agent_restore = r->agent_restore;
        }
    }
    // with agents.restore off the record goes; otherwise the old pane's agent is
    // resumed once the new shell is idle.
    if (!o_.agents.restore) {
        record_.agent = json::Value::null();
    } else if (o_.restore_from && record_.agent_restore) {
        restore_agent_ = agent_record_from_json(record_.agent);
    }
    if (!term_) {
        auto t = vt::Terminal::create(o_.size, topts, this);
        if (!t) return Unexpected(t.error());
        term_ = std::move(*t);
    } else {
        term_->set_scrollback_limit(o_.scrollback);
        term_->set_scrollback_max_bytes(o_.pane.scrollback_bytes);
        if (term_->size().cols != o_.size.cols || term_->size().rows != o_.size.rows) term_->resize(o_.size);
    }
    vt::ThemeColors theme;
    if (auto c = parse_color(record_.theme_fg)) theme.fg = *c;
    if (auto c = parse_color(record_.theme_bg)) theme.bg = *c;
    if (auto c = parse_color(record_.theme_cursor)) theme.cursor = *c;
    term_->set_theme(theme);

    if (start_cwd.empty()) start_cwd = os::home_dir();
    start_cwd = nearest_existing_dir(start_cwd);
    cwd_.set_initial(start_cwd);
    record_.cwd = cwd_.cwd();
    record_.cwd_source = std::string(to_string(cwd_.source()));

    // History folder and journal.
    if (auto r = os::make_private_dir(state_dir_); !r) return r;
    if (o_.history) {
        std::string why;
        auto store = os::open_journal_store(os::join(state_dir_, kJournalFile), journal::kRingOffset + o_.ring,
                                            o_.journal_fallback, &why);
        if (!store) return Unexpected(store.error());
        const bool mapped = (*store)->mapped();
        uint32_t flags = mapped ? 0u : journal::kFlagFallback;
        if (o_.restore_from) flags |= journal::kFlagRestored;
        journal_ = std::make_unique<journal::Writer>(std::move(*store), o_.id, o_.ring, flags);
        journal_->set_size(o_.size.cols, o_.size.rows);
        if (!mapped) log::info("host", "journal: buffered fallback (" + why + ")");
        if (record_.history_id.empty()) record_.history_id = o_.id.str();
        if (o_.store) {
            const pty::SpawnSpec probe = shell_spec(o_);
            start_store(probe.argv.empty() ? std::string() : probe.argv[0]);
        }
    }
    if (!separator.empty()) {
        if (term_->cursor().col != 0) separator = "\r\n" + separator;
        const uint64_t seq = term_->stream_offset();
        term_->feed(separator);
        journal_append(journal::Kind::Output, seq, std::as_bytes(std::span(separator.data(), separator.size())));
#ifdef _WIN32
        // ConPTY (and the agent behind it) repaints the whole screen when it starts (ESC[2J): the
        // restored lines and the separator go to the scrollback first, the shell gets a blank screen.
        const uint16_t used = term_->cursor().row;
        if (used > 0) {
            std::string scroll = "\x1b[" + std::to_string(term_->size().rows) + ";1H" + std::string(used, '\n') + "\x1b[H";
            const uint64_t seq2 = term_->stream_offset();
            term_->feed(scroll);
            journal_append(journal::Kind::Output, seq2, std::as_bytes(std::span(scroll.data(), scroll.size())));
        }
#endif
    }

    // our first checkpoint, then the old folder goes.
    if (o_.history) {
        if (auto r = write_checkpoint(); !r) log::warn("host", "first checkpoint: " + r.error().what);
    }
    if (!old_dir.empty() && old_dir != state_dir_) (void)os::remove_tree(old_dir);

    // The endpoint before the child: a client that sees the record can connect.
    auto l = ipc::listen(os::endpoint_name(paths_, o_.id));
    if (!l) return Unexpected(l.error());
    listener_ = std::move(*l);

    // The child.
    pty::SpawnSpec spec = shell_spec(o_);
    spec.cwd = start_cwd;
    record_.shell = spec.argv.empty() ? std::string() : spec.argv[0];
    std::string int_endpoint;
    if (o_.argv.empty() && o_.pane.shell_integration && os::getenv_utf8("KEEPTERM_SHELL_INTEGRATION") != "0") {
        const pty::BackendKind kind = pty::resolve_backend(o_.backend);
        if (!pty::expected_caps(kind).osc_passthrough) {
            // The console agent loses OSC sequences: the side channel carries them.
            int_endpoint = os::endpoint_name(paths_, o_.id, true);
            if (auto il = ipc::listen(int_endpoint, 8)) {
                int_listener_ = std::move(*il);
            } else {
                int_endpoint.clear();
            }
        }
        if (auto dir = pty::install_integration_scripts(paths_.state)) {
            pty::apply_integration(spec, *dir, int_endpoint);
        } else {
            log::warn("host", "shell integration: " + dir.error().what);
        }
    }
    // Containment of the child: a job on Windows (created by the PTY backend), a
    // delegated cgroup or the subreaper on Linux, tracking on macOS.
    resources::HostContainment contain = resources::setup_host_containment(o_.id.str(), os::profile(), o_.subreaper);
    if (!contain.note.empty()) log::info("host", "containment: " + contain.note);
    spec.containment.kind = contain.kind;
    spec.containment.cgroup_procs_fd = contain.procs_fd;
    spec.containment.handle = contain.handle;
    auto child = pty::spawn(spec, o_.backend);
    resources::release_host_containment(contain);
    if (!child) return Unexpected(child.error());
    pty_ = std::move(*child);
    record_.backend = backend_name(pty_->kind());
    record_.cols = o_.size.cols;
    record_.rows = o_.size.rows;
    record_.updated_unix_ms = os::unix_ms();
    write_record_now();

    RegistryRecord rr;
    rr.id = o_.id;
    rr.host = {os::current_pid(), start_time_};
    rr.boot_id = os::hex(os::boot_id().bytes);
    rr.endpoint = os::endpoint_name(paths_, o_.id);
    rr.build_key = build_key();
    rr.version = KT_VERSION;
    rr.profile = os::profile();
    rr.created_unix_ms = record_.created_unix_ms;
    if (auto r = reg_->write(rr); !r) log::warn("host", "registry record: " + r.error().what);
    return {};
}

// ------------------------------------------------------------------------------- run

int Host::run() {
    os::watch_shutdown([this](bool final) { on_shutdown(final, false); });
    if (auto r = setup(); !r) {
        fail_ready(r.error().what);
        if (listener_) listener_->close();
        if (reg_) reg_->remove(o_.id);
        // A failed restore must not lose the old folder; our own folder is empty.
        if (!state_dir_.empty()) (void)os::remove_tree(state_dir_);
        return 1;
    }
    make_keeper();
    if (store_) {
        // One janitor across processes (the lock file); the others stay idle and retry.
        const config::Config cfg = config::Config::load(load_options(o_.config_file, o_.sets));
        const store::Policy pol = store::policy_from_config(cfg);
        store::JanitorThread::Options jo;
        jo.policy = [pol] { return pol; };
        jo.on_report = [](const store::JanitorReport& r) {
            if (r.expired || r.closed_sessions || r.over_session_cap || r.over_total_cap || r.recompressed || r.trimmed) {
                log::info("history", "janitor: expired " + std::to_string(r.expired) + " (raw " +
                                         std::to_string(r.raw_expired) + "), capped " +
                                         std::to_string(r.over_session_cap + r.over_total_cap) + ", repacked " +
                                         std::to_string(r.recompressed) + ", trimmed " + std::to_string(r.trimmed));
            }
            if (r.pins_over_cap) log::warn("history", "pinned history alone exceeds storage.max_total");
        };
        janitor_ = std::make_unique<store::JanitorThread>(platform::paths::from_utf8(os::join(paths_.state, "history")), jo);
    }
    std::thread(&Host::pty_writer, this).detach();
    std::thread(&Host::accept_loop, this, listener_.get()).detach();
    if (int_listener_) std::thread(&Host::integration_loop, this).detach();
    std::thread(&Host::ticker, this).detach();
    std::thread(&Host::agents_loop, this).detach();
    if (!os::signal_ready(o_.ready, "OK " + std::to_string(os::current_pid()))) {
        // The spawner timed out and gave up on us: nobody will attach, so end like a KILL.
        log::warn("host", "session " + o_.id.str() + ": nobody waits for the ready line, ending");
        {
            std::lock_guard lk(mu_);
            delete_on_end_ = true;
        }
        std::lock_guard pl(pty_mu_);
        (void)pty_->signal(pty::PtySignal::Kill);
    }
    o_.ready = -1;
    log::info("host", "session " + o_.id.str() + " ready, backend " + record_.backend);
    pty_reader();
    return 0;
}

void Host::journal_append(journal::Kind k, uint64_t seq, std::span<const std::byte> b) {
    if (!journal_) return;
    const uint64_t now = os::unix_ms();
    if (k != journal::Kind::Output) {
        journal_->append(k, seq, b, now);
        return;
    }
    while (!b.empty()) {
        const std::size_t n = std::min(b.size(), kJournalChunk);
        journal_->append(k, seq, b.first(n), now);
        seq += n;
        b = b.subspan(n);
    }
}

void Host::pty_reader() {
    std::vector<std::byte> buf(64 * 1024);
    for (;;) {
        auto r = pty_->read(buf);
        if (!r) {
            log::warn("host", "pty read: " + r.error().what);
            break;
        }
        if (r->bytes) {
            const auto bytes = std::span<const std::byte>(buf.data(), r->bytes);
            std::lock_guard lk(mu_);
            const uint64_t seq = term_->stream_offset();
            term_->feed(bytes);
            journal_append(journal::Kind::Output, seq, bytes);
            output_since_checkpoint_ += bytes.size();
            output_total_ += bytes.size();
            std::vector<std::byte> frame;
            proto::encode(proto::Output{seq, bytes}, frame);
            for (auto& c : clients_) {
                if (c->live) enqueue(*c, frame);
            }
        }
        if (r->eof || stopping_) break;
    }
    if (stopping_) {
        // on_shutdown() owns the end.
        for (;;) std::this_thread::sleep_for(1h);
    }
    std::optional<pty::ExitStatus> st = pty_->exit_status();
    for (int i = 0; !st && i < 300; ++i) {
        std::this_thread::sleep_for(10ms);
        st = pty_->exit_status();
    }
    proto::ExitStatus es;
    es.code = st ? st->code : -1;
    es.signal = st && st->signal ? *st->signal : 0;
    es.ended_unix_ms = os::unix_ms();
    bool pane_closed = false;
    {
        std::unique_lock lk(mu_);
        es.final_seq = term_->stream_offset();
        ended_ = true;
        record_.exit_code = es.code;
        std::vector<std::byte> frame;
        proto::encode(es, frame);
        bool any_gui = false;
        for (auto& c : clients_) {
            if (!c->attached) continue;
            enqueue(*c, frame);
            any_gui = any_gui || c->role == proto::Role::Gui;
        }
        if (any_gui) {
            exit_cv_.wait_for(lk, o_.linger, [&] {
                for (auto& c : clients_) {
                    if (c->attached && c->role == proto::Role::Gui && !c->exit_acked) return false;
                }
                return true;
            });
        }
        pane_closed = delete_on_end_ || pane_closed_;
    }
    log::info("host", "shell ended with " + std::to_string(es.code));
    finish(pane_closed);
}

void Host::finish(bool delete_folder) {
    stopping_ = true;
    if (listener_) listener_->close();
    if (int_listener_) int_listener_->close();
    reg_->remove(o_.id);
    stop_store(os::unix_ms());   // the shell ended: the session's history is complete
    {
        std::lock_guard sl(save_mu_);
        if (journal_) (void)journal_->flush();
        if (delete_folder) {
            // Close the journal's file before removing the folder (Windows keeps open files).
            // The writer itself stays: the ticker and connection threads may still reach it.
            if (journal_) journal_->close();
            (void)os::remove_tree(state_dir_);
        } else {
            record_.updated_unix_ms = os::unix_ms();
            (void)write_recovery_record(state_dir_, record_);
        }
    }
    // Let the writers drain what is queued (EXIT_STATUS), then close the connections.
    const auto deadline = Clock::now() + 500ms;
    for (;;) {
        bool empty = replies_due_.load() == 0;
        {
            std::lock_guard lk(mu_);
            for (auto& c : clients_) {
                std::lock_guard ql(c->qmu);
                if (!c->q.empty() && !c->closed) empty = false;
            }
        }
        if (empty || Clock::now() > deadline) break;
        std::this_thread::sleep_for(5ms);
    }
    {
        std::lock_guard lk(mu_);
        for (auto& c : clients_) c->conn->shutdown();
    }
    std::_Exit(0);
}

void Host::on_shutdown(bool final, bool end_tree) {
    // One final save: a second request (a signal during a suspend) waits for the exit.
    static std::atomic<bool> once{false};
    if (final && once.exchange(true)) {
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    log::info("host", end_tree ? "suspend requested: saving history"
                      : final  ? "end of session requested: saving history"
                               : "end-session query: saving history");
    capture_agent();   // capture at quit
    {
        std::lock_guard sl(save_mu_);
        if (journal_) (void)journal_->flush();
    }
    if (o_.history) (void)write_checkpoint();
    write_record_now();
    if (!final) return;
    stop_store(std::nullopt);   // a later --restore-from continues the series
    stopping_ = true;
    if (listener_) listener_->close();
    if (int_listener_) int_listener_->close();
    reg_->remove(o_.id);
    if (end_tree) {
        // Programs that ignore SIGHUP (or outlive their console) must not stay behind. Bounded:
        // the PTY lock may be held by a stuck write, so a helper takes it while we wait at most
        // a moment.
        std::thread([this] {
            std::lock_guard pl(pty_mu_);
            if (pty_) (void)pty_->signal(pty::PtySignal::Kill);
        }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    // The shell gets SIGHUP (POSIX) or its console closes (Windows) when we exit.
    std::_Exit(0);
}

// ------------------------------------------------------------------------------- PTY input

void Host::push_input(std::span<const std::byte> bytes, bool from_terminal) {
    if (bytes.empty()) return;
    std::unique_lock lk(in_mu_);
    if (!from_terminal) {
        // Backpressure: the client's reader thread waits; the client queues, then gets Busy.
        in_cv_.wait(lk, [&] { return in_stop_ || in_bytes_ < kMaxInputQueue; });
    }
    if (in_stop_) return;
    in_q_.emplace_back(bytes.begin(), bytes.end());
    in_bytes_ += bytes.size();
    in_cv_.notify_all();
}

void Host::pty_writer() {
    std::unique_lock lk(in_mu_);
    for (;;) {
        in_cv_.wait(lk, [&] { return in_stop_ || !in_q_.empty(); });
        if (in_stop_) return;
        std::vector<std::byte> chunk = std::move(in_q_.front());
        in_q_.pop_front();
        lk.unlock();
        std::size_t off = 0;
        bool failed = false;
        while (off < chunk.size() && !failed && !stopping_) {
            Result<std::size_t> n = [&] {
                std::lock_guard pl(pty_mu_);
                return pty_->write(std::span<const std::byte>(chunk).subspan(off));
            }();
            if (!n) {
                failed = true;
            } else if (*n == 0) {
                std::this_thread::sleep_for(2ms);
            } else {
                off += *n;
            }
        }
        lk.lock();
        in_bytes_ -= chunk.size();
        if (failed) {
            for (auto& q : in_q_) in_bytes_ -= q.size();
            in_q_.clear();
        }
        in_cv_.notify_all();
    }
}

// ------------------------------------------------------------------------------- clients

void Host::accept_loop(ipc::Listener* l) {
    for (;;) {
        auto s = l->accept();
        if (!s) {
            if (s.error().code == Errc::lost || stopping_) return;
            log::warn("host", "accept: " + s.error().what);
            std::this_thread::sleep_for(50ms);
            continue;
        }
        auto c = std::make_shared<Client>();
        c->conn = std::make_unique<Conn>(std::move(*s));
        {
            std::lock_guard lk(mu_);
            c->id = next_client_id_++;
        }
        client_threads_ += 2;
        std::thread(&Host::client_writer, this, c).detach();
        std::thread(&Host::client_reader, this, c).detach();
    }
}

void Host::enqueue(Client& c, std::vector<std::byte> frame) {
    bool lagging = false;
    {
        std::lock_guard ql(c.qmu);
        if (c.closed) return;
        c.qbytes += frame.size();
        c.q.push_back(std::move(frame));
        lagging = c.live && c.qbytes > kMaxClientQueue;
        if (lagging) {
            // The client cannot keep up: drop its queue and send a fresh snapshot.
            c.q.clear();
            c.qbytes = 0;
        }
    }
    if (lagging) {
        std::vector<std::byte> f;
        proto::encode(proto::Resync{proto::ResyncReason::Lag}, f);
        {
            std::lock_guard ql(c.qmu);
            c.qbytes += f.size();
            c.q.push_back(std::move(f));
        }
        send_snapshot(c, true);
    }
    c.qcv.notify_one();
}

void Host::broadcast(const std::vector<std::byte>& frame) {
    for (auto& c : clients_) {
        if (c->live) enqueue(*c, frame);
    }
}

void Host::send_snapshot(Client& c, bool resync) {
    struct Sink final : vt::SnapshotChunkSink {
        Client& c;
        uint32_t id;
        uint64_t seq;
        uint8_t extra;
        uint32_t index = 0;
        Sink(Client& cl, uint32_t i, uint64_t s, uint8_t e) : c(cl), id(i), seq(s), extra(e) {}
        void chunk(std::span<const std::byte> bytes, bool last) override {
            proto::SnapshotChunk ch;
            ch.snapshot_id = id;
            ch.index = index++;
            ch.flags = static_cast<uint8_t>(extra | (last ? proto::kSnapLast : 0));
            ch.seq = seq;
            ch.major = vt::kSnapshotMajor;
            ch.minor = vt::kSnapshotMinor;
            ch.data = bytes;
            std::vector<std::byte> f;
            proto::encode(ch, f);
            std::lock_guard ql(c.qmu);
            if (c.closed) return;
            c.qbytes += f.size();
            c.q.push_back(std::move(f));
        }
    } sink(c, next_snapshot_id_++, term_->stream_offset(), resync ? proto::kSnapResync : 0);
    (void)term_->snapshot(vt::SnapshotOptions{}, sink);
    c.live = true;
    c.qcv.notify_one();
}

int Host::gui_clients_locked() const {
    int n = 0;
    for (auto& c : clients_) n += (c->attached && c->role == proto::Role::Gui) ? 1 : 0;
    return n;
}

void Host::apply_resize(const proto::Resize& s) {
    if (s.cols == 0 || s.rows == 0) return;
    const vt::GridSize cur = term_->size();
    if (cur.cols == s.cols && cur.rows == s.rows) return;
    const vt::GridSize g{s.cols, s.rows};
    term_->resize(g);
    {
        std::lock_guard pl(pty_mu_);
        (void)pty_->resize(pty::GridSize{s.cols, s.rows, s.px_w, s.px_h});
    }
    const uint64_t seq = term_->stream_offset();
    const std::byte payload[4] = {std::byte(s.cols & 0xFF), std::byte(s.cols >> 8), std::byte(s.rows & 0xFF),
                                  std::byte(s.rows >> 8)};
    journal_append(journal::Kind::Resize, seq, payload);
    if (journal_) journal_->set_size(s.cols, s.rows);
    record_.cols = s.cols;
    record_.rows = s.rows;
    mark_record_dirty();
    std::vector<std::byte> f;
    proto::encode(proto::Resized{s, seq}, f);
    broadcast(f);
}

void Host::set_leader(const std::shared_ptr<Client>& c) {
    auto old = leader_.lock();
    if (old == c) return;
    leader_ = c;
    std::vector<std::byte> f;
    if (old) {
        proto::encode(proto::Leader{false, c ? c->id : 0}, f);
        enqueue(*old, f);
    }
    if (c) {
        f.clear();
        proto::encode(proto::Leader{true, c->id}, f);
        enqueue(*c, f);
        apply_resize(c->size);
    }
}

proto::ProcessInfo Host::process_info_locked(const Result<pty::Pid>& fg) {
    proto::ProcessInfo pi;
    pi.host_pid = os::current_pid();
    pi.host_start = start_time_;
    const auto& ci = pty_->containment();
    pi.root_pid = ci.root_pid;
    pi.root_start = ci.root_start_time;
    pi.agent_pid = ci.agent_pid;
    pi.posix_sid = ci.posix_sid;
    pi.tty_dev = ci.tty_dev;
    pi.containment_kind = static_cast<uint8_t>(ci.kind);
    pi.containment_handle = ci.handle;
    if (fg) {
        pi.foreground_pid = *fg;
        pi.foreground_is_shell = *fg == ci.root_pid;
    }
    if (auto ids = pty_->process_ids()) pi.children = *ids;
    pi.cwd = cwd_.cwd();
    pi.cwd_source = std::string(to_string(cwd_.source()));
    pi.backend = record_.backend;
    pi.shell_kind = std::string(pty::to_string(pty::classify_shell(record_.shell).kind));
    pi.last_input_ns = last_input_ns_;
    if (auto a = agent_record_from_json(record_.agent)) {
        pi.saved_agent = a->agent;
        pi.saved_agent_session = a->session_id;
        pi.saved_agent_dir = a->directory;
    }
    pi.agent_restore = record_.agent_restore;
    return pi;
}

proto::ProcessInfo Host::process_info_full() {
    proto::ProcessInfo pi;
    // Never under mu_: ConPTY answers the foreground query (AttachConsole) only while it can
    // write its output, and the output reader needs mu_; under a flood that waits forever.
    Result<pty::Pid> fg = [&] {
        std::lock_guard pl(pty_mu_);
        return pty_->foreground_pid();
    }();
    {
        std::lock_guard lk(mu_);
        pi = process_info_locked(fg);
    }
    const ProcReader read = make_proc_reader();
    if (auto f = read(pi.root_pid)) pi.shell_image = f->image;
    if (pi.shell_image.empty()) pi.shell_image = image_name(record_.shell);
    if (pi.foreground_pid && !pi.foreground_is_shell) pi.foreground_chain = foreground_chain(pi.root_pid, pi.foreground_pid);
    return pi;
}

// ------------------------------------------------------------------------------- agents

void Host::capture_agent() {
    if (!keeper_) return;
    if (!keeper_->settings().restore) {
        std::lock_guard lk(mu_);
        if (!record_.agent.is_null()) {
            record_.agent = json::Value::null();
            mark_record_dirty();
        }
        return;
    }
    if (ended_) return;
    const proto::ProcessInfo pi = process_info_full();
    std::optional<agents::AgentResume> current;
    {
        std::lock_guard lk(mu_);
        current = agent_record_from_json(record_.agent);
    }
    auto next = keeper_->capture(pi, current);
    {
        // "Forget agent session": the same session is not taken again while it runs.
        std::lock_guard lk(mu_);
        if (next && !agent_forgotten_.empty()) {
            if (next->session_id == agent_forgotten_) next.reset();
            else agent_forgotten_.clear();
        }
    }
    if (next == current) return;
    std::lock_guard lk(mu_);
    record_.agent = agent_record_to_json(next);
    mark_record_dirty();
    log::info("agents", "session " + o_.id.str() + ": agent record " +
                            (next ? next->agent + " " + next->session_id + " in " + next->directory : std::string("cleared")));
}

void Host::make_keeper() {
    AgentPane pane;
    pane.process_info = [this]() -> std::optional<proto::ProcessInfo> {
        if (stopping_ || ended_) return std::nullopt;
        return process_info_full();
    };
    pane.type = [this](std::string_view text) {
        if (stopping_ || ended_) return false;
        push_input(std::as_bytes(std::span(text.data(), text.size())), true);
        return true;
    };
    pane.last_input_ns = [this] { return last_input_ns_.load(); };
    pane.cancelled = [this] { return stopping_.load() || ended_; };
    keeper_ = std::make_unique<AgentKeeper>(o_.agents, std::move(pane));
    context_ = std::make_unique<AgentContextKeeper>(o_.agents);
    context_->set_private(o_.private_pane);
    context_->on_wake = [this] { wake_agents(); };
}

void Host::agents_loop() {
    {
        // agents.* apply live.
        config::ConfigService::Options so;
        so.load = load_options(o_.config_file, o_.sets);
        if (auto svc = config::ConfigService::open(std::move(so))) {
            cfg_svc_ = std::move(*svc);
            cfg_subs_.push_back(cfg_svc_->subscribe("agents.", [this](const config::Config& c, const config::ConfigDelta&) {
                keeper_->set_settings(agent_settings(c));
                std::lock_guard lk(agents_mu_);
                context_settings_ = agent_settings(c);
                agents_wake_ = true;
                agents_cv_.notify_all();
            }));
        }
    }
    if (restore_agent_) {
        const std::string outcome = keeper_->restore(*restore_agent_, o_.id.str());
        (void)outcome;
        restore_agent_.reset();
    }
    // Capture: every agents.capture_interval, and every 2 s for 20 s after the foreground changed
    // (an agent writes its session file shortly after it starts).
    uint32_t last_fg = 0;
    auto last_capture = Clock::now() - 24h;
    auto fast_until = Clock::now() + 20s;
    capture_agent();
    // Context telemetry: the chain again on a foreground change and every 10 s (every 2 s while
    // the fast capture window is open), the file on every wake; AGENT_CONTEXT at most once a second.
    auto last_chain = Clock::now() - 24h;
    auto last_sent = Clock::now() - 24h;
    uint32_t ctx_fg = UINT32_MAX;
    bool ctx_dirty = false;
    while (!stopping_ && !ended_) {
        std::optional<AgentSettings> ctx_settings;
        {
            std::unique_lock lk(agents_mu_);
            agents_cv_.wait_for(lk, 1s, [&] { return agents_wake_; });
            agents_wake_ = false;
            ctx_settings.swap(context_settings_);
        }
        if (stopping_ || ended_) break;
        if (resume_now_.exchange(false)) {
            std::optional<agents::AgentResume> rec;
            {
                std::lock_guard lk(mu_);
                rec = agent_record_from_json(record_.agent);
            }
            // The restore steps of a respawn: wait for an idle shell, type the line, watch.
            if (rec) (void)keeper_->restore(*rec, o_.id.str());
        }
        if (ctx_settings) {
            context_->set_settings(*ctx_settings);
            last_chain = Clock::now() - 24h;
        }
        uint32_t fg = 0;
        {
            std::lock_guard pl(pty_mu_);
            if (auto f = pty_->foreground_pid()) fg = *f;
        }
        const auto now = Clock::now();
        if (fg != last_fg) {
            last_fg = fg;
            fast_until = now + 20s;
        }
        const auto period = now < fast_until ? std::chrono::milliseconds(2000) : keeper_->settings().capture_interval;
        if (now - last_capture >= period) {
            last_capture = now;
            capture_agent();
        }
        {
            const bool chain_due = fg != ctx_fg || now - last_chain >= (now < fast_until ? 2s : 10s);
            std::optional<proto::ProcessInfo> pi;
            if (chain_due) {
                ctx_fg = fg;
                last_chain = now;
                pi = process_info_full();
            }
            if (context_->tick(pi ? &*pi : nullptr, static_cast<int64_t>(os::unix_ms()))) ctx_dirty = true;
            if (ctx_dirty && Clock::now() - last_sent >= 1s) {
                ctx_dirty = false;
                last_sent = Clock::now();
                std::vector<std::byte> out;
                proto::encode(context_->current(), out);
                std::lock_guard lk(mu_);
                broadcast(out);
            }
        }
    }
}

void Host::client_writer(std::shared_ptr<Client> c) {
    for (;;) {
        std::vector<std::byte> f;
        {
            std::unique_lock ql(c->qmu);
            c->qcv.wait(ql, [&] { return c->closed || !c->q.empty(); });
            if (c->q.empty()) break;
            f = std::move(c->q.front());
            c->q.pop_front();
            c->qbytes -= f.size();
        }
        if (!c->conn->send(f)) {
            c->conn->shutdown();
            break;
        }
    }
    {
        std::lock_guard ql(c->qmu);
        c->closed = true;
        c->q.clear();
    }
    --client_threads_;
}

void Host::client_reader(std::shared_ptr<Client> c) {
    proto::Hello mine = make_hello(proto::Role::Host, o_.id);
    {
        std::lock_guard lk(mu_);
        mine.caps.push_back("gui-clients=" + std::to_string(gui_clients_locked()));
        mine.caps.push_back("agent-context=" + std::to_string(proto::kAgentContextVersion));
    }
    auto hello = c->conn->server_handshake(mine, 5000);
    if (hello) {
        c->role = hello->role;
        std::lock_guard lk(mu_);
        clients_.push_back(c);
    }
    while (hello) {
        auto fr = c->conn->recv(-1);
        if (!fr || !*fr) break;
        const auto& p = (*fr)->payload;
        const auto type = static_cast<proto::Type>((*fr)->type);
        std::vector<std::byte> out;
        bool stop = false;
        switch (type) {
            case proto::Type::Attach: {
                proto::Attach a;
                if (!proto::decode(p, a)) break;
                std::lock_guard lk(mu_);
                if (c->attached) break;
                c->attached = true;
                c->size = a.size;
                if (c->role == proto::Role::Gui) {
                    auto cur = leader_.lock();
                    if (!cur) {
                        set_leader(c);   // applies its size before the snapshot
                    } else {
                        proto::encode(proto::Leader{false, cur->id}, out);
                        enqueue(*c, out);
                    }
                }
                send_snapshot(*c, false);
                out.clear();
                proto::encode(proto::Cwd{cwd_.cwd(), "", std::string(to_string(cwd_.source())), term_->stream_offset()},
                              out);
                enqueue(*c, out);
                if (!term_->title().empty()) {
                    out.clear();
                    const auto t = term_->title();
                    proto::encode(proto::Title{std::string(reinterpret_cast<const char*>(t.data()), t.size()),
                                               proto::TitleSource::Osc2, term_->stream_offset()},
                                  out);
                    enqueue(*c, out);
                }
                if (context_) {
                    out.clear();
                    proto::encode(context_->current(), out);
                    enqueue(*c, out);
                }
                if (ended_) {
                    out.clear();
                    proto::ExitStatus es;
                    es.code = record_.exit_code.value_or(0);
                    es.final_seq = term_->stream_offset();
                    proto::encode(es, out);
                    enqueue(*c, out);
                }
                break;
            }
            case proto::Type::Detach: stop = true; break;
            case proto::Type::Input: {
                proto::Input in;
                if (!proto::decode(p, in)) break;
                if (!c->attached) break;
                last_input_ns_ = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
                push_input(in.bytes, false);
                break;
            }
            case proto::Type::Resize: {
                proto::Resize r;
                if (!proto::decode(p, r)) break;
                std::lock_guard lk(mu_);
                c->size = r;
                if (leader_.lock() == c) apply_resize(r);
                break;
            }
            case proto::Type::ClaimLeader: {
                std::lock_guard lk(mu_);
                if (c->attached && c->role == proto::Role::Gui) set_leader(c);
                break;
            }
            case proto::Type::SnapshotRequest: {
                std::lock_guard lk(mu_);
                if (c->attached) send_snapshot(*c, false);
                break;
            }
            case proto::Type::Ping: {
                proto::Ping ping;
                if (!proto::decode(p, ping)) break;
                proto::encode_ping(ping, true, out);
                std::lock_guard lk(mu_);
                enqueue(*c, out);
                break;
            }
            case proto::Type::AgentContextRequest: {
                proto::encode(context_ ? context_->current() : proto::AgentContext{}, out);
                std::lock_guard lk(mu_);
                enqueue(*c, out);
                break;
            }
            case proto::Type::AgentStatusline: {
                // From the relay (`keepterm --agent-statusline claude`); numbers and ids only.
                proto::AgentStatusline m;
                if (!proto::decode(p, m) || !context_) break;
                if (context_->statusline(m, static_cast<int64_t>(os::unix_ms()))) wake_agents();
                break;
            }
            case proto::Type::ProcessInfoRequest: {
                proto::encode(process_info_full(), out);
                std::lock_guard lk(mu_);
                enqueue(*c, out);
                break;
            }
            case proto::Type::Kill: {
                proto::Kill k;
                if (!proto::decode(p, k)) break;
                proto::KillResult kr;
                kr.ok = true;
                if (k.mode == proto::KillMode::Suspend) {
                    // Answer first; the save, the tree's end and the exit run on their own thread
                    // (the capture reads the process tree, which may take a moment).
                    proto::encode(kr, out);
                    {
                        std::lock_guard lk(mu_);
                        enqueue(*c, out);
                    }
                    std::thread([this] { on_shutdown(true, true); }).detach();
                    break;
                }
                {
                    std::lock_guard lk(mu_);
                    if (k.mode == proto::KillMode::KillTree || k.mode == proto::KillMode::EndHost) delete_on_end_ = true;
                }
                ++replies_due_;
                struct ReplyQueued {
                    std::atomic<int>& n;
                    ~ReplyQueued() { --n; }
                } queued{replies_due_};
                Result<void> r;
                {
                    std::lock_guard pl(pty_mu_);
                    switch (k.mode) {
                        case proto::KillMode::Hangup: r = pty_->signal(pty::PtySignal::Hangup); break;
                        case proto::KillMode::Terminate: r = pty_->signal(pty::PtySignal::Terminate); break;
                        case proto::KillMode::KillTree:
                        case proto::KillMode::EndHost: r = pty_->signal(pty::PtySignal::Kill); break;
                        case proto::KillMode::SignalPids: r = make_error(Errc::not_supported, "signal_pids"); break;
                        case proto::KillMode::Suspend: break;   // answered above
                    }
                }
                if (!r) {
                    kr.ok = false;
                    kr.detail = r.error().what;
                }
                proto::encode(kr, out);
                std::lock_guard lk(mu_);
                enqueue(*c, out);
                break;
            }
            case proto::Type::ExitAck: {
                proto::ExitAck a;
                if (!proto::decode(p, a)) break;
                std::lock_guard lk(mu_);
                c->exit_acked = true;
                if (a.pane_closed) pane_closed_ = true;
                exit_cv_.notify_all();
                break;
            }
            case proto::Type::SetOption: {
                proto::SetOption so;
                if (!proto::decode(p, so)) break;
                // The pane's agent session (the pane and sidebar menus).
                if (so.key == "agent.forget") {
                    std::lock_guard lk(mu_);
                    if (auto a = agent_record_from_json(record_.agent)) agent_forgotten_ = a->session_id;
                    record_.agent = json::Value::null();
                    mark_record_dirty();
                    break;
                }
                if (so.key == "agent.restore") {
                    std::lock_guard lk(mu_);
                    record_.agent_restore = so.value == "1" || so.value == "true";
                    mark_record_dirty();
                    break;
                }
                if (so.key == "agent.resume_now") {
                    resume_now_ = true;
                    wake_agents();
                    break;
                }
                if (so.key == "pane.private") {
                    if (context_) context_->set_private(so.value == "1" || so.value == "true");
                    wake_agents();
                    break;
                }
                std::lock_guard lk(mu_);
                vt::ThemeColors th = term_->theme();
                const auto col = parse_color(so.value);
                if (so.key == "theme.fg") {
                    th.fg = col;
                    record_.theme_fg = col ? color_hex(*col) : "";
                } else if (so.key == "theme.bg") {
                    th.bg = col;
                    record_.theme_bg = col ? color_hex(*col) : "";
                } else if (so.key == "theme.cursor") {
                    th.cursor = col;
                    record_.theme_cursor = col ? color_hex(*col) : "";
                } else {
                    break;
                }
                term_->set_theme(th);
                mark_record_dirty();
                break;
            }
            default:
                if ((*fr)->type & proto::kMustUnderstand) {
                    proto::encode(proto::ErrorMsg{proto::ErrorCode::UnknownRequiredType, "unknown type", (*fr)->type}, out);
                    std::lock_guard lk(mu_);
                    enqueue(*c, out);
                }
                break;
        }
        if (stop) break;
    }
    {
        std::lock_guard lk(mu_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
        exit_cv_.notify_all();
        if (leader_.lock() == c || leader_.expired()) {
            leader_.reset();
            // The oldest remaining GUI client leads.
            for (auto& o : clients_) {
                if (o->attached && o->role == proto::Role::Gui) {
                    set_leader(o);
                    break;
                }
            }
        }
    }
    {
        std::lock_guard ql(c->qmu);
        c->closed = true;
    }
    c->qcv.notify_one();
    c->conn->shutdown();
    --client_threads_;
}

void Host::integration_loop() {
    for (;;) {
        auto s = int_listener_->accept();
        if (!s) {
            if (s.error().code == Errc::lost || stopping_) return;
            continue;
        }
        std::shared_ptr<ipc::Stream> st(std::move(*s));
        std::thread([this, st] {
            std::byte buf[4096];
            std::size_t total = 0;
            for (;;) {
                auto n = st->read(buf, 2000);
                if (!n || *n == 0) break;
                total += *n;
                if (total > (64u << 10)) break;   // one report is small
                std::lock_guard lk(mu_);
                term_->feed_integration(std::span<const std::byte>(buf, *n));
            }
        }).detach();
    }
}

// ------------------------------------------------------------------------------- events

void Host::title_changed(std::u8string_view t) {
    if (replaying_) return;
    const std::string s(reinterpret_cast<const char*>(t.data()), t.size());
    if (record_.history) {
        record_.title = s;
        mark_record_dirty();
    }
    std::vector<std::byte> f;
    proto::encode(proto::Title{s, proto::TitleSource::Osc2, term_->stream_offset()}, f);
    broadcast(f);
}

void Host::cwd_changed(const vt::CwdReport& r) {
    if (replaying_) return;
    const std::string v(reinterpret_cast<const char*>(r.value.data()), r.value.size());
    std::optional<std::string> path;
    if (r.form == vt::CwdReport::Form::Osc7) {
        std::string host;
        path = cwd_from_osc7(v, &host);
        if (path && !host.empty() && !is_local_host(host)) return;   // ssh: a remote folder
    } else {
        path = cwd_from_osc9_9(v);
    }
    if (path) cwd_report(CwdSource::Integration, *path);
}

void Host::cwd_report(CwdSource src, const std::string& path) {
    if (!cwd_.report(src, path)) return;
    record_.cwd = cwd_.cwd();
    record_.cwd_source = std::string(to_string(cwd_.source()));
    mark_record_dirty();
    std::vector<std::byte> f;
    proto::encode(proto::Cwd{cwd_.cwd(), "", record_.cwd_source, term_->stream_offset()}, f);
    broadcast(f);
}

void Host::bell() {
    if (replaying_) return;
    std::vector<std::byte> f;
    proto::encode(proto::Bell{term_->stream_offset()}, f);
    broadcast(f);
}

void Host::command_started(std::u8string_view line) {
    if (replaying_ || !store_) return;
    store_->command_started(std::string(reinterpret_cast<const char*>(line.data()), line.size()), cwd_.cwd(),
                            term_->stream_offset(), os::unix_ms());
}

void Host::command_finished(std::optional<int> exit_code) {
    if (replaying_ || !store_) return;
    store_->command_finished(exit_code, os::unix_ms());
}

// ------------------------------------------------------------------------------- store

void Host::start_store(const std::string& shell) {
    const config::Config cfg = config::Config::load(load_options(o_.config_file, o_.sets));
    HistoryWriterOptions ho;
    ho.root = platform::paths::from_utf8(os::join(paths_.state, "history"));
    ho.session.id = record_.history_id;
    ho.session.shell = shell;
    ho.session.cwd = record_.cwd;
    ho.session.started_ms = record_.created_unix_ms;
    ho.fresh = store::fresh_options_from_config(cfg);
    ho.policy = store::policy_from_config(cfg);
    ho.on_usage = [](const store::UsageEvent& e) {
        // The GUI subscribes to these for its notifications later; until then the log has them.
        if (e.type == store::UsageEvent::Type::threshold) {
            log::info("history", "usage: " + e.limit + (e.session.empty() ? "" : " " + e.session) + " at " +
                                     std::to_string(static_cast<int>(e.at.percent)) + "% (" +
                                     (e.level ? std::to_string(e.level) + "% reached" : "below 80% again") + ")");
        } else {
            log::warn("history", e.type == store::UsageEvent::Type::writes_stopped ? "usage: warm writes stopped"
                                                                                   : "usage: warm writes resumed");
        }
    };
    auto w = HistoryWriter::start(std::move(ho), mu_, *term_);
    if (!w) {
        // The journal and the restore do not depend on the store.
        log::warn("host", "history store: " + w.error().what);
        return;
    }
    store_ = std::move(*w);
    journal_->set_tap(store_->tap());
}

void Host::stop_store(std::optional<uint64_t> ended_ms) {
    if (!store_) return;
    if (journal_) journal_->set_tap(nullptr);
    store_->stop(ended_ms);
    // The janitor is not joined: a repack in progress leaves a temp file the next pass removes.
}

// ------------------------------------------------------------------------------- saving

Result<void> Host::write_checkpoint() {
    std::lock_guard sl(save_mu_);
    if (!journal_) return {};
    std::vector<std::byte> snap;
    uint64_t seq = 0;
    {
        std::lock_guard lk(mu_);
        seq = term_->stream_offset();
        if (seq == last_checkpoint_seq_) return {};
        snap = term_->snapshot();
        output_since_checkpoint_ = 0;
        last_checkpoint_ = Clock::now();
    }
    const std::string path = os::join(state_dir_, checkpoint_name(seq));
    const auto t0 = Clock::now();
    if (auto r = os::write_file_atomic(path, std::string_view(reinterpret_cast<const char*>(snap.data()), snap.size()));
        !r) {
        return r;
    }
    journal_->set_checkpoint(seq);
    (void)journal_->flush();
    last_checkpoint_seq_ = seq;
    // Keep the newest two.
    auto list = list_checkpoints(state_dir_);
    for (std::size_t i = 2; i < list.size(); ++i) (void)os::remove_file(list[i].second);
    const auto ms = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
    log::info("host", "checkpoint " + std::to_string(seq) + ": " + std::to_string(snap.size()) + " bytes, " +
                          std::to_string(ms) + " us");
    return {};
}

void Host::write_record_now() {
    RecoveryRecord copy;
    {
        std::lock_guard lk(mu_);
        record_dirty_ = false;
        record_.updated_unix_ms = os::unix_ms();
        copy = record_;
    }
    std::lock_guard sl(save_mu_);
    if (auto r = write_recovery_record(state_dir_, copy); !r) log::warn("host", "record.json: " + r.error().what);
}

void Host::ticker() {
    auto last_flush = Clock::now();
    auto last_record = Clock::now();
    auto last_cwd = Clock::now();
    // A flood leaves free memory in the allocator (row storage, checkpoint and resync snapshots);
    // after 2 s without output it goes back to the OS, once per flood.
    uint64_t seen_output = 0;
    auto last_output = Clock::now();
    bool released = true;
    while (!stopping_) {
        std::this_thread::sleep_for(250ms);
        if (stopping_) break;
        const auto now = Clock::now();
        if (journal_) {
            std::lock_guard sl(save_mu_);
            if (journal_) (void)journal_->pump();
        }
        if (journal_ && now - last_flush >= 30s) {
            std::lock_guard sl(save_mu_);
            if (journal_) (void)journal_->flush();
            last_flush = now;
        }
        bool want_checkpoint = false;
        bool dirty = false;
        {
            std::lock_guard lk(mu_);
            want_checkpoint = journal_ && (output_since_checkpoint_ >= o_.checkpoint_bytes ||
                                           (output_since_checkpoint_ > 0 && now - last_checkpoint_ >= o_.checkpoint_interval));
            dirty = record_dirty_;
            if (output_total_ != seen_output) {
                seen_output = output_total_;
                last_output = now;
                released = false;
            }
        }
        if (want_checkpoint) {
            if (auto r = write_checkpoint(); !r) log::warn("host", "checkpoint: " + r.error().what);
        }
        if (!released && !want_checkpoint && now - last_output >= 2s) {
            os::release_free_memory();
            released = true;
        }
        if (dirty && now - last_record >= 2s) {
            write_record_now();
            last_record = now;
        }
        // cwd sources 3 and 4 while the shell reports nothing itself.
        if (now - last_cwd >= 2s) {
            last_cwd = now;
            bool integration = false;
            uint32_t fg = 0;
            {
                std::lock_guard lk(mu_);
                integration = cwd_.integration_active() || ended_;
            }
            if (!integration) {
                {
                    std::lock_guard pl(pty_mu_);
                    if (auto f = pty_->foreground_pid()) fg = *f;
                }
                auto q = fg ? query_process_cwd(fg) : Result<std::string>(make_error(Errc::unavailable, "no pid"));
                std::lock_guard lk(mu_);
                if (q && !q->empty()) {
#ifdef _WIN32
                    cwd_report(CwdSource::ProcessPeb, *q);
#else
                    cwd_report(CwdSource::OsQuery, *q);
#endif
                } else {
                    const std::u8string row = term_->cursor_row_text();
                    if (auto pc = parse_prompt_cwd(std::string_view(reinterpret_cast<const char*>(row.data()), row.size()));
                        pc && directory_exists(*pc)) {
                        cwd_report(CwdSource::PromptParse, *pc);
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------- args

bool parse_size(const std::string& s, vt::GridSize& g) {
    const auto x = s.find('x');
    if (x == std::string::npos) return false;
    const long c = std::strtol(s.c_str(), nullptr, 10);
    const long r = std::strtol(s.c_str() + x + 1, nullptr, 10);
    if (c < 2 || r < 1 || c > 4096 || r > 4096) return false;
    g.cols = static_cast<uint16_t>(c);
    g.rows = static_cast<uint16_t>(r);
    return true;
}

}  // namespace

int host_main(int argc, char** argv, int i) {
    const bool subreaper = os::host_process_setup();
    if (!os::getenv_utf8("KEEPTERM_HOST_LOG").empty()) log::set_level(log::Level::info);
    Options o;
    o.subreaper = subreaper;
    bool have_id = false;
    bool explicit_history = false, explicit_pty = false, explicit_scrollback = false, explicit_ring = false;
    std::string bad;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                bad = a + " needs a value";
                return {};
            }
            return argv[++i];
        };
        if (a == "--session-host") {
            auto id = SessionId::parse(next());
            if (!id) {
                bad = "bad session id";
                break;
            }
            o.id = *id;
            have_id = true;
        } else if (a == "--ready") {
            o.ready = std::strtoll(next().c_str(), nullptr, 10);
        } else if (a == "--restore-from") {
            auto id = SessionId::parse(next());
            if (!id) {
                bad = "bad --restore-from id";
                break;
            }
            o.restore_from = *id;
        } else if (a == "--cwd") {
            o.cwd = next();
        } else if (a == "--size") {
            if (!parse_size(next(), o.size)) bad = "bad --size";
        } else if (a == "--private") {
            o.private_pane = true;
        } else if (a == "--no-history") {
            o.history = false;
            explicit_history = true;
        } else if (a == "--store") {
            o.store = true;
        } else if (a == "--no-store") {
            o.store = false;
        } else if (a == "--config") {
            o.config_file = next();
        } else if (a == "--set") {
            o.sets.push_back(next());
        } else if (a == "--env") {
            const std::string kv = next();
            const auto eq = kv.find('=');
            const std::string name = kv.substr(0, eq);
            // A plain variable name; the host's own variables stay the host's.
            const bool ok_name = !name.empty() && name.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos &&
                !(name[0] >= '0' && name[0] <= '9');
            if (eq == std::string::npos || !ok_name || name == "KEEPTERM_SESSION" || name == "KEEPTERM_HOST_LOG") {
                if (bad.empty()) bad = "bad --env " + name;
            } else {
                o.env.emplace_back(name, kv.substr(eq + 1));
            }
        } else if (a == "--journal-fallback") {
            o.journal_fallback = true;
        } else if (a.rfind("--pty=", 0) == 0) {
            const std::string v = a.substr(6);
            if (v == "auto") o.backend = pty::BackendChoice::Auto;
            else if (v == "posix") o.backend = pty::BackendChoice::Posix;
            else if (v == "conpty") o.backend = pty::BackendChoice::ConPty;
            else if (v == "agent") o.backend = pty::BackendChoice::Agent;
            else bad = "bad --pty";
            explicit_pty = true;
        } else if (a == "--scrollback") {
            o.scrollback = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
            explicit_scrollback = true;
        } else if (a == "--ring") {
            o.ring = std::max<uint32_t>(journal::kMinRing, static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10)));
            explicit_ring = true;
        } else if (a == "--checkpoint-bytes") {
            o.checkpoint_bytes = std::max<uint64_t>(1, std::strtoull(next().c_str(), nullptr, 10));
        } else if (a == "--linger") {
            o.linger = std::chrono::milliseconds(std::strtol(next().c_str(), nullptr, 10));
        } else if (a == "--") {
            for (++i; i < argc; ++i) o.argv.emplace_back(argv[i]);
            break;
        } else {
            bad = "unknown option " + a;
        }
        if (!bad.empty()) break;
    }
    if (!have_id && bad.empty()) bad = "--session-host needs an id";
    if (!bad.empty()) {
        os::signal_ready(o.ready, "ERR " + bad);
        std::fprintf(stderr, "keepterm: %s\n", bad.c_str());
        return 2;
    }
    // The config's new-pane keys; explicit options win (session/pane_config.h).
    const config::Config cfg = config::Config::load(load_options(o.config_file, o.sets));
    for (const auto& d : cfg.diagnostics()) log::warn("config", config::format(d));
    o.pane = pane_settings(cfg);
    o.agents = agent_settings(cfg);
    if (!explicit_history) o.history = o.pane.history;
    if (!explicit_scrollback) o.scrollback = o.pane.scrollback;
    if (!explicit_ring && o.pane.history_ring) o.ring = std::max<uint32_t>(journal::kMinRing, o.pane.history_ring);
#ifdef _WIN32
    if (!explicit_pty && !o.pane.pty.empty()) {
        o.backend = o.pane.pty == "conpty" ? pty::BackendChoice::ConPty : pty::BackendChoice::Agent;
    }
#else
    (void)explicit_pty;
#endif
    Host h(std::move(o));
    return h.run();
}

}  // namespace kt::session
