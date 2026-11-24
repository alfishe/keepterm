#include "ui/resource_panel.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <cstdio>

#include "ui/form_widgets.h"
#include "ui/window.h"
#include "ui/workspace_widgets.h"

namespace kt::ui {

namespace {

constexpr float kRow = 22, kHeadH = 22, kIndent = 14, kPidW = 56, kMemW = 64, kCpuW = 48, kCtxW = 52, kTreeW = 360, kTreeWLarge = 520, kPanelW = 720, kPanelWLarge = 1000;
constexpr size_t kHistory = 90;   // samples per node (3 minutes at the 2 s interval)

float px_round(float v, float s) { return std::round(v * s) / s; }

uint64_t kind_key(ResourcePanel::Row::Kind k, uint64_t id) { return (static_cast<uint64_t>(k) << 60) ^ id; }

void flatten(const ProcItem& p, std::vector<ProcItem>& out) {
    ProcItem f = p;
    f.children.clear();
    out.push_back(std::move(f));
    for (const auto& c : p.children) flatten(c, out);
}

std::string duration_text(uint64_t ms) {
    const uint64_t s = ms / 1000;
    char buf[48];
    if (s < 60) std::snprintf(buf, sizeof buf, "%llus", static_cast<unsigned long long>(s));
    else if (s < 3600) std::snprintf(buf, sizeof buf, "%llum %llus", static_cast<unsigned long long>(s / 60), static_cast<unsigned long long>(s % 60));
    else if (s < 86400) std::snprintf(buf, sizeof buf, "%lluh %llum", static_cast<unsigned long long>(s / 3600), static_cast<unsigned long long>(s / 60 % 60));
    else std::snprintf(buf, sizeof buf, "%llud %lluh", static_cast<unsigned long long>(s / 86400), static_cast<unsigned long long>(s / 3600 % 24));
    return buf;
}

// A small sort triangle (rects, so it needs no glyph): pointing down when descending.
void sort_arrow(PaintContext& p, float x, float cy, bool down, Color c) {
    const float px = 1.0f / p.scale;
    for (int i = 0; i < 4; ++i) {
        const float w = static_cast<float>(7 - 2 * i);
        const float y = down ? cy - 2 + static_cast<float>(i) : cy + 1 - static_cast<float>(i);
        p.dl.rect(Rect{x + static_cast<float>(i), px_round(y, p.scale), w, std::max(px, 1.0f)}, c);
    }
}

std::string pane_name(const PaneItem& p) {
    if (!p.agent.empty()) return p.agent;
    if (!p.process.empty()) return p.process;
    return p.title.empty() ? std::string("shell") : p.title;
}

}  // namespace

std::string format_cpu(float pct) {
    char buf[24];
    if (pct < 0.05f) return "0%";
    if (pct < 10) std::snprintf(buf, sizeof buf, "%.1f%%", static_cast<double>(pct));
    else std::snprintf(buf, sizeof buf, "%.0f%%", static_cast<double>(pct));
    return buf;
}

// ---------------------------------------------------------------- the table

class ResourceTable final : public Widget {
public:
    explicit ResourceTable(ResourcePanel& p) : Widget("table"), panel_(p) {}
    void set_count(size_t n) {
        style().height = std::max<float>(kRow, static_cast<float>(n) * kRow);
        invalidate_paint();
    }
    void paint(PaintContext& p) const override {
        const Theme& t = p.theme;
        const Rect b = local_bounds();
        const FontRole body = role_for(t, TextStyle::body), small = role_for(t, TextStyle::small),
                       bold = role_for(t, TextStyle::body_bold);
        const auto& rows = panel_.rows();
        for (size_t i = 0; i < rows.size(); ++i) {
            const ResourcePanel::Row& r = rows[i];
            const float y = static_cast<float>(i) * kRow;
            if (r.key == panel_.selected()) p.dl.round_rect(Rect{2, y, b.w - 4, kRow}, 4, t.accent.with_alpha(0.22f));
            else if (hover_ && *hover_ == i) p.dl.round_rect(Rect{2, y, b.w - 4, kRow}, 4, t.fg.with_alpha(0.06f));
            const float cy = y + kRow / 2;
            float x = 8 + static_cast<float>(r.depth) * kIndent;
            if (r.has_children) p.dl.icon(r.collapsed ? Icon::chevron_right : Icon::chevron_down, 10, {x, px_round(cy - 5, p.scale)}, t.fg_dim);
            x += 14;
            Icon ic = Icon::none;
            switch (r.kind) {
                case ResourcePanel::Row::Kind::project: ic = Icon::folder; break;
                case ResourcePanel::Row::Kind::tab: ic = Icon::terminal; break;
                case ResourcePanel::Row::Kind::pane: ic = r.detail.find("agent") != std::string::npos ? Icon::agent : Icon::split_right; break;
                case ResourcePanel::Row::Kind::process: break;
            }
            if (ic != Icon::none) {
                p.dl.icon(ic, 12, {x, px_round(cy - 6, p.scale)}, t.fg_muted);
                x += 17;
            }
            const bool ctx = panel_.has_context();
            const float right_cols = kPidW + kMemW + kCpuW + (ctx ? kCtxW : 0) + 8;
            const float name_max = std::max(20.0f, b.w - right_cols - x - 8);
            const FontRole nr = r.kind == ResourcePanel::Row::Kind::process ? body : bold;
            // The name first, whole when it fits; then the role and the arguments in what is left.
            auto name = p.text.ellipsize(r.name, nr, p.scale, name_max);
            p.dl.text(name, {x, px_round(cy - name->height() / 2 + name->ascent(), p.scale)}, r.valid ? t.fg : t.fg_dim);
            float dx = x + name->width() + 6;
            for (const std::string* extra : {&r.detail, &r.args}) {
                if (extra->empty()) continue;
                const float dmax = x + name_max - dx;
                if (dmax <= 16) break;
                auto det = p.text.ellipsize(*extra, small, p.scale, dmax);
                p.dl.text(det, {dx, px_round(cy - det->height() / 2 + det->ascent(), p.scale)}, t.fg_dim);
                dx += det->width() + 8;
            }
            // Right-aligned figure columns: PID, Memory, CPU[, Context].
            const auto sort = panel_.sort();
            auto cell = [&](const std::string& v, float right, bool active) {
                if (v.empty()) return;
                auto run = p.text.shape(v, small, p.scale);
                p.dl.text(run, {px_round(right - 4 - run->width(), p.scale), px_round(cy - run->height() / 2 + run->ascent(), p.scale)},
                          active ? t.fg : t.fg_muted);
            };
            const std::string dash = "\xE2\x80\x94";
            float rx = b.w - 6;
            if (ctx) {
                cell(r.context >= 0 ? format_context_percent(r.context * 100) : std::string(), rx, sort == ResourcePanel::Sort::context);
                rx -= kCtxW;
            }
            cell(r.valid ? format_cpu(r.cpu) : dash, rx, sort == ResourcePanel::Sort::cpu);
            rx -= kCpuW;
            cell(r.valid ? format_bytes(r.memory) : dash, rx, sort == ResourcePanel::Sort::memory);
            rx -= kMemW;
            cell(r.pid ? std::to_string(r.pid) : std::string(), rx, sort == ResourcePanel::Sort::pid);
        }
        if (rows.empty()) {
            auto run = p.text.shape(std::string(tr("No figures yet")), small, p.scale);
            p.dl.text(run, {12, px_round(kRow / 2 - run->height() / 2 + run->ascent(), p.scale)}, t.fg_dim);
        }
    }
    EventResult on_pointer(PointerEvent& e) override {
        const auto& rows = panel_.rows();
        std::optional<size_t> at;
        if (e.local.y >= 0 && e.local.y < static_cast<float>(rows.size()) * kRow) at = static_cast<size_t>(e.local.y / kRow);
        if (e.type == PointerEvent::Type::Move || e.type == PointerEvent::Type::Enter) {
            if (at != hover_) {
                hover_ = at;
                set_tooltip(at ? rows[*at].tip : std::string());
                invalidate_paint();
            }
            return EventResult::Ignored;
        }
        if (e.type == PointerEvent::Type::Leave) {
            hover_.reset();
            invalidate_paint();
            return EventResult::Ignored;
        }
        if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left && at) {
            const auto& r = rows[*at];
            const float chevron_x = 8 + static_cast<float>(r.depth) * kIndent;
            if (r.has_children && (e.clicks >= 2 || (e.local.x >= chevron_x - 4 && e.local.x < chevron_x + 14))) {
                panel_.toggle(r.key);
            } else {
                panel_.select(r.key);
            }
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }
    EventResult on_key(const KeyEvent& k) override {
        if (k.action == platform::KeyAction::release) return EventResult::Ignored;
        const auto& rows = panel_.rows();
        size_t cur = rows.size();
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].key == panel_.selected()) cur = i;
        }
        if (k.key == platform::Key::down && cur + 1 < rows.size()) panel_.select(rows[cur + 1].key);
        else if (k.key == platform::Key::down && cur == rows.size() && !rows.empty()) panel_.select(rows.front().key);
        else if (k.key == platform::Key::up && cur < rows.size() && cur > 0) panel_.select(rows[cur - 1].key);
        else if ((k.key == platform::Key::left || k.key == platform::Key::right) && cur < rows.size() && rows[cur].has_children &&
                 rows[cur].collapsed == (k.key == platform::Key::right))
            panel_.toggle(rows[cur].key);
        else return EventResult::Ignored;
        return EventResult::Handled;
    }
    bool accepts_focus() const override { return true; }
    void on_hover(bool in) override {
        Widget::on_hover(in);
        if (!in) {
            hover_.reset();
            invalidate_paint();
        }
    }

private:
    ResourcePanel& panel_;
    std::optional<size_t> hover_;
};

// ---------------------------------------------------------------- column headers

// Name, PID, Memory, CPU[, Context]: a click sorts by the column, again flips the direction.
class ResourceHeader final : public Widget {
public:
    explicit ResourceHeader(ResourcePanel& p) : Widget("columns"), panel_(p) {
        style().height = kHeadH;
        style().shrink = 0;
    }
    struct Col {
        ResourcePanel::Sort sort;
        const char* label;
        float left, right;
        bool right_aligned;
    };
    std::vector<Col> cols() const {
        const float w = frame().w;
        std::vector<Col> out;
        float rx = w - 6 - 14;   // the tree's scroll bar gutter
        auto add = [&](ResourcePanel::Sort s, const char* l, float width) {
            out.push_back({s, l, rx - width, rx, true});
            rx -= width;
        };
        if (panel_.has_context()) add(ResourcePanel::Sort::context, "Context", kCtxW);
        add(ResourcePanel::Sort::cpu, "CPU", kCpuW);
        add(ResourcePanel::Sort::memory, "Memory", kMemW);
        add(ResourcePanel::Sort::pid, "PID", kPidW);
        out.push_back({ResourcePanel::Sort::name, "Name", 0, rx, false});
        return out;
    }
    void paint(PaintContext& p) const override {
        const Theme& t = p.theme;
        const Rect b = local_bounds();
        const FontRole f = role_for(t, TextStyle::small);
        const FontRole fb = role_for(t, TextStyle::small_bold);
        const auto cs = cols();
        for (size_t i = 0; i < cs.size(); ++i) {
            const Col& c = cs[i];
            const bool active = panel_.sort() == c.sort;
            if (hover_ == static_cast<int>(i)) p.dl.round_rect(Rect{c.left + 2, 1, c.right - c.left - 4, b.h - 2}, 4, t.fg.with_alpha(0.06f));
            auto run = p.text.shape(std::string(tr(c.label)), active ? fb : f, p.scale);
            const float y = px_round(b.h / 2 - run->height() / 2 + run->ascent(), p.scale);
            float x = c.right_aligned ? c.right - 4 - run->width() : 8 + 14;
            if (active && c.right_aligned) x -= 10;
            p.dl.text(run, {px_round(x, p.scale), y}, active ? t.fg : t.fg_muted);
            if (active) sort_arrow(p, x + run->width() + 3, b.h / 2, panel_.descending(), t.fg_muted);
        }
        p.dl.hairline(Rect{0, b.h - 1, b.w, 1}, t.border);
    }
    EventResult on_pointer(PointerEvent& e) override {
        const auto cs = cols();
        int at = -1;
        for (size_t i = 0; i < cs.size(); ++i)
            if (e.local.x >= cs[i].left && e.local.x < cs[i].right) at = static_cast<int>(i);
        if (e.type == PointerEvent::Type::Move || e.type == PointerEvent::Type::Enter) {
            if (at != hover_) {
                hover_ = at;
                set_tooltip(at >= 0 ? std::string(tr("Sort by ")) + std::string(tr(cs[static_cast<size_t>(at)].label)) : std::string());
                invalidate_paint();
            }
            return EventResult::Ignored;
        }
        if (e.type == PointerEvent::Type::Leave) {
            hover_ = -1;
            invalidate_paint();
            return EventResult::Ignored;
        }
        if (e.type == PointerEvent::Type::Press && e.button == platform::MouseButton::left && at >= 0) {
            panel_.set_sort(cs[static_cast<size_t>(at)].sort);
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }
    void on_hover(bool in) override {
        Widget::on_hover(in);
        if (!in) {
            hover_ = -1;
            invalidate_paint();
        }
    }
    std::string describe() const override {
        std::string d;
        for (const auto& c : cols()) {
            if (panel_.sort() == c.sort) d = std::string(c.label) + (panel_.descending() ? " desc" : " asc");
        }
        return d;
    }

private:
    ResourcePanel& panel_;
    int hover_ = -1;
};

namespace {
ResourcePanel::Page g_last_page = ResourcePanel::Page::processes;   // the page the popover reopens on
}

// ---------------------------------------------------------------- the panel

ResourcePanel::ResourcePanel(IWorkspaceView& view, ResourcePanelHooks hooks, ResourceScope scope)
    : Widget("resources"), view_(view), hooks_(std::move(hooks)), scope_(scope) {
    style().dir = LayoutStyle::Dir::Column;
    style().width = kPanelW;
    style().padding = Insets{8, 8, 8, 8};
    style().gap = 6;
    auto head = row("header", 6);
    head->style().align_items = LayoutStyle::Align::Center;
    head->style().padding = Insets{6, 0, 0, 0};
    head->emplace<IconView>(Icon::memory, 14.0f, Tone::muted);
    title_ = &head->emplace<Label>("", TextStyle::body_bold, Tone::normal, "title");
    title_->style().shrink = 1;
    total_ = &head->emplace<Label>("", TextStyle::small, Tone::muted, "total");
    head->add(spacer());
    auto cp = Button::icon_button(Icon::copy, std::string(tr("Copy as Text")), [this] {
        if (hooks_.copy) hooks_.copy(as_text());
    }, "copy", 22);
    head->add(std::move(cp));
    auto ex = Button::icon_button(Icon::maximize, std::string(tr("Larger")), [this] { set_expanded(!large_); }, "expand", 22);
    expand_ = &head->add(std::move(ex));
    add(std::move(head));
    auto seg = std::make_unique<SegmentedControl>(std::vector<std::string>{std::string(tr("Processes")), std::string(tr("Agent"))}, 0, "pages");
    seg->on_change = [this](size_t i) { set_page(i == 1 ? Page::agent : Page::processes); };
    pages_ = seg.get();
    auto seg_row = row("pages_row", 0);
    seg_row->add(std::move(seg));
    add(std::move(seg_row));
    auto body = row("body", 8);
    body->style().align_items = LayoutStyle::Align::Stretch;
    auto left = column("left", 0);
    left->style().shrink = 0;
    header_ = &left->emplace<ResourceHeader>(*this);
    header_->style().width = kTreeW;
    auto table = std::make_unique<ResourceTable>(*this);
    table_ = table.get();
    auto sv = std::make_unique<ScrollView>(std::move(table), "tree");
    sv->style().width = kTreeW;
    sv->style().height = 380.0f - kHeadH;
    sv->style().shrink = 0;
    scroll_ = &left->add(std::move(sv));
    body->add(std::move(left));
    auto det = column("detail", 4);
    det->style().grow = 1;
    det->style().shrink = 1;
    det->style().basis = 0.0f;
    det->style().padding = Insets{10, 4, 4, 4};
    detail_ = &body->add(std::move(det));
    body_ = &add(std::move(body));
    // The Agent page scrolls: several agents in one scope do not fit the popover's height.
    auto ag = column("agent", 4);
    ag->style().padding = Insets{6, 10, 8, 6};
    agent_ = ag.get();
    auto agv = std::make_unique<ScrollView>(std::move(ag), "agent_scroll");
    agv->style().height = 380.0f;
    agv->set_visible(false);
    agent_scroll_ = &add(std::move(agv));
    refresh();
    const Page first = scope_.page == 1 ? Page::processes : (scope_.page == 2 ? Page::agent : g_last_page);
    if (first == Page::agent) set_page(Page::agent);
    else g_last_page = Page::processes;
}

void ResourcePanel::on_attached() { fit_tree_width(); }

// The tree as wide as its widest row (indent, name, role, arguments up to a cap) plus the figure
// columns, at least the default width; the popover grows with it, within the window.
void ResourcePanel::fit_tree_width() {
    const float base_tree = large_ ? kTreeWLarge : kTreeW, base_panel = large_ ? kPanelWLarge : kPanelW;
    const float detail_min = base_panel - base_tree - 24;   // padding 8+8, gap 8
    float tree = base_tree;
    float panel = base_panel;
    if (Window* w = window()) {
        ITextService& tx = w->text();
        const Theme& th = w->theme();
        const float sc = w->scale();
        const FontRole body = role_for(th, TextStyle::body), small = role_for(th, TextStyle::small), bold = role_for(th, TextStyle::body_bold);
        float need = 0;
        for (const Row& r : rows_) {
            float x = 8 + static_cast<float>(r.depth) * kIndent + 14 + (r.kind == Row::Kind::process ? 0.0f : 17.0f);
            x += tx.shape(r.name, r.kind == Row::Kind::process ? body : bold, sc)->width();
            if (!r.detail.empty()) x += 6 + tx.shape(r.detail, small, sc)->width();
            if (!r.args.empty()) x += 8 + std::min(260.0f, tx.shape(r.args, small, sc)->width());
            need = std::max(need, x);
        }
        const float right = kPidW + kMemW + kCpuW + (has_context() ? kCtxW : 0) + 8 + 14 + 8;
        const float max_panel = std::max(base_panel, w->size().w - 48);
        tree = std::clamp(std::ceil(need + right), base_tree, std::max(base_tree, max_panel - detail_min - 24));
        panel = std::min(max_panel, std::max(base_panel, tree + detail_min + 24));
    }
    // Grows only while open: rows coming and going must not make the popover jump.
    if (tree_w_ > tree) {
        tree = tree_w_;
        panel = std::max(panel, tree + detail_min + 24);
    }
    if (tree == tree_w_ && cstyle().width && *cstyle().width == panel) return;
    tree_w_ = tree;
    style().width = panel;
    scroll_->style().width = tree;
    header_->style().width = tree;
    invalidate_layout();
}

void ResourcePanel::set_page(Page p) {
    page_ = p;
    g_last_page = p;
    if (pages_) pages_->set_selected(p == Page::agent ? 1 : 0);
    body_->set_visible(p == Page::processes);
    agent_scroll_->set_visible(p == Page::agent);
    agent_sig_.clear();
    if (p == Page::agent) {
        if (hooks_.refresh) hooks_.refresh();
        update_agent();
    }
    invalidate_layout();
    invalidate_paint();
}

std::vector<const PaneItem*> ResourcePanel::scope_agent_panes() const {
    const WorkspaceSnapshot& s = view_.snapshot();
    std::vector<const PaneItem*> out;
    auto take = [&](PaneKey k) {
        const PaneItem* p = s.pane(k);
        if (p && !p->agent.empty()) out.push_back(p);
    };
    auto tab_panes = [&](const TabItem& t) {
        for (PaneKey k : t.panes) take(k);
    };
    switch (scope_.kind) {
        case ResourceScope::Kind::pane: take(scope_.key); break;
        case ResourceScope::Kind::tab:
            if (const TabItem* t = s.tab(scope_.key)) tab_panes(*t);
            break;
        case ResourceScope::Kind::project:
            if (const ProjectItem* pr = s.project(scope_.key))
                for (const TabItem& t : pr->tabs) tab_panes(t);
            break;
        case ResourceScope::Kind::all:
            for (const ProjectItem& pr : s.projects)
                for (const TabItem& t : pr.tabs) tab_panes(t);
            break;
    }
    return out;
}

namespace {

std::string limit_name(const std::string& n, uint32_t minutes) {
    if (n == "five_hour" || minutes == 300) return std::string(tr("5-hour limit"));
    if (n == "seven_day" || minutes == 7 * 24 * 60) return std::string(tr("Weekly limit"));
    if (minutes) return std::to_string(minutes >= 60 ? minutes / 60 : minutes) + (minutes >= 60 ? "-hour limit" : "-minute limit");
    return n;
}

// The command that reopens the agent's session from a new terminal, in the syntax of the pane's
// shell: cmd.exe ("cd /d" and "&&"), PowerShell (";" between, single quotes), else POSIX sh.
std::string resume_command(const std::string& agent, const std::string& id, const std::string& cwd, const std::string& shell) {
    if (id.empty()) return {};
    std::string run;
    if (agent == "claude") run = "claude --resume " + id;
    else if (agent == "codex") run = "codex resume " + id;
    else if (agent == "gemini") run = "gemini --resume " + id;
    else if (agent == "qoder" || agent == "qodercli") run = "qodercli --resume " + id;
    else return {};
    if (cwd.empty()) return run;
    std::string sh = shell;
    for (char& c : sh) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    const bool win_path = (cwd.size() >= 2 && cwd[1] == ':') || cwd.find('\\') != std::string::npos;
    auto single_quoted = [](const std::string& v, const std::string& esc) {
        std::string o = "'";
        for (char c : v) o += c == '\'' ? esc : std::string(1, c);
        return o + "'";
    };
    if (sh.starts_with("powershell") || sh.starts_with("pwsh")) return "cd " + single_quoted(cwd, "''") + "; " + run;
    if (sh.starts_with("cmd") || (sh.empty() && win_path)) return "cd /d \"" + cwd + "\" && " + run;
    return "cd " + single_quoted(cwd, "'\\''") + " && " + run;
}

std::string clock_text(uint64_t unix_s) {
    if (!unix_s) return {};
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t d = static_cast<int64_t>(unix_s) - now;
    if (d <= 0) return std::string(tr("now"));
    char b[32];
    if (d < 3600) std::snprintf(b, sizeof b, "in %lldm", static_cast<long long>(d / 60));
    else if (d < 86400) std::snprintf(b, sizeof b, "in %lldh %lldm", static_cast<long long>(d / 3600), static_cast<long long>(d / 60 % 60));
    else std::snprintf(b, sizeof b, "in %lldd %lldh", static_cast<long long>(d / 86400), static_cast<long long>(d / 3600 % 24));
    return b;
}

std::string ago_text(uint64_t unix_ms) {
    if (!unix_ms) return {};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t d = (now - static_cast<int64_t>(unix_ms)) / 1000;
    if (d < 5) return std::string(tr("just now"));
    char b[32];
    if (d < 60) std::snprintf(b, sizeof b, "%llds ago", static_cast<long long>(d));
    else if (d < 3600) std::snprintf(b, sizeof b, "%lldm ago", static_cast<long long>(d / 60));
    else std::snprintf(b, sizeof b, "%lldh ago", static_cast<long long>(d / 3600));
    return b;
}

// The local time of day of `unix_ms` ("14:05"); with the date when it is not today.
std::string time_of_day(uint64_t unix_ms) {
    const std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
    const std::time_t now = std::time(nullptr);
    std::tm tm{}, tn{};
#ifdef _WIN32
    localtime_s(&tm, &t);
    localtime_s(&tn, &now);
#else
    localtime_r(&t, &tm);
    localtime_r(&now, &tn);
#endif
    char b[32];
    if (tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday) std::strftime(b, sizeof b, "%H:%M", &tm);
    else std::strftime(b, sizeof b, "%b %d %H:%M", &tm);
    return b;
}

// Limits older than this are shown dim: the agent has not answered for a while.
constexpr uint64_t kStaleLimitsMs = 60ull * 60 * 1000;

}  // namespace

void ResourcePanel::update_agent() {
    if (!agent_ || page_ != Page::agent) return;
    const auto panes = scope_agent_panes();
    // Rebuild only when a shown figure changes (the monitor ticks every second).
    std::string sig;
    for (const PaneItem* p : panes) {
        const AgentStats& a = p->agent_stats;
        sig += std::to_string(p->id) + p->agent + p->model + agent_state(*p) + p->cwd + std::to_string(a.used_tokens) + "/" +
               std::to_string(a.window_tokens) + "/" + std::to_string(a.input_tokens + a.output_tokens + a.cache_read_tokens) + "/" +
               std::to_string(a.turns) + "/" + std::to_string(a.limits.size()) + ago_text(a.updated_unix_ms) + ";";
        for (const auto& l : a.limits) sig += std::to_string(static_cast<int>(l.used_percent * 10)) + clock_text(l.resets_at_unix);
        sig += ago_text(a.limits_unix_ms);
    }
    if (sig == agent_sig_ && !sig.empty()) return;
    agent_sig_ = sig;
    agent_->clear_children();
    if (panes.empty()) {
        agent_->emplace<Label>(std::string(tr("No agent runs here.")), TextStyle::body, Tone::muted);
        auto& l = agent_->emplace<Label>(std::string(tr("Start Claude Code, Codex or another agent in a pane to see its model, context and limits.")),
                                         TextStyle::small, Tone::dim);
        l.set_wrap(true);
        return;
    }
    // key | value [| copy button]
    auto kv = [&](Box& into, const std::string& key, const std::string& value, Tone tone = Tone::normal, const std::string& copy = {}) {
        if (value.empty()) return;
        auto line = row({}, 8);
        line->style().align_items = LayoutStyle::Align::Center;
        line->style().min_h = 24.0f;
        auto& k = line->emplace<Label>(key, TextStyle::body, Tone::muted);
        k.style().width = 96.0f;
        k.style().shrink = 0;
        auto& v = line->emplace<Label>(value, TextStyle::body, tone);
        v.set_elide(Label::Elide::middle);
        v.style().shrink = 1;
        v.style().grow = 1;
        v.style().basis = 0.0f;
        v.set_tooltip(value);
        if (!copy.empty()) {
            line->add(Button::icon_button(Icon::copy, std::string(tr("Copy")) + ": " + copy, [this, copy] {
                if (hooks_.copy) hooks_.copy(copy);
            }, "copy_" + key, 20));
        }
        into.add(std::move(line));
    };
    auto section = [&](Box& into, const char* title) {
        auto& h = into.emplace<Label>(std::string(tr(title)), TextStyle::body_bold, Tone::muted);
        h.style().margin.t = 14;
        h.style().margin.b = 4;
    };
    // A meter: the name on the left and the figure on the right, a bar, a caption below. The
    // context and every limit use it, so they read alike.
    auto meter = [&](Box& into, const std::string& id, const std::string& name, const std::string& figure, Tone figure_tone,
                     float fraction, bool muted, const std::string& caption) {
        auto block = column(id, 5);
        block->style().margin.b = 10;
        block->style().shrink = 0;
        auto top = row({}, 8);
        top->style().align_items = LayoutStyle::Align::Center;
        top->emplace<Label>(name, TextStyle::body, Tone::normal);
        top->add(spacer());
        auto& f = top->emplace<Label>(figure, TextStyle::body_bold, figure_tone, id + "_figure");
        f.set_tooltip(figure);
        block->add(std::move(top));
        if (fraction >= 0) {
            auto& g = block->emplace<Gauge>(id + "_bar");
            g.set_fraction(std::clamp(fraction, 0.0f, 1.0f));
            g.set_thickness(8.0f);
            g.set_muted(muted);
            g.style().height = 8.0f;
        }
        if (!caption.empty()) {
            auto& c = block->emplace<Label>(caption, TextStyle::small, Tone::muted, id + "_caption");
            c.set_elide(Label::Elide::end);
            c.set_tooltip(caption);
        }
        into.add(std::move(block));
    };
    const ResourceTrees* trees = hooks_.trees ? &hooks_.trees() : nullptr;
    agent_->style().gap = 2;
    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    for (size_t i = 0; i < panes.size(); ++i) {
        const PaneItem& p = *panes[i];
        const AgentStats& a = p.agent_stats;
        if (i) {
            auto& gap = agent_->emplace<Widget>();
            gap.style().height = 18.0f;
        }
        // Header: the agent and its model, the state on the right.
        auto head = row({}, 8);
        head->style().align_items = LayoutStyle::Align::Center;
        head->style().margin.b = 2;
        head->emplace<IconView>(Icon::agent, 20.0f, Tone::accent);
        head->emplace<Label>(agent_title(p.agent), TextStyle::large_title, Tone::normal, "agent_name");
        head->add(spacer());
        head->emplace<Label>(agent_state(p), TextStyle::body, Tone::muted, "state");
        agent_->add(std::move(head));
        if (!p.model.empty()) {
            auto& m = agent_->emplace<Label>(p.model, TextStyle::body, Tone::muted, "model");
            m.set_elide(Label::Elide::end);
            m.set_tooltip(p.model);
        }

        // Context and limits: one meter each.
        section(*agent_, "Usage");
        const std::string line = context_line(a);
        if (line.empty()) {
            auto& none = agent_->emplace<Label>(std::string(tr("No figures yet: they come with the agent's first reply.")), TextStyle::body,
                                                Tone::dim, "context");
            none.set_wrap(true);
        } else {
            std::string figure = a.used_tokens ? format_tokens(a.used_tokens) : std::string("?");
            if (a.window_tokens) figure += " / " + format_tokens(a.window_tokens);
            std::string caption;
            if (a.percent >= 0) caption = format_context_percent(a.percent) + std::string(tr(" of the window"));
            if (a.after_compaction) caption += (caption.empty() ? "" : " \xC2\xB7 ") + std::string(tr("compacted; exact again after the next reply"));
            meter(*agent_, "context", std::string(tr("Context")), figure, Tone::normal, a.percent >= 0 ? a.percent / 100.0f : -1.0f, false,
                  caption);
        }
        const bool stale = a.limits_unix_ms && now_ms > a.limits_unix_ms + kStaleLimitsMs;
        for (size_t li = 0; li < a.limits.size(); ++li) {
            const auto& l = a.limits[li];
            char b[24];
            std::snprintf(b, sizeof b, "%.0f%% used", static_cast<double>(l.used_percent));
            const Tone tone = stale ? Tone::dim : (l.used_percent >= 90 ? Tone::failure : (l.used_percent >= 70 ? Tone::warning : Tone::normal));
            std::string caption;
            if (const std::string r = clock_text(l.resets_at_unix); !r.empty()) caption = std::string(tr("resets ")) + r;
            meter(*agent_, "limit_" + std::to_string(li), limit_name(l.name, l.window_minutes), b, tone, l.used_percent / 100.0f, stale,
                  caption);
        }
        if (!a.limits.empty()) {
            // The agent reports its limits with its replies: an idle account shows old figures.
            // A host of an older keepterm does not send the time.
            std::string v = a.limits_unix_ms ? std::string(tr("Limits as of ")) + time_of_day(a.limits_unix_ms) + " (" + ago_text(a.limits_unix_ms) + ")"
                                             : std::string(tr("Limits: age unknown (from the last reply of the agent)"));
            auto& asof = agent_->emplace<Label>(v, TextStyle::small, stale ? Tone::warning : Tone::dim, "limits_age");
            asof.set_wrap(true);
        }

        // Token totals and turns.
        if (a.input_tokens || a.output_tokens || a.cache_read_tokens || a.cache_write_tokens) {
            std::string t = std::string(tr("in ")) + format_tokens(a.input_tokens);
            if (a.cache_read_tokens || a.cache_write_tokens)
                t += " \xC2\xB7 " + std::string(tr("cache ")) + format_tokens(a.cache_read_tokens) + std::string(tr(" read, ")) +
                     format_tokens(a.cache_write_tokens) + std::string(tr(" written"));
            t += " \xC2\xB7 " + std::string(tr("out ")) + format_tokens(a.output_tokens);
            kv(*agent_, std::string(tr("Tokens")), t);
        }
        std::string use;
        if (a.turns) use = std::to_string(a.turns) + (a.turns == 1 ? std::string(tr(" turn")) : std::string(tr(" turns")));
        if (a.compactions)
            use += (use.empty() ? "" : " \xC2\xB7 ") + std::to_string(a.compactions) +
                   (a.compactions == 1 ? std::string(tr(" compaction")) : std::string(tr(" compactions")));
        if (a.cost_usd) {
            char b[32];
            std::snprintf(b, sizeof b, "$%.2f", *a.cost_usd);
            use += (use.empty() ? "" : " \xC2\xB7 ") + std::string(b);
        }
        kv(*agent_, std::string(tr("Activity")), use);

        // The session: what it is called, where it runs, how to get back to it.
        section(*agent_, "Session");
        const std::string cwd = !a.session_cwd.empty() ? a.session_cwd : p.cwd;
        kv(*agent_, std::string(tr("Name")), a.session_name);
        kv(*agent_, std::string(tr("Session id")), a.session_id, Tone::normal, a.session_id);
        kv(*agent_, std::string(tr("Folder")), cwd, Tone::normal, cwd);
        std::string shell;
        if (trees) {
            if (auto it = trees->find(p.id); it != trees->end() && !it->second.roots.empty()) shell = it->second.roots.front().name;
        }
        const std::string resume = resume_command(p.agent, a.session_id, cwd, shell);
        kv(*agent_, std::string(tr("Resume")), resume, Tone::muted, resume);
    }
}

void ResourcePanel::set_sort(Sort s) {
    if (s == sort_) desc_ = !desc_;
    else desc_ = s != Sort::name && s != Sort::pid;
    sort_ = s;
    header_->invalidate_paint();
    refresh();
}

bool ResourcePanel::has_context() const {
    for (const Row& r : rows_)
        if (r.context >= 0) return true;
    return false;
}

bool ResourcePanel::before(const std::string& an, uint32_t ap, uint64_t am, float ac, float ax, const std::string& bn, uint32_t bp,
                           uint64_t bm, float bc, float bx) const {
    int c = 0;
    auto cmp = [](auto a, auto b) { return a < b ? -1 : (b < a ? 1 : 0); };
    switch (sort_) {
        case Sort::name: {
            std::string a = an, b = bn;
            for (char& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (char& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            c = cmp(a, b);
            break;
        }
        case Sort::pid: c = cmp(ap, bp); break;
        case Sort::memory: c = cmp(am, bm); break;
        case Sort::cpu: c = cmp(ac, bc); break;
        case Sort::context: c = cmp(ax, bx); break;
    }
    return desc_ ? c > 0 : c < 0;
}

void ResourcePanel::toggle(uint64_t key) {
    if (!collapsed_.erase(key)) collapsed_.insert(key);
    refresh();
}

void ResourcePanel::add_proc(const ProcItem& p, int depth, std::vector<Row>& out) {
    Row r;
    r.kind = Row::Kind::process;
    r.depth = depth;
    r.key = kind_key(Row::Kind::process, (static_cast<uint64_t>(p.pid) << 20) ^ p.start);
    r.name = p.name.empty() ? std::string("?") : p.name;
    r.pid = p.pid;
    r.detail = p.role;
    // The arguments after the program (the command line's first word), so that several
    // "python.exe" rows tell apart.
    if (!p.cmdline.empty()) {
        size_t i = 0;
        if (p.cmdline[0] == '"') {
            const size_t q = p.cmdline.find('"', 1);
            i = q == std::string::npos ? p.cmdline.size() : q + 1;
        } else {
            i = p.cmdline.find(' ');
            if (i == std::string::npos) i = p.cmdline.size();
        }
        while (i < p.cmdline.size() && p.cmdline[i] == ' ') ++i;
        r.args = p.cmdline.substr(i);
        if (r.args.size() > 400) r.args.resize(400);
    }
    if (p.no_access) r.detail += (r.detail.empty() ? "" : "  ") + std::string(tr("no access"));
    r.tip = p.tip;
    r.memory = p.children.empty() ? p.memory : p.subtree_memory;
    r.cpu = p.children.empty() ? p.cpu : p.subtree_cpu;
    r.has_children = !p.children.empty();
    r.collapsed = collapsed_.count(r.key) != 0;
    r.killable = p.killable && p.pid != 0;
    r.proc = p;
    r.proc.children.clear();
    if (r.has_children) {
        // Kill-tree needs the pids below; keep them flat in the row's copy.
        std::vector<const ProcItem*> stack;
        for (const auto& c : p.children) stack.push_back(&c);
        while (!stack.empty()) {
            const ProcItem* c = stack.back();
            stack.pop_back();
            ProcItem flat = *c;
            flat.children.clear();
            r.proc.children.push_back(std::move(flat));
            for (const auto& cc : c->children) stack.push_back(&cc);
        }
    }
    out.push_back(std::move(r));
    if (out.back().collapsed) return;
    std::vector<const ProcItem*> kids;
    for (const auto& c : p.children) kids.push_back(&c);
    std::stable_sort(kids.begin(), kids.end(), [this](const ProcItem* a, const ProcItem* b) {
        return before(a->name, a->pid, a->subtree_memory, a->subtree_cpu, -1, b->name, b->pid, b->subtree_memory, b->subtree_cpu, -1);
    });
    for (const ProcItem* c : kids) add_proc(*c, depth + 1, out);
}

void ResourcePanel::refresh() {
    static const ResourceTrees kEmpty;
    const ResourceTrees& trees = hooks_.trees ? hooks_.trees() : kEmpty;
    const WorkspaceSnapshot& s = view_.snapshot();
    std::vector<Row> out;
    uint64_t total_mem = 0;
    float total_cpu = 0;
    bool any = false;
    std::string scope_name = std::string(tr("All Sessions"));

    // A pane: a header row with its processes below, or (header = false: the pane is the only
    // one of its tab, or the scope itself) its processes alone at `depth`.
    auto pane_rows = [&](const PaneItem& pi, int depth, std::vector<Row>& dst, bool header, Row* info) {
        Row r;
        r.kind = Row::Kind::pane;
        r.depth = depth;
        r.key = kind_key(Row::Kind::pane, pi.id);
        r.name = pane_name(pi);
        r.pane = pi.id;
        r.context = pi.context;
        auto it = trees.find(pi.id);
        if (it != trees.end()) {
            const SessionTree& st = it->second;
            r.memory = st.memory;
            r.cpu = st.cpu;
            r.detail = st.containment;
            r.tip = std::string(tr("Session ")) + st.session + " \xC2\xB7 " + st.containment + " \xC2\xB7 " + st.accuracy;
            r.has_children = !st.roots.empty();
            r.containment = st.containment + " (" + st.accuracy + ")";
            if (st.overhead_processes)
                r.overhead = std::to_string(st.overhead_processes) + " " + std::string(tr("processes")) + ", " + format_bytes(st.overhead_memory) +
                             ", " + format_cpu(st.overhead_cpu) + " " + std::string(tr("CPU"));
            for (const auto& root_proc : st.roots) flatten(root_proc, r.procs);
        } else {
            r.valid = pi.res.valid;
            r.memory = pi.res.memory;
            r.cpu = pi.res.cpu;
        }
        if (info) {
            info->pane = pi.id;
            info->context = pi.context;
            info->containment = r.containment;
            info->overhead = r.overhead;
            if (info->detail.empty()) info->detail = r.detail;
        }
        r.collapsed = header && collapsed_.count(r.key) != 0;
        const std::vector<ProcItem> no_roots;
        const std::vector<ProcItem>& roots = it != trees.end() ? it->second.roots : no_roots;
        if (header) dst.push_back(r);
        else dst.push_back(Row{});   // placeholder carrying the flat process list, removed below
        dst.back().procs = r.procs;
        const size_t mark = dst.size() - 1;
        if (!r.collapsed) {
            std::vector<const ProcItem*> sorted;
            for (const auto& x : roots) sorted.push_back(&x);
            std::stable_sort(sorted.begin(), sorted.end(), [this](const ProcItem* a, const ProcItem* b) {
                return before(a->name, a->pid, a->subtree_memory, a->subtree_cpu, -1, b->name, b->pid, b->subtree_memory, b->subtree_cpu, -1);
            });
            for (const ProcItem* x : sorted) add_proc(*x, header ? depth + 1 : depth, dst);
        }
        std::vector<ProcItem> flat = std::move(dst[mark].procs);
        if (!header) dst.erase(dst.begin() + static_cast<std::ptrdiff_t>(mark));
        else dst[mark].procs = flat;
        return std::make_tuple(r.memory, r.cpu, std::move(flat));
    };

    struct Group {
        Row head;
        std::vector<Row> rows;
    };
    auto tab_group = [&](const TabItem& t, int depth) {
        Group g;
        g.head.kind = Row::Kind::tab;
        g.head.depth = depth;
        g.head.key = kind_key(Row::Kind::tab, t.id);
        g.head.name = t.title.empty() ? std::string(tr("Shell")) : t.title;
        g.head.collapsed = collapsed_.count(g.head.key) != 0;
        g.head.has_children = !t.panes.empty();
        struct PaneGroup {
            std::string name;
            uint64_t memory;
            float cpu, context;
            std::vector<Row> rows;
        };
        std::vector<PaneGroup> panes;
        const bool single = t.panes.size() == 1;   // the pane level would repeat the tab
        for (PaneKey k : t.panes) {
            const PaneItem* pi = s.pane(k);
            if (!pi) continue;
            std::vector<Row> rows;
            auto [m, c, flat] = pane_rows(*pi, depth + 1, rows, !single, single ? &g.head : nullptr);
            g.head.procs.insert(g.head.procs.end(), flat.begin(), flat.end());
            g.head.memory += m;
            g.head.cpu += c;
            panes.push_back({pane_name(*pi), m, c, pi->context, std::move(rows)});
        }
        std::stable_sort(panes.begin(), panes.end(), [this](const PaneGroup& a, const PaneGroup& b) {
            return before(a.name, 0, a.memory, a.cpu, a.context, b.name, 0, b.memory, b.cpu, b.context);
        });
        if (!g.head.collapsed) {
            for (auto& pg : panes) g.rows.insert(g.rows.end(), pg.rows.begin(), pg.rows.end());
        }
        if (t.panes.size() > 1) g.head.detail = std::to_string(t.panes.size()) + " " + std::string(tr("panes"));
        return g;
    };
    auto emit = [&](std::vector<Group>& groups) {
        std::stable_sort(groups.begin(), groups.end(), [this](const Group& a, const Group& b) {
            return before(a.head.name, 0, a.head.memory, a.head.cpu, a.head.context, b.head.name, 0, b.head.memory, b.head.cpu, b.head.context);
        });
        for (auto& g : groups) {
            out.push_back(g.head);
            out.insert(out.end(), g.rows.begin(), g.rows.end());
        }
    };

    if (scope_.kind == ResourceScope::Kind::pane) {
        if (const PaneItem* pi = s.pane(scope_.key)) {
            scope_name = pane_name(*pi);
            auto [m, c, flat] = pane_rows(*pi, 0, out, false, nullptr);
            (void)flat;
            total_mem = m;
            total_cpu = c;
            any = true;
        }
    } else if (scope_.kind == ResourceScope::Kind::tab) {
        if (const TabItem* t = s.tab(scope_.key)) {
            scope_name = t->title.empty() ? std::string(tr("Tab")) : t->title;
            Group g = tab_group(*t, 0);
            total_mem = g.head.memory;
            total_cpu = g.head.cpu;
            out.insert(out.end(), g.rows.begin(), g.rows.end());
            any = true;
        }
    } else {
        std::vector<Group> projects;
        for (const ProjectItem& p : s.projects) {
            if (scope_.kind == ResourceScope::Kind::project && p.id != scope_.key) continue;
            Group pg;
            pg.head.kind = Row::Kind::project;
            pg.head.key = kind_key(Row::Kind::project, p.id);
            pg.head.name = p.name;
            pg.head.has_children = !p.tabs.empty();
            pg.head.collapsed = collapsed_.count(pg.head.key) != 0;
            pg.head.detail = std::to_string(p.tabs.size()) + " " + std::string(p.tabs.size() == 1 ? tr("tab") : tr("tabs"));
            std::vector<Group> tabs;
            for (const TabItem& t : p.tabs) {
                Group g = tab_group(t, scope_.kind == ResourceScope::Kind::project ? 0 : 1);
                pg.head.memory += g.head.memory;
                pg.head.cpu += g.head.cpu;
                pg.head.procs.insert(pg.head.procs.end(), g.head.procs.begin(), g.head.procs.end());
                tabs.push_back(std::move(g));
            }
            if (scope_.kind == ResourceScope::Kind::project) {
                scope_name = p.name;
                total_mem = pg.head.memory;
                total_cpu = pg.head.cpu;
                any = true;
                emit(tabs);
                continue;
            }
            std::stable_sort(tabs.begin(), tabs.end(), [this](const Group& a, const Group& b) {
                return before(a.head.name, 0, a.head.memory, a.head.cpu, a.head.context, b.head.name, 0, b.head.memory, b.head.cpu, b.head.context);
            });
            if (!pg.head.collapsed) {
                for (auto& g : tabs) {
                    pg.rows.push_back(g.head);
                    pg.rows.insert(pg.rows.end(), g.rows.begin(), g.rows.end());
                }
            }
            total_mem += pg.head.memory;
            total_cpu += pg.head.cpu;
            any = true;
            projects.push_back(std::move(pg));
        }
        if (scope_.kind == ResourceScope::Kind::all) emit(projects);
    }
    rows_ = std::move(out);
    title_->set_text(scope_name);
    total_->set_text(any ? format_bytes(total_mem) + " \xC2\xB7 " + format_cpu(total_cpu) + " " + std::string(tr("CPU")) : std::string());
    table_->set_count(rows_.size());
    if (!selected_row() && !rows_.empty()) selected_ = rows_.front().key;
    update_detail();
    update_agent();
    fit_tree_width();
    invalidate_layout();
    invalidate_paint();
}

const ResourcePanel::Row* ResourcePanel::selected_row() const {
    for (const Row& r : rows_) {
        if (r.key == selected_) return &r;
    }
    return nullptr;
}

void ResourcePanel::select(uint64_t key) {
    if (key == selected_) return;
    selected_ = key;
    update_detail();
    table_->invalidate_paint();
}

void ResourcePanel::tick() {
    refresh();
    for (const Row& r : rows_) {
        auto& m = mem_hist_[r.key];
        auto& c = cpu_hist_[r.key];
        m.push_back(static_cast<float>(r.memory));
        c.push_back(r.cpu);
        if (m.size() > kHistory) m.erase(m.begin());
        if (c.size() > kHistory) c.erase(c.begin());
    }
    update_detail();
}

void ResourcePanel::set_expanded(bool large) {
    large_ = large;
    scroll_->style().height = (large ? 560.0f : 380.0f) - kHeadH;
    agent_scroll_->style().height = large ? 560.0f : 380.0f;
    tree_w_ = 0;
    fit_tree_width();
    expand_->set_icon(large ? Icon::minimize : Icon::maximize, 13);
    expand_->set_tooltip(std::string(large ? tr("Smaller") : tr("Larger")));
    invalidate_layout();
}

void ResourcePanel::update_detail() {
    detail_->clear_children();
    const Row* r = selected_row();
    if (!r) {
        detail_->emplace<Label>(std::string(tr("Select a row")), TextStyle::small, Tone::dim);
        return;
    }
    auto& title = detail_->emplace<Label>(r->name, TextStyle::title, Tone::normal, "name");
    title.set_elide(Label::Elide::middle);
    std::string sub;
    switch (r->kind) {
        case Row::Kind::project: sub = std::string(tr("Project")); break;
        case Row::Kind::tab: sub = std::string(tr("Tab")); break;
        case Row::Kind::pane: sub = std::string(tr("Pane")); break;
        case Row::Kind::process: sub = std::string(tr("Process ")) + std::to_string(r->proc.pid) + (r->proc.role.empty() ? "" : " \xC2\xB7 " + r->proc.role); break;
    }
    detail_->emplace<Label>(sub, TextStyle::small, Tone::muted, "kind");

    auto chart = [&](const char* label, const std::string& value, const std::vector<float>& hist, Tone tone, std::optional<float> max) {
        auto line = row({}, 6);
        line->style().align_items = LayoutStyle::Align::Center;
        line->style().margin.t = 6;
        auto& l = line->emplace<Label>(std::string(tr(label)), TextStyle::small, Tone::muted);
        l.style().width = 64.0f;
        auto& v = line->emplace<Label>(value, TextStyle::body_bold, Tone::normal, "value");
        v.style().width = 70.0f;
        auto& sp = line->emplace<Sparkline>(kHistory, "chart");
        sp.style().grow = 1;
        sp.style().height = 28.0f;
        sp.set_tone(tone);
        sp.set_max(max);
        sp.set_values(hist);
        detail_->add(std::move(line));
    };
    static const std::vector<float> kNone;
    auto mh = mem_hist_.find(r->key);
    auto ch = cpu_hist_.find(r->key);
    chart("Memory", r->valid ? format_bytes(r->memory) : std::string("\xE2\x80\x94"), mh == mem_hist_.end() ? kNone : mh->second, Tone::accent,
          std::nullopt);
    chart("CPU", r->valid ? format_cpu(r->cpu) : std::string("\xE2\x80\x94"), ch == cpu_hist_.end() ? kNone : ch->second, Tone::success, 100.0f);

    auto kv = [&](const char* key, const std::string& value) {
        if (value.empty()) return;
        auto line = row({}, 6);
        auto& k = line->emplace<Label>(std::string(tr(key)), TextStyle::small, Tone::muted);
        k.style().width = 110.0f;
        k.style().shrink = 0;
        auto& v = line->emplace<Label>(value, TextStyle::small, Tone::normal);
        v.set_elide(Label::Elide::middle);
        v.style().shrink = 1;
        v.style().grow = 1;
        v.style().basis = 0.0f;
        v.set_tooltip(value);
        detail_->add(std::move(line));
    };
    auto sep = [&] {
        auto& s2 = detail_->emplace<Widget>();
        s2.style().height = 6.0f;
    };
    sep();
    if (r->kind == Row::Kind::process) {
        const ProcItem& p = r->proc;
        // "Memory" is the figure of the tree: physical memory the process alone uses (private
        // working set, footprint, PSS). Committed is address space, not memory in use.
        kv("Own memory", format_bytes(p.memory));
        if (p.resident) kv("Resident (RSS)", format_bytes(p.resident));
        if (p.private_resident && p.private_resident != p.memory) kv("Private resident", format_bytes(p.private_resident));
        if (p.resident > p.memory) kv("Shared (approx.)", format_bytes(p.resident - p.memory));
        if (p.peak) kv("Peak resident", format_bytes(p.peak));
        if (p.committed) kv("Committed", format_bytes(p.committed));
        if (!r->proc.children.empty()) kv("With children", format_bytes(p.subtree_memory) + " \xC2\xB7 " + format_cpu(p.subtree_cpu));
        if (p.cpu_ns) kv("CPU time (user+sys)", duration_text(p.cpu_ns / 1000000));
        if (p.threads) kv("Threads", std::to_string(p.threads));
        if (p.start_unix_ms) {
            const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch())
                                                       .count());
            if (now > p.start_unix_ms) kv("Running for", duration_text(now - p.start_unix_ms));
        }
        kv("Command", p.cmdline);
        kv("Folder", p.cwd);
        if (p.no_access) kv("Access", std::string(tr("denied: figures are partial")));
        if (r->killable) {
            auto btns = row("actions", 6);
            btns->style().margin.t = 8;
            const std::string id = std::to_string(r->key);
            btns->add(std::make_unique<Button>(std::string(tr("Kill")), [this, k = r->key] {
                for (size_t i = 0; i < rows_.size(); ++i) {
                    if (rows_[i].key == k) confirm_kill(i, false);
                }
            }, Button::Kind::danger, "kill"));
            if (!r->proc.children.empty()) {
                btns->add(std::make_unique<Button>(std::string(tr("Kill Tree")), [this, k = r->key] {
                    for (size_t i = 0; i < rows_.size(); ++i) {
                        if (rows_[i].key == k) confirm_kill(i, true);
                    }
                }, Button::Kind::danger, "kill_tree"));
            }
            detail_->add(std::move(btns));
        }
        return;
    }
    // The pane's foreground program, its state and the agent's context (moved here from the
    // title bar).
    if (const PaneItem* pi = r->pane ? view_.snapshot().pane(r->pane) : nullptr) {
        std::string prog = !pi->agent.empty() ? pi->agent : (!pi->process.empty() ? pi->process : std::string(tr("shell")));
        if (pi->pid) prog += "  \xC2\xB7  " + std::to_string(pi->pid);
        kv("Program", prog);
        if (!pi->agent.empty()) kv("State", agent_state(*pi));
        else if (pi->exited) kv("State", std::string(tr("exited")));
        if (pi->uptime_s) kv(pi->agent.empty() ? "Uptime" : "Agent uptime", format_uptime(pi->uptime_s));
        if (!pi->agent.empty()) {
            const std::string cl = context_line(pi->agent_stats);
            kv("Agent context", cl.empty() ? std::string(tr("no figures yet")) : cl + "  \xC2\xB7  " + std::string(tr("details on the Agent tab")));
        }
        kv("Folder", pi->cwd);
    }
    kv("Processes", std::to_string(r->procs.size()));
    if (!r->containment.empty()) kv("Containment", r->containment);
    if (!r->overhead.empty()) kv("keepterm overhead", r->overhead);
    uint64_t res = 0;
    uint32_t threads = 0;
    for (const ProcItem& p : r->procs) {
        res += p.resident;
        threads += p.threads;
    }
    if (res) kv("Resident (RSS)", format_bytes(res));
    if (threads) kv("Threads", std::to_string(threads));
    auto top = [&](const char* label, bool by_cpu) {
        if (r->procs.empty()) return;
        std::vector<const ProcItem*> v;
        for (const ProcItem& p : r->procs) v.push_back(&p);
        std::stable_sort(v.begin(), v.end(), [by_cpu](const ProcItem* a, const ProcItem* b) { return by_cpu ? a->cpu > b->cpu : a->memory > b->memory; });
        auto& h = detail_->emplace<Label>(std::string(tr(label)), TextStyle::small_bold, Tone::muted);
        h.style().margin.t = 8;
        for (size_t i = 0; i < v.size() && i < 4; ++i) {
            kv(v[i]->name.c_str(), (by_cpu ? format_cpu(v[i]->cpu) : format_bytes(v[i]->memory)) + "  \xC2\xB7  " + std::to_string(v[i]->pid));
        }
    };
    top("Top by memory", false);
    top("Top by CPU", true);
}

std::string ResourcePanel::as_text() const {
    std::string out = title_->text() + "  " + total_->text() + "\n";
    for (const Row& r : rows_) {
        std::string line(static_cast<size_t>(r.depth) * 2, ' ');
        line += r.name;
        if (!r.detail.empty()) line += " (" + r.detail + ")";
        if (line.size() < 48) line.resize(48, ' ');
        char buf[64];
        std::snprintf(buf, sizeof buf, " %10s %7s", r.valid ? format_bytes(r.memory).c_str() : "-", r.valid ? format_cpu(r.cpu).c_str() : "-");
        out += line + buf + "\n";
    }
    return out;
}

void ResourcePanel::confirm_kill(size_t row, bool tree) {
    if (row >= rows_.size() || !window()) return;
    const Row& r = rows_[row];
    ProcItem target = r.proc;
    DialogSpec d;
    d.title = (tree ? std::string(tr("Kill \xE2\x80\x9C")) + r.name + std::string(tr("\xE2\x80\x9D and its children?"))
                    : std::string(tr("Kill \xE2\x80\x9C")) + r.name + "\xE2\x80\x9D?");
    d.message = std::string(tr("Process ")) + std::to_string(target.pid) +
                (tree ? " " + std::string(tr("and ")) + std::to_string(target.children.size()) + " " + std::string(tr("more")) : std::string()) +
                ". " + std::string(tr("Unsaved work in it is lost."));
    d.buttons.push_back({std::string(tr("Cancel")), {}, Button::Kind::secondary, false, true});
    d.buttons.push_back({std::string(tr("Kill")),
                         // The panel may be gone when the dialog answers: keep the hook, not `this`.
                         [kill = hooks_.kill, target, tree] {
                             if (kill) kill(target, tree);
                         },
                         Button::Kind::danger, true, false});
    show_dialog(*window(), std::move(d));
}

void ResourcePanel::paint(PaintContext& p) const {
    const Theme& t = p.theme;
    const Rect b = local_bounds();
    p.dl.shadow(Rect{b.x, b.y + 4, b.w, b.h}, 10, 10, t.shadow);
    p.dl.round_rect(b, 10, over(t.fg.with_alpha(t.scheme == Scheme::dark ? 0.07f : 0.0f), t.bg), 1,
                    t.fg.with_alpha(t.scheme == Scheme::dark ? 0.14f : 0.12f));
}

std::string ResourcePanel::describe() const { return std::to_string(rows_.size()) + " rows"; }

void split_session_tree(ProcItem n, SessionTree& out) {
    if (!n.keepterm) {
        out.memory += n.subtree_memory;
        out.cpu += n.subtree_cpu;
        out.roots.push_back(std::move(n));
        return;
    }
    out.overhead_memory += n.memory;
    out.overhead_cpu += n.cpu;
    ++out.overhead_processes;
    for (auto& c : n.children) split_session_tree(std::move(c), out);
}

WidgetId show_resource_panel(Window& w, IWorkspaceView& view, ResourcePanelHooks hooks, ResourceScope scope, Rect anchor, bool above) {
    w.close_transient();
    auto panel = std::make_unique<ResourcePanel>(view, std::move(hooks), scope);
    OverlayOptions o;
    o.anchor = anchor;
    o.placement = above ? OverlayOptions::Placement::Above : OverlayOptions::Placement::Below;
    o.modal = false;
    o.transient = true;
    o.take_focus = false;
    return w.show_overlay(std::move(panel), o);
}

}  // namespace kt::ui
