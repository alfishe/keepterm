#include "session/history_output.h"

#include <algorithm>
#include <deque>
#include <filesystem>

#include "platform/paths.h"
#include "store/catalog.h"
#include "store/session_store.h"

namespace kt::session {

namespace fs = std::filesystem;

namespace {

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// Finds the target in a stream of lines: the exact line (its seq, its text's start) as soon as it
// comes; else, once the stream is past what may hold it, the first line at or after the seq that
// contains the text within `scan` lines.
class TargetScan {
public:
    TargetScan(const OutputTarget& t, std::size_t scan) : t_(t), scan_(scan) {}
    // Line `index` of the stream; true when the target is decided (found() tells which).
    bool add(const OutputLine& l, uint64_t index) {
        if (decided_) return true;
        if (l.seq < t_.seq) return false;
        if (!since_) since_ = index;
        if (l.seq == t_.seq && starts_with(l.text, t_.text)) return decide(index);
        if (!fallback_ && !t_.text.empty() && index - *since_ <= scan_ && l.text.find(t_.text) != std::string::npos)
            fallback_ = index;
        if (l.seq > t_.seq && index - *since_ >= scan_) return end();
        return false;
    }
    bool end() { return decide(fallback_); }
    bool decided() const { return decided_; }
    std::optional<uint64_t> found() const { return found_; }

private:
    bool decide(std::optional<uint64_t> i) {
        decided_ = true;
        found_ = i;
        return true;
    }
    const OutputTarget& t_;
    std::size_t scan_;
    std::optional<uint64_t> since_, fallback_, found_;
    bool decided_ = false;
};

}  // namespace

std::optional<std::size_t> find_output_line(const std::vector<OutputLine>& lines, const OutputTarget& t, std::size_t scan) {
    TargetScan s(t, scan);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (s.add(lines[i], i)) break;
    }
    if (!s.decided()) s.end();
    if (!s.found()) return std::nullopt;
    return static_cast<std::size_t>(*s.found());
}

Result<HistoryOutput> load_history_output(const os::Paths& p, const std::string& id, const OutputWindowOptions& o) {
    if (id.empty() || id.size() > 64 || id.find_first_of("/\\.:") != std::string::npos)
        return make_error(Errc::invalid_argument, "not a session history id: " + id);
    const fs::path root = platform::paths::from_utf8(os::join(p.state, "history"));
    if (std::error_code ec; !fs::exists(root / store::kCatalogFile, ec)) return make_error(Errc::unavailable, "nothing is stored");
    auto cat = store::Catalog::open(root / store::kCatalogFile);
    if (!cat) return Unexpected(cat.error());

    const std::size_t max_lines = std::max<std::size_t>(o.max_lines, 1);
    const std::size_t max_bytes = std::max<std::size_t>(o.max_bytes, 1);
    // Before the target is known the window keeps up to the whole limit; once it is, the lines
    // before it are cut to half of it, and the rest is filled with the lines after.
    std::deque<OutputLine> win;
    std::size_t bytes = 0;
    uint64_t index = 0, before = 0, after = 0;   // `before`: stream index of win.front()
    std::optional<TargetScan> scan;
    if (o.around) scan.emplace(*o.around, 2000);
    std::optional<uint64_t> target;   // stream index
    bool full = false;                // the window took all it may after the target

    auto drop_front = [&] {
        bytes -= win.front().text.size();
        win.pop_front();
        ++before;
    };
    auto settle = [&] {
        target = scan->found();
        if (target && *target < before) target.reset();   // a fallback line the window already dropped
        if (!target) return;
        const std::size_t keep_lines = max_lines / 2, keep_bytes = max_bytes / 2;
        std::size_t lines_before = static_cast<std::size_t>(*target - before), bytes_before = 0;
        for (std::size_t i = 0; i < lines_before; ++i) bytes_before += win[i].text.size();
        while (lines_before > keep_lines || (lines_before > 0 && bytes_before > keep_bytes)) {
            bytes_before -= win.front().text.size();
            drop_front();
            --lines_before;
        }
    };

    auto r = store::for_each_stored_record(**cat, root, id, store::Kind::text, 0, [&](const store::RecordView& v) {
        const uint64_t i = index++;
        if (full) {
            ++after;
            return true;
        }
        OutputLine l{std::string(v.payload), v.seq, v.ts};
        const bool waiting = scan && !scan->decided();
        if (waiting && scan->add(l, i)) settle();
        if (target || !waiting || !scan) {
            // After the target, or no target (newest lines): the window is full when it is.
            const bool fits = win.size() < max_lines && bytes + l.text.size() <= max_bytes;
            if (target && !fits && i != *target) {
                full = true;
                ++after;
                return true;
            }
        }
        bytes += l.text.size();
        win.push_back(std::move(l));
        while (win.size() > max_lines || (win.size() > 1 && bytes > max_bytes)) {
            if (target && *target <= before) break;   // never the target itself
            drop_front();
        }
        return true;
    });
    if (!r) return Unexpected(r.error());
    if (scan && !scan->decided()) {
        scan->end();
        settle();
    }
    if (index == 0) return make_error(Errc::unavailable, "no output of this session is stored");

    HistoryOutput out;
    out.lines.assign(std::make_move_iterator(win.begin()), std::make_move_iterator(win.end()));
    out.lines_before = before;
    out.lines_after = after;
    if (target && *target >= before && *target - before < out.lines.size())
        out.target = static_cast<std::size_t>(*target - before);

    if (auto cmds = (*cat)->commands(id)) {
        for (const auto& c : *cmds) {
            OutputCommand oc;
            oc.started_ms = c.started_ms;
            oc.ended_ms = c.ended_ms;
            oc.exit_code = c.exit_code;
            oc.command = c.command;
            oc.cwd = c.cwd;
            oc.seq = c.seq;
            out.commands.push_back(std::move(oc));
        }
        std::sort(out.commands.begin(), out.commands.end(),
                  [](const OutputCommand& a, const OutputCommand& b) { return a.seq != b.seq ? a.seq < b.seq : a.started_ms < b.started_ms; });
        // Each command's line: the first line from its seq on that shows the command line.
        // Commands come in seq order and their lines too, so the scan goes on from the last one.
        std::size_t from = 0;
        for (auto& c : out.commands) {
            if (c.command.empty()) continue;
            while (from < out.lines.size() && out.lines[from].seq < c.seq) ++from;
            if (from >= out.lines.size()) break;
            const std::size_t end = std::min(out.lines.size(), from + 2000);
            for (std::size_t i = from; i < end; ++i) {
                if (out.lines[i].text.find(c.command) != std::string::npos) {
                    c.line = i;
                    from = i + 1;
                    break;
                }
            }
        }
    }
    return out;
}

}  // namespace kt::session
