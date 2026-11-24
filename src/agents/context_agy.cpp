// Antigravity CLI context probe.
//
// The CLI keeps each conversation in a SQLite database,
// <home>/.gemini/antigravity-cli/conversations/<id>.db. Table gen_metadata has one row per model
// call; its `data` column is a protobuf message (no published schema). Read from the CLI's own
// databases:
//
//   1 (message, the call)
//     17 (message)
//       2 (message, usage): 2 input tokens not from the cache, 5 input tokens from the cache,
//                           3 output tokens, 9 thinking tokens
//     19 (string) the model id ("gemini-3.8-flash-n")
//     21 (string) the model's display name ("Gemini 3.8 Flash (High)")
//
// The context is the prompt of the newest call: field 2 + field 5. The window comes from the
// model table (Gemini 1M). Everything fails soft: a blob that is not a well-formed message, a
// missing field or an unreadable database gives "no data", never an error. No message text is
// read: only these numbers and the two model strings.
#include <array>

#include "agents/context.h"

namespace kt::agents {
namespace {

constexpr std::size_t kMaxRows = 64;          // the newest calls; the totals then cover only them
constexpr std::size_t kMaxModelString = 128;

// A bounds-checked protobuf reader over one message.
class Pb {
public:
    explicit Pb(std::string_view b) : b_(b) {}
    bool done() const { return i_ >= b_.size(); }
    // The next field; false at the end or on malformed input (then ok() is false).
    bool next(std::uint32_t& field, std::uint8_t& wire) {
        if (done()) return false;
        std::uint64_t key = 0;
        if (!varint(key) || (key >> 3) == 0 || (key >> 3) > 0x1FFFFFFF) return fail();
        field = static_cast<std::uint32_t>(key >> 3);
        wire = static_cast<std::uint8_t>(key & 7);
        return true;
    }
    bool varint(std::uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (i_ >= b_.size()) return fail();
            const auto c = static_cast<unsigned char>(b_[i_++]);
            v |= static_cast<std::uint64_t>(c & 0x7F) << shift;
            if (c < 0x80) return true;
        }
        return fail();
    }
    bool bytes(std::string_view& out) {
        std::uint64_t n = 0;
        if (!varint(n) || n > b_.size() - i_) return fail();
        out = b_.substr(i_, static_cast<std::size_t>(n));
        i_ += static_cast<std::size_t>(n);
        return true;
    }
    bool skip(std::uint8_t wire) {
        std::uint64_t v = 0;
        std::string_view s;
        switch (wire) {
            case 0: return varint(v);
            case 1: return advance(8);
            case 2: return bytes(s);
            case 5: return advance(4);
            default: return fail();   // groups (3, 4) are not used by current encoders
        }
    }
    bool ok() const { return ok_; }

private:
    bool advance(std::size_t n) {
        if (n > b_.size() - i_) return fail();
        i_ += n;
        return true;
    }
    bool fail() {
        ok_ = false;
        i_ = b_.size();
        return false;
    }
    std::string_view b_;
    std::size_t i_ = 0;
    bool ok_ = true;
};

// Walks a whole message; `on` gets (field, wire, reader) and must consume the value (or skip it).
template <class F>
bool walk(std::string_view msg, F&& on) {
    Pb p(msg);
    std::uint32_t f = 0;
    std::uint8_t w = 0;
    while (p.next(f, w)) {
        if (!on(f, w, p)) return false;
    }
    return p.ok();
}

bool read_usage(std::string_view msg, AgyGeneration& g) {
    return walk(msg, [&](std::uint32_t f, std::uint8_t w, Pb& p) {
        if (w != 0) return p.skip(w);
        std::uint64_t v = 0;
        if (!p.varint(v)) return false;
        switch (f) {
            case 2: g.input = v, g.has_usage = true; break;
            case 5: g.cached = v, g.has_usage = true; break;
            case 3: g.output = v, g.has_usage = true; break;
            case 9: g.thinking = v; break;
            default: break;
        }
        return true;
    });
}

bool printable(std::string_view s) {
    if (s.empty() || s.size() > kMaxModelString) return false;
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

bool read_call(std::string_view msg, AgyGeneration& g) {
    return walk(msg, [&](std::uint32_t f, std::uint8_t w, Pb& p) {
        if (w != 2) return p.skip(w);
        std::string_view s;
        if (!p.bytes(s)) return false;
        if (f == 17) {
            // 17.2 is the usage; other submessages of 17 are not read.
            return walk(s, [&](std::uint32_t f2, std::uint8_t w2, Pb& p2) {
                if (w2 != 2) return p2.skip(w2);
                std::string_view u;
                if (!p2.bytes(u)) return false;
                if (f2 == 2) (void)read_usage(u, g);   // a malformed usage block leaves no figures
                return true;
            });
        }
        if (f == 19 && printable(s)) g.model_id = std::string(s);
        if (f == 21 && printable(s)) g.model_name = std::string(s);
        return true;
    });
}

class AgyReducer final : public IContextReducer {
public:
    AgyReducer(const IAgyStore* store, WindowOverrides w) : store_(store), windows_(std::move(w)) { clear(); }

    void feed(std::string_view) override {}
    void reset_from_tail(std::string_view) override { clear(); }
    void reset() override { clear(); }
    const ContextUsage& usage() const override { return u_; }
    bool saw_usage() const override { return saw_; }
    bool rereads() const override { return true; }

    void reread(const std::string& path) override {
        if (!store_) return;
        const std::vector<std::string> rows = store_->generations(path, kMaxRows);
        const std::string session = u_.session_id;
        clear();
        u_.session_id = session;
        for (const std::string& r : rows) apply(r);
        u_.totals_since_attach = rows.size() >= kMaxRows;
        if (saw_) u_.format = FormatStatus::Ok;
        update_window();
    }

    void set_session(std::string id) { u_.session_id = std::move(id); }

private:
    void clear() {
        u_ = {};
        u_.agent = "antigravity";
        saw_ = false;
        model_id_.clear();
    }

    void apply(std::string_view blob) {
        auto g = parse_agy_generation(blob);
        if (!g) return;
        if (!g->model_id.empty()) model_id_ = g->model_id;
        if (!g->model_name.empty()) u_.model = g->model_name;
        else if (!g->model_id.empty() && u_.model.empty()) u_.model = g->model_id;
        if (!g->has_usage) return;
        ++u_.api_calls;
        u_.input_tokens += g->input + g->cached;
        u_.cache_read_tokens += g->cached;
        u_.output_tokens += g->output + g->thinking;
        u_.used_tokens = g->input + g->cached;
        u_.used_source = Source::Data;
        saw_ = true;
    }

    void update_window() {
        u_.window_tokens.reset();
        u_.window_source = Source::Unknown;
        u_.percent.reset();
        if (auto mw = model_window(model_id_, windows_)) {
            u_.window_tokens = mw->first;
            u_.window_source = mw->second;
        }
        if (u_.window_tokens && *u_.window_tokens && u_.used_tokens) {
            u_.percent = 100.0 * static_cast<double>(*u_.used_tokens) / static_cast<double>(*u_.window_tokens);
        }
    }

    const IAgyStore* store_;
    WindowOverrides windows_;
    ContextUsage u_;
    bool saw_ = false;
    std::string model_id_;
};

class AgyProbe final : public IContextProbe {
public:
    AgyProbe(const IFileSystem& fs, const IProcessProbe& proc, const IAgyStore* store, std::string home, WindowOverrides w)
        : fs_(fs), proc_(proc), store_(store), home_(std::move(home)), windows_(std::move(w)) {}

    std::string_view agent() const override { return "antigravity"; }

    std::optional<ContextTarget> locate(const ContextQuery& q) const override {
        std::optional<AgentSessionInfo> s = q.session;
        if (!s || s->session_id.empty()) {
            AgySessionFiles files(fs_, proc_, home_);
            SessionQuery sq = q.process;
            if (sq.argv.empty()) sq.argv = q.argv;
            s = files.session_of(sq);
        }
        if (!s || s->session_id.empty()) return std::nullopt;
        const std::string dir = join_path(join_path(join_path(home_, ".gemini"), "antigravity-cli"), "conversations");
        const std::string path = join_path(dir, s->session_id + ".db");
        if (!fs_.mtime_ns(path)) return std::nullopt;   // no conversation database yet
        ContextTarget t;
        t.path = path;
        t.session_id = s->session_id;
        t.watch_dir = dir;
        return t;
    }

    std::unique_ptr<IContextReducer> reducer(const ContextTarget& t) const override {
        auto r = std::make_unique<AgyReducer>(store_, windows_);
        r->set_session(t.session_id);
        return r;
    }

private:
    const IFileSystem& fs_;
    const IProcessProbe& proc_;
    const IAgyStore* store_;
    std::string home_;
    WindowOverrides windows_;
};

}  // namespace

std::optional<AgyGeneration> parse_agy_generation(std::string_view blob) {
    AgyGeneration g;
    const bool ok = walk(blob, [&](std::uint32_t f, std::uint8_t w, Pb& p) {
        if (w != 2) return p.skip(w);
        std::string_view s;
        if (!p.bytes(s)) return false;
        if (f == 1) return read_call(s, g);
        return true;
    });
    if (!ok) return std::nullopt;
    return g;
}

std::unique_ptr<IContextReducer> make_agy_reducer(const IAgyStore* store, WindowOverrides windows) {
    return std::make_unique<AgyReducer>(store, std::move(windows));
}

std::unique_ptr<IContextProbe> make_agy_context_probe(const IFileSystem& fs, const IProcessProbe& proc,
                                                      const IAgyStore* store, std::string home, WindowOverrides windows) {
    return std::make_unique<AgyProbe>(fs, proc, store, std::move(home), std::move(windows));
}

}  // namespace kt::agents
