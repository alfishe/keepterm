#include "app/history_controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "agents/registry.h"
#include "ui/form_widgets.h"
#include "ui/window.h"
#include "ui/workspace_widgets.h"

namespace kt::app {

using namespace kt::ui;
using history::Group;
using history::Item;
using history::Sort;
using history::State;

namespace {

constexpr float kRowH = 42;
constexpr float kRowSearchH = 60;          // a third line: what the search found
constexpr size_t kMinOutputText = 3;       // shorter text: no output search
constexpr size_t kNoCount = static_cast<size_t>(-1);
constexpr float kFilterW = 200;
constexpr float kDetailsW = 330;
constexpr float kDetailsMinWindow = 900;   // below it the details column folds away

const std::array<std::pair<Group, const char*>, 5> kGroups = {
    {{Group::day, "Day"}, {Group::project, "Project"}, {Group::agent, "Agent"}, {Group::tag, "Tag"}, {Group::none, "None"}}};
const std::array<std::pair<Sort, const char*>, 5> kSorts = {{{Sort::last_used, "Last used"},
                                                             {Sort::created, "Created"},
                                                             {Sort::duration, "Duration"},
                                                             {Sort::size, "Size"},
                                                             {Sort::commands, "Commands"}}};

std::string tr_s(std::string_view s) { return std::string(tr(s)); }

std::optional<ProjectColor> project_color(const std::string& name) {
    static constexpr std::array<const char*, 10> kNames = {"none", "red",  "orange", "yellow", "green",
                                                           "teal", "blue", "purple", "pink",   "gray"};
    for (size_t i = 1; i < kNames.size(); ++i) {
        if (name == kNames[i]) return static_cast<ProjectColor>(i);
    }
    return std::nullopt;
}

std::string project_color_of(const Item& it) {
    return it.e.project.kind() == session::json::Value::Kind::Object ? it.e.project.str_or("color") : std::string();
}

std::string state_text(const Item& it, uint64_t now) {
    switch (it.state) {
        case State::live: return tr_s("live");
        case State::background: return tr_s("running, no tab");
        case State::closed: return tr_s("closed") + " " + history::ago_text(it.e.ended_ms.value_or(it.e.last_used_ms), now);
    }
    return {};
}

std::string commands_text(uint64_t n) { return std::to_string(n) + (n == 1 ? " cmd" : " cmds"); }

// The second line of a row: time span, duration, commands, stored size.
std::string row_detail(const Item& it, const history::Clock& c, uint64_t now) {
    std::string s = history::clock_text(c, it.e.created_ms) + " \xE2\x86\x92 ";
    if (it.state != State::closed) s += tr_s("now");
    else s += history::clock_text(c, it.e.ended_ms.value_or(it.e.last_used_ms));
    s += " \xC2\xB7 " + history::duration_text(history::duration_ms(it, now));
    if (it.e.stats.commands) s += " \xC2\xB7 " + commands_text(it.e.stats.commands);
    if (it.e.stats.stored_bytes) s += " \xC2\xB7 " + format_bytes(it.e.stats.stored_bytes);
    return s;
}

std::string agent_label(const std::string& id) {
    if (id == history::kAnyAgent) return tr_s("Any agent");
    static const agents::AgentRegistry reg = agents::AgentRegistry::builtin();
    if (const auto* d = reg.find(id)) return std::string(d->display_name);
    if (id.empty()) return id;
    std::string s = id;
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::unique_ptr<Label> label(std::string text, TextStyle st = TextStyle::body, Tone tone = Tone::normal, std::string id = {}) {
    auto l = std::make_unique<Label>(std::move(text), st, tone, std::move(id));
    l->style().shrink = 1;
    return l;
}

std::unique_ptr<Label> heading(std::string text) {
    auto l = label(std::move(text), TextStyle::small_bold, Tone::muted);
    l->style().margin = Insets{0, 10, 0, 2};
    return l;
}

// The name of a field a search matched in, as a row and the details show it.
std::string field_text(const history::Snippet& sn, const history::Clock& c) {
    switch (sn.field) {
        case history::Snippet::Field::title: return tr_s("title");
        case history::Snippet::Field::project: return tr_s("project");
        case history::Snippet::Field::folder: return tr_s("folder");
        case history::Snippet::Field::agent: return tr_s("agent");
        case history::Snippet::Field::tag: return tr_s("tag");
        case history::Snippet::Field::command: return tr_s("command");
        case history::Snippet::Field::output: return sn.ts_ms ? history::clock_text(c, sn.ts_ms) : tr_s("output");
    }
    return {};
}

// A match in the details column: where (field or time), then the line with the match marked.
// A click on an output line jumps to it.
class MatchLine final : public Widget {
public:
    MatchLine(history::Snippet sn, std::string where, std::function<void()> open)
        : Widget("match"), sn_(std::move(sn)), where_(std::move(where)), open_(std::move(open)) {
        style().height = 20.0f;
        set_tooltip(sn_.text);
    }
    const history::Snippet& snippet() const { return sn_; }
    void click() {
        if (open_) open_();
    }
    void paint(PaintContext& p) const override {
        const Theme& th = p.theme;
        const Rect r = local_bounds();
        if (open_ && hovered()) p.dl.round_rect(r, th.radius_control, th.hover);
        const FontRole small = role_for(th, TextStyle::small), mono = role_for(th, TextStyle::mono_small);
        auto w = p.text.ellipsize(where_, small, p.scale, 58);
        p.dl.text(w, {4, std::round((r.h - w->height()) / 2 + w->ascent())}, th.fg_muted);
        const float top = std::round((r.h - p.text.shape("M", mono, p.scale)->height()) / 2);
        paint_marked(p, sn_.text, sn_.marks, mono, 66, top, r.w - 70, th.fg);
    }
    EventResult on_pointer(PointerEvent& e) override {
        if (!open_ || e.button != platform::MouseButton::left) return EventResult::Ignored;
        if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
        if (e.type == PointerEvent::Type::Release) {
            if (local_bounds().contains(e.local)) click();
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }

private:
    history::Snippet sn_;
    std::string where_;
    std::function<void()> open_;
};

// A row that runs an action on a click (a command of a closed session), marked under the pointer.
class ClickRow final : public Box {
public:
    ClickRow(std::string id, std::function<void()> click) : Box(std::move(id), LayoutStyle::Dir::Row), click_(std::move(click)) {
        style().gap = 6;
    }
    void click() {
        if (click_) click_();
    }
    void paint(PaintContext& p) const override {
        if (hovered()) p.dl.round_rect(local_bounds(), p.theme.radius_control, p.theme.hover);
    }
    void on_hover(bool inside) override {
        Box::on_hover(inside);
        invalidate_paint();
    }
    platform::CursorShape cursor_at(Vec2) const override { return platform::CursorShape::hand; }
    EventResult on_pointer(PointerEvent& e) override {
        if (e.button != platform::MouseButton::left) return EventResult::Ignored;
        if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
        if (e.type == PointerEvent::Type::Release) {
            if (local_bounds().contains(e.local)) click();
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }

private:
    std::function<void()> click_;
};

// A filter value: a check mark, the label, the count on the right. A click toggles it.
class FacetRow final : public Widget {
public:
    FacetRow(std::string id, std::string text, size_t count, bool on, std::optional<ProjectColor> swatch, std::function<void()> toggle)
        : Widget(std::move(id)), text_(std::move(text)), count_(count), on_(on), swatch_(swatch), toggle_(std::move(toggle)) {
        style().height = 24.0f;
    }
    bool on() const { return on_; }
    size_t count() const { return count_; }
    void click() {
        if (toggle_) toggle_();
    }
    void paint(PaintContext& p) const override {
        const Theme& th = p.theme;
        const Rect r = local_bounds();
        if (on_) p.dl.round_rect(r, th.radius_control, th.selection);
        else if (hovered()) p.dl.round_rect(r, th.radius_control, th.hover);
        float x = 6;
        const Rect box{x, (r.h - 12) / 2, 12, 12};
        if (swatch_) p.dl.round_rect(box, 3, th.tag(*swatch_));
        else p.dl.round_rect(box, 3, Color{}, 1, th.border);
        if (on_) p.dl.icon(Icon::check, 12, {box.x, box.y}, swatch_ ? th.on_accent : th.accent);
        x += 18;
        const Color fg = count_ == 0 && !on_ ? th.fg_dim : th.fg;
        auto n = p.text.shape(count_ == kNoCount ? std::string() : std::to_string(count_), role_for(th, TextStyle::small), p.scale);
        const float nx = r.w - 6 - n->width();
        auto t = p.text.ellipsize(text_, role_for(th, TextStyle::body), p.scale, std::max(10.0f, nx - x - 6));
        p.dl.text(t, {x, std::round((r.h - t->height()) / 2 + t->ascent())}, fg);
        p.dl.text(n, {nx, std::round((r.h - n->height()) / 2 + n->ascent())}, th.fg_muted);
    }
    EventResult on_pointer(PointerEvent& e) override {
        if (e.button != platform::MouseButton::left) return EventResult::Ignored;
        if (e.type == PointerEvent::Type::Press) return EventResult::Handled;
        if (e.type == PointerEvent::Type::Release) {
            if (local_bounds().contains(e.local)) click();
            return EventResult::Handled;
        }
        return EventResult::Ignored;
    }
    void on_hover(bool inside) override {
        Widget::on_hover(inside);
        invalidate_paint();
    }
    std::string describe() const override {
        return text_ + (count_ == kNoCount ? std::string() : " " + std::to_string(count_)) + (on_ ? " on" : "");
    }

private:
    std::string text_;
    size_t count_;
    bool on_;
    std::optional<ProjectColor> swatch_;
    std::function<void()> toggle_;
};

// The window's root: the theme background; the details column folds away when narrow.
class HistoryRoot final : public Box {
public:
    HistoryRoot() : Box("history", LayoutStyle::Dir::Column) {}
    Widget* details = nullptr;
    std::function<void()> on_escape, on_delete, on_find, on_select_all;
    void paint(PaintContext& p) const override { p.dl.rect(local_bounds(), p.theme.bg); }
    void arrange(const Rect& r) override {
        if (details) {
            const bool show = r.w >= kDetailsMinWindow;
            if (details->visible() != show) details->set_visible(show);
        }
        Box::arrange(r);
    }
    EventResult on_key(const KeyEvent& k) override {
        if (k.action == platform::KeyAction::release) return EventResult::Ignored;
        std::function<void()>* fn = nullptr;
        if (k.key == platform::Key::escape) fn = &on_escape;
        else if ((k.key == platform::Key::delete_ || k.key == platform::Key::backspace) && (k.mods & ~primary_mod()) == 0) fn = &on_delete;
        else if (k.key == platform::Key::f && k.mods == primary_mod()) fn = &on_find;
        else if (k.key == platform::Key::a && k.mods == primary_mod()) fn = &on_select_all;
        if (!fn || !*fn) return EventResult::Ignored;
        (*fn)();
        return EventResult::Handled;
    }
};

}  // namespace

HistoryController::HistoryController(HistoryEnv env, HistoryUiState state) : env_(std::move(env)), st_(std::move(state)) {}

HistoryController::~HistoryController() { *alive_ = false; }

Window* HistoryController::window() const { return root_ ? root_->window() : nullptr; }

std::function<void()> HistoryController::later(std::function<void()> fn) {
    return [this, fn = std::move(fn), alive = std::weak_ptr<bool>(alive_)] {
        Window* w = window();
        if (!w) {
            fn();
            return;
        }
        w->add_timer(Duration::zero(), [fn, alive] {
            if (auto a = alive.lock(); a && *a) fn();
        });
    };
}

void HistoryController::closed() {
    root_ = nullptr;
    search_ = nullptr;
    group_ = sort_ = nullptr;
    chips_ = filters_ = details_ = nullptr;
    list_ = nullptr;
    details_scroll_ = nullptr;
    footer_ = nullptr;
}

std::unique_ptr<Widget> HistoryController::make_view() {
    items_ = env_.load ? env_.load() : std::vector<Item>{};
    commands_loaded_ = false;
    auto root = std::make_unique<HistoryRoot>();
    HistoryRoot* rp = root.get();
    root_ = rp;
    rp->on_escape = [this] {
        if (!marked_.empty()) return clear_marked();
        if (on_close) on_close();
    };
    rp->on_select_all = [this] { select_all(); };
    rp->on_delete = [this] { ask_delete(); };
    rp->on_find = [this] {
        if (Window* w = window(); w && search_) w->set_focus(&search_->field(), true);
    };

    // Header: title, search, group, sort.
    auto head = row("header", 10);
    head->style().padding = Insets{14, 12, 14, 10};
    head->style().align_items = LayoutStyle::Align::Center;
    head->add(label(tr_s("Session History"), TextStyle::title, Tone::normal, "title"));
    auto search = std::make_unique<SearchField>(tr_s("Search sessions and output"), "search");
    search->style().grow = 1;
    search->style().max_w = 420;
    search->set_text(st_.filter.text);
    search->on_change = [this](const std::string& t) { set_text(t); };
    search_ = search.get();
    head->add(std::move(search));
    auto spacer = std::make_unique<Box>("spacer");
    spacer->style().grow = 1;
    head->add(std::move(spacer));
    head->add(label(tr_s("Group:"), TextStyle::small, Tone::muted));
    std::vector<DropdownOption> go;
    size_t gsel = 0;
    for (size_t i = 0; i < kGroups.size(); ++i) {
        go.push_back({tr_s(kGroups[i].second), {}, {}});
        if (kGroups[i].first == st_.group) gsel = i;
    }
    auto gd = std::make_unique<Dropdown>(std::move(go), gsel, "group");
    gd->on_change = [this](size_t i) { set_group(kGroups[i].first); };
    group_ = gd.get();
    head->add(std::move(gd));
    head->add(label(tr_s("Sort:"), TextStyle::small, Tone::muted));
    std::vector<DropdownOption> so;
    size_t ssel = 0;
    for (size_t i = 0; i < kSorts.size(); ++i) {
        so.push_back({tr_s(kSorts[i].second), {}, {}});
        if (kSorts[i].first == st_.sort) ssel = i;
    }
    auto sd = std::make_unique<Dropdown>(std::move(so), ssel, "sort");
    sd->on_change = [this](size_t i) { set_sort(kSorts[i].first); };
    sort_ = sd.get();
    head->add(std::move(sd));
    rp->add(std::move(head));

    // Active filters as chips.
    auto chips = row("chips", 6);
    chips->style().padding = Insets{14, 0, 14, 8};
    chips->style().align_items = LayoutStyle::Align::Center;
    chips_ = chips.get();
    rp->add(std::move(chips));

    // Body: filters | list | details.
    auto body = row("body", 0);
    body->style().align_items = LayoutStyle::Align::Stretch;
    body->style().grow = 1;
    body->style().shrink = 1;
    body->style().min_h = 0;
    auto fcol = column("filters", 0);
    fcol->style().padding = Insets{10, 0, 10, 10};
    filters_ = fcol.get();
    auto fscroll = std::make_unique<ScrollView>(std::move(fcol), "filters_scroll");
    fscroll->style().width = kFilterW;
    fscroll->style().shrink = 0;
    fscroll->style().clip = true;
    body->add(std::move(fscroll));

    auto list = std::make_unique<ListView>(ListView::PaintRow{}, "list");
    list->set_row_height(kRowH);
    list->style().grow = 1;
    list->style().shrink = 1;
    list->set_paint_row([this](size_t i, PaintContext& p, Rect r, ListView::RowState s) { paint_row(i, p, r, s); });
    list->on_select_mods = [this](size_t i, uint16_t mods) { pick(i, mods); };
    list->on_activate = [this](size_t i) {
        if (i >= rows_.size()) return;
        if (rows_[i].header) toggle_fold(view_.sections[rows_[i].section].key);
        else activate();
    };
    list_ = list.get();
    auto lframe = std::make_unique<Box>("list_frame", LayoutStyle::Dir::Column);
    lframe->style().grow = 1;
    lframe->style().shrink = 1;
    lframe->style().clip = true;
    lframe->add(std::move(list));
    body->add(std::move(lframe));

    auto dcol = column("details", 6);
    dcol->style().padding = Insets{14, 4, 14, 14};
    details_ = dcol.get();
    auto dscroll = std::make_unique<ScrollView>(std::move(dcol), "details_scroll");
    dscroll->style().width = kDetailsW;
    dscroll->style().shrink = 0;
    dscroll->style().clip = true;
    details_scroll_ = dscroll.get();
    rp->details = dscroll.get();
    body->add(std::move(dscroll));
    rp->add(std::move(body));

    // Footer: totals and retention.
    auto foot = row("footer", 8);
    foot->style().padding = Insets{14, 8, 14, 10};
    foot->style().align_items = LayoutStyle::Align::Center;
    auto fl = label({}, TextStyle::small, Tone::muted, "summary");
    fl->style().grow = 1;
    footer_ = fl.get();
    foot->add(std::move(fl));
    if (env_.open_settings) {
        foot->add(Button::icon_button(Icon::gear, tr_s("History settings"), [this] {
            if (env_.open_settings) env_.open_settings();
        }, "settings"));
    }
    rp->add(std::move(foot));

    search_output();
    rebuild(false);
    return root;
}

void HistoryController::reload() {
    items_ = env_.load ? env_.load() : std::vector<Item>{};
    commands_of_.clear();
    commands_loaded_ = false;
    rebuild();
}

void HistoryController::rebuild(bool keep_scroll) {
    history::Options o;
    o.group = st_.group;
    o.sort = st_.sort;
    o.folded = st_.folded;
    o.now_ms = env_.now_ms ? env_.now_ms() : 0;
    o.local_day = env_.clock.day;
    o.output = &output_;
    o.agent_name = agent_label;
    if (!st_.filter.text.empty() && !commands_loaded_ && env_.commands) {
        // The search looks into the command lines too: read them once, when it starts.
        for (auto& it : items_) {
            if (!it.commands.empty()) continue;
            for (const auto& c : env_.commands(it.e.id)) it.commands += c.command + "\n";
        }
        commands_loaded_ = true;
    }
    view_ = history::build(items_, st_.filter, o);
    rows_.clear();
    std::set<size_t> listed;
    for (size_t s = 0; s < view_.sections.size(); ++s) {
        const auto& sec = view_.sections[s];
        if (!sec.title.empty()) rows_.push_back({true, s, 0});
        if (sec.folded) continue;
        for (size_t r : sec.rows) rows_.push_back({false, s, r});
    }
    // The selection follows the session; gone (filtered out, deleted): none.
    const history::Item* sel = selected();
    if (!marked_.empty()) {
        std::set<std::string> shown;
        for (const auto& r : rows_)
            if (!r.header && marked_.count(items_[r.item].e.id)) shown.insert(items_[r.item].e.id);
        marked_ = shown.size() >= 2 ? std::move(shown) : std::set<std::string>{};
    }
    if (list_) {
        const float off = list_->offset();
        list_->set_row_height(view_.matches.empty() ? kRowH : kRowSearchH);
        list_->set_count(rows_.size());
        std::optional<size_t> at;
        if (sel) {
            for (size_t i = 0; i < rows_.size(); ++i)
                if (!rows_[i].header && items_[rows_[i].item].e.id == st_.selected) {
                    at = i;
                    break;
                }
        }
        list_->set_selected(at, !keep_scroll);
        anchor_ = at;
        (void)off;
        list_->invalidate_paint();
    }
    build_filters();
    build_chips();
    build_details();
    build_footer();
}

const history::Item* HistoryController::selected() const {
    if (st_.selected.empty()) return nullptr;
    for (const auto& s : view_.sections)
        for (size_t r : s.rows)
            if (items_[r].e.id == st_.selected) return &items_[r];
    return nullptr;
}

void HistoryController::pick(size_t i, uint16_t mods) {
    if (i >= rows_.size() || rows_[i].header) return;   // the keys pass over headers
    const bool range = (mods & platform::mod_shift) != 0;
    const std::string id = items_[rows_[i].item].e.id;
    const bool toggle = !range && (mods & primary_mod()) != 0;
    if (range && anchor_ && *anchor_ < rows_.size()) {
        marked_.clear();
        for (size_t j = std::min(*anchor_, i); j <= std::max(*anchor_, i); ++j)
            if (!rows_[j].header) marked_.insert(items_[rows_[j].item].e.id);
        st_.selected = id;
    } else if (toggle) {
        if (marked_.empty() && !st_.selected.empty()) marked_.insert(st_.selected);
        if (marked_.erase(id)) {
            if (st_.selected == id) st_.selected = marked_.empty() ? std::string() : *marked_.begin();
        } else {
            marked_.insert(id);
            st_.selected = id;
        }
        anchor_ = i;
    } else {
        marked_.clear();
        st_.selected = id;
        anchor_ = i;
    }
    if (marked_.size() < 2) {
        if (marked_.size() == 1) st_.selected = *marked_.begin();
        marked_.clear();
    }
    if (list_) list_->invalidate_paint();
    build_details();
}

void HistoryController::select_all() {
    marked_.clear();
    for (const auto& r : rows_)
        if (!r.header) marked_.insert(items_[r.item].e.id);
    if (marked_.size() < 2) {
        marked_.clear();
        return;
    }
    if (st_.selected.empty() || !marked_.count(st_.selected)) st_.selected = *marked_.begin();
    if (list_) list_->invalidate_paint();
    build_details();
}

void HistoryController::clear_marked() {
    if (marked_.empty()) return;
    marked_.clear();
    if (list_) list_->invalidate_paint();
    build_details();
}

std::vector<const Item*> HistoryController::chosen() const {
    std::vector<const Item*> out;
    if (marked_.empty()) return out;
    std::set<std::string> seen;   // grouped by tag, a session can be in several sections
    for (const auto& r : rows_) {
        if (r.header) continue;
        const Item& it = items_[r.item];
        if (marked_.count(it.e.id) && seen.insert(it.e.id).second) out.push_back(&it);
    }
    return out;
}

void HistoryController::run_each(const std::vector<const Item*>& items, const std::function<Result<void>(const Item&)>& fn) {
    // Copies: the reload after each would move the items.
    std::vector<Item> copy;
    for (const Item* it : items) copy.push_back(*it);
    std::optional<Error> first;
    for (const Item& it : copy) {
        if (auto r = fn(it); !r && !first) first = r.error();
    }
    if (first) toast(first->what, true);
    reload();
}

bool HistoryController::select(const std::string& id) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i].header && items_[rows_[i].item].e.id == id) {
            marked_.clear();
            anchor_ = i;
            st_.selected = id;
            if (list_) list_->set_selected(i, true);
            build_details();
            return true;
        }
    }
    // Folded away: unfold its section.
    for (const auto& s : view_.sections) {
        for (size_t r : s.rows) {
            if (items_[r].e.id == id && s.folded) {
                st_.folded.erase(s.key);
                rebuild();
                return select(id);
            }
        }
    }
    return false;
}

void HistoryController::set_text(const std::string& text) {
    st_.filter.text = text;
    if (search_ && search_->text() != text) search_->set_text(text);
    search_output();
    rebuild(false);
}

void HistoryController::search_output() {
    std::string text = st_.filter.text;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    if (text == output_text_ && (output_busy_ || !output_.empty())) return;
    output_.clear();
    output_text_ = text;
    output_busy_ = false;
    if (text.size() < kMinOutputText || !env_.search_output) return;
    output_busy_ = true;
    env_.search_output(text, [this, alive = std::weak_ptr<bool>(alive_), text](history::OutputMatches m) {
        if (auto a = alive.lock(); !a || !*a) return;
        if (text != output_text_) return;   // the text changed meanwhile
        output_ = std::move(m);
        output_busy_ = false;
        rebuild();
    });
}

namespace {
template <class Set, class V>
void flip(Set& s, const V& v) {
    if (!s.erase(v)) s.insert(v);
}
}  // namespace

void HistoryController::toggle_state(State s) {
    flip(st_.filter.states, s);
    rebuild(false);
}
void HistoryController::set_date(history::DateRange d) {
    st_.filter.date = st_.filter.date == d ? history::DateRange::any : d;
    rebuild(false);
}
void HistoryController::toggle_project(const std::string& v) {
    flip(st_.filter.projects, v);
    rebuild(false);
}
void HistoryController::toggle_agent(const std::string& v) {
    flip(st_.filter.agents, v);
    rebuild(false);
}
void HistoryController::set_agents_all(bool on) {
    st_.filter.agents_all = on;
    rebuild(false);
}
void HistoryController::toggle_tag(const std::string& v) {
    flip(st_.filter.tags, v);
    rebuild(false);
}
void HistoryController::clear_filters() {
    st_.filter = {};
    if (search_) search_->set_text({});
    rebuild(false);
}
void HistoryController::set_group(Group g) {
    st_.group = g;
    if (group_) {
        for (size_t i = 0; i < kGroups.size(); ++i)
            if (kGroups[i].first == g) group_->set_selected(i);
    }
    rebuild(false);
}
void HistoryController::set_sort(Sort s) {
    st_.sort = s;
    if (sort_) {
        for (size_t i = 0; i < kSorts.size(); ++i)
            if (kSorts[i].first == s) sort_->set_selected(i);
    }
    rebuild(false);
}
void HistoryController::toggle_fold(const std::string& key) {
    flip(st_.folded, key);
    rebuild();
}

void HistoryController::build_filters() {
    if (!filters_) return;
    filters_->clear_children();
    auto add_facet = [&](const std::string& id, const std::string& title, const std::vector<history::FacetValue>& vals,
                         const std::function<void(const history::FacetValue&)>& toggle, bool colors) {
        if (vals.empty()) return;
        auto sec = column(id, 0);
        sec->add(heading(title));
        for (const auto& v : vals) {
            std::optional<ProjectColor> sw;
            if (colors) {
                for (const auto& it : items_)
                    if (it.e.project_name() == v.key && !v.key.empty()) {
                        sw = project_color(project_color_of(it));
                        break;
                    }
            }
            sec->add_child(std::make_unique<FacetRow>(v.key.empty() ? std::string("none") : v.key, v.label, v.count, v.on, sw,
                                                      later([toggle, v] { toggle(v); })));
        }
        filters_->add_child(std::move(sec));
    };
    add_facet("state", tr_s("STATE"), view_.states,
              [this](const history::FacetValue& v) {
                  toggle_state(v.key == "live" ? State::live : v.key == "background" ? State::background : State::closed);
              },
              false);
    add_facet("date", tr_s("DATE"), view_.dates,
              [this](const history::FacetValue& v) { set_date(static_cast<history::DateRange>(std::stoi(v.key))); }, false);
    add_facet("projects", tr_s("PROJECTS"), view_.projects, [this](const history::FacetValue& v) { toggle_project(v.key); }, true);
    add_facet("agents", tr_s("AGENTS"), view_.agents, [this](const history::FacetValue& v) { toggle_agent(v.key); }, false);
    // Two agents or more: a session with any of them, or with all of them.
    if (Widget* sec = filters_->child_count() ? filters_->child(filters_->child_count() - 1) : nullptr;
        st_.filter.agents.size() >= 2 && sec && sec->automation_id() == "agents") {
        sec->add_child(std::make_unique<FacetRow>("agents_all", tr_s("Match all selected"), kNoCount, st_.filter.agents_all,
                                                  std::nullopt, later([this] { set_agents_all(!st_.filter.agents_all); })));
    }
    // Tags: only when some session has one (else just "Untagged").
    if (view_.tags.size() > 1 || (!view_.tags.empty() && !view_.tags.front().key.empty()))
        add_facet("tags", tr_s("TAGS"), view_.tags, [this](const history::FacetValue& v) { toggle_tag(v.key); }, false);
}

void HistoryController::build_chips() {
    if (!chips_) return;
    chips_->clear_children();
    const auto& f = st_.filter;
    const bool any = !f.states.empty() || f.date != history::DateRange::any || !f.projects.empty() || !f.agents.empty() || !f.tags.empty();
    chips_->set_visible(any);
    if (!any) return;
    auto chip = [&](const std::string& id, const std::string& text, std::function<void()> off) {
        auto b = std::make_unique<Button>(text, later(std::move(off)), Button::Kind::secondary, id);
        b->set_icon(Icon::close, 10);
        chips_->add_child(std::move(b));
    };
    for (State s : f.states) chip("chip_state", view_.states[static_cast<size_t>(s)].label, [this, s] { toggle_state(s); });
    if (f.date != history::DateRange::any) {
        std::string t = tr_s("Custom range");
        for (const auto& d : view_.dates)
            if (d.on) t = d.label;
        chip("chip_date", t, [this] {
            st_.filter.date = history::DateRange::any;
            rebuild(false);
        });
    }
    for (const auto& p : f.projects) chip("chip_project", p.empty() ? tr_s("No project") : p, [this, p] { toggle_project(p); });
    for (const auto& a : f.agents) chip("chip_agent", a.empty() ? tr_s("No agent") : agent_label(a), [this, a] { toggle_agent(a); });
    if (f.agents_all && f.agents.size() >= 2) chip("chip_agents_all", tr_s("All of these agents"), [this] { set_agents_all(false); });
    for (const auto& t : f.tags) chip("chip_tag", t.empty() ? tr_s("Untagged") : t, [this, t] { toggle_tag(t); });
    chips_->add_child(std::make_unique<Button>(tr_s("Clear all"), later([this] {
        const std::string text = st_.filter.text;
        clear_filters();
        if (!text.empty()) set_text(text);   // the search stays: it has its own field
    }), Button::Kind::plain, "clear"));
}

void HistoryController::paint_row(size_t i, PaintContext& p, Rect r, ListView::RowState s) const {
    if (i >= rows_.size()) return;
    const Theme& th = p.theme;
    const Row& row = rows_[i];
    if (row.header) {
        const auto& sec = view_.sections[row.section];
        const float y = r.y + r.h - 10;
        p.dl.icon(sec.folded ? Icon::chevron_right : Icon::chevron_down, 12, {r.x + 10, y - 11}, th.fg_muted);
        std::string t = sec.title;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        auto run = p.text.shape(t + "  " + std::to_string(sec.rows.size()), role_for(th, TextStyle::small_bold), p.scale);
        p.dl.text(run, {r.x + 26, std::round(y - run->height() + run->ascent())}, th.fg_muted);
        p.dl.hairline(Rect{r.x + 10, r.y + r.h - 1, r.w - 20, 1}, th.border);
        return;
    }
    const Item& it = items_[row.item];
    const uint64_t now = env_.now_ms ? env_.now_ms() : 0;
    const Rect pill = r.inset(Insets::hv(6, 1));
    const bool sel = marked_.empty() ? s.selected : marked_.count(it.e.id) > 0;
    if (sel) p.dl.round_rect(pill, th.radius_control, s.focused ? th.accent.with_alpha(0.25f) : th.selection);
    else if (s.hovered) p.dl.round_rect(pill, th.radius_control, th.hover);
    // State by weight, color and badge together.
    const bool closed = it.state == State::closed;
    const Color fg = closed ? th.fg_muted : th.fg;
    const bool agent = !it.e.agents.empty();
    p.dl.icon(agent ? Icon::agent : Icon::terminal, 16, {pill.x + 8, pill.y + 5}, closed ? th.fg_dim : agent ? th.accent : th.fg);
    // The badge on the right of the first line.
    std::string badge = state_text(it, now);
    auto br = p.text.shape(badge, role_for(th, TextStyle::small), p.scale);
    const float line1 = pill.y + 4;
    float bx = pill.right() - 10 - br->width();
    if (it.state != State::closed) {
        const Rect dot{bx - 12, line1 + (br->height() - 7) / 2, 7, 7};
        if (it.state == State::live) p.dl.round_rect(dot, 3.5f, th.success);
        else p.dl.round_rect(dot, 3.5f, Color{}, 1.2f, th.success);
    }
    if (it.e.pinned) p.dl.icon(Icon::pin, 11, {bx - (it.state != State::closed ? 28.0f : 14.0f), line1 + 1}, th.fg_muted);
    // Project chip.
    float right = bx - (it.state != State::closed ? 18.0f : 6.0f) - (it.e.pinned ? 16.0f : 0.0f);
    const std::string proj = it.e.project_name();
    if (!proj.empty() && st_.group != Group::project) {
        auto pr = p.text.ellipsize(proj, role_for(th, TextStyle::small), p.scale, 120);
        const float pw = pr->width() + 12;
        const Rect chip{right - pw, line1, pw, pr->height() + 2};
        const auto pc = project_color(project_color_of(it));
        p.dl.round_rect(chip, 4, pc ? th.tag(*pc).with_alpha(0.22f) : th.surface_raised);
        p.dl.text(pr, {chip.x + 6, std::round(chip.y + 1 + pr->ascent())}, fg);
        right = chip.x - 8;
    }
    p.dl.text(br, {bx, std::round(line1 + br->ascent())}, closed ? th.fg_muted : th.fg);
    const std::string title = history::name_of(it.e);
    const std::vector<history::Snippet>* found = row.item < view_.matches.size() ? &view_.matches[row.item] : nullptr;
    const FontRole title_role = role_for(th, closed ? TextStyle::body : TextStyle::body_bold);
    std::vector<std::pair<size_t, size_t>> title_marks;
    if (found && !found->empty() && found->front().field == history::Snippet::Field::title) title_marks = found->front().marks;
    paint_marked(p, title, title_marks, title_role, pill.x + 30, line1, std::max(20.0f, right - (pill.x + 30)), fg);
    // Second line.
    const history::Clock& c = env_.clock;
    auto dr = p.text.ellipsize(row_detail(it, c, now), role_for(th, TextStyle::small), p.scale, pill.w - 40);
    p.dl.text(dr, {pill.x + 30, std::round(pill.y + 22 + dr->ascent())}, th.fg_muted);
    if (it.e.stats.failed_commands) {
        auto fr = p.text.shape("\xE2\x9C\x97" + std::to_string(it.e.stats.failed_commands), role_for(th, TextStyle::small), p.scale);
        const float fx = pill.x + 30 + dr->width() + 8;
        if (fx + fr->width() < pill.right() - 6) p.dl.text(fr, {fx, std::round(pill.y + 22 + fr->ascent())}, th.failure);
    }
    // Third line while searching: the first match outside the title, "+N" for the others.
    if (found) {
        const history::Snippet* first = nullptr;
        size_t others = 0;
        for (const auto& sn : *found) {
            if (sn.field == history::Snippet::Field::title) continue;
            if (!first) first = &sn;
            else ++others;
        }
        if (first) {
            const float y3 = pill.y + 39;
            const FontRole small = role_for(th, TextStyle::small);
            auto wr = p.text.shape(field_text(*first, c), small, p.scale);
            p.dl.text(wr, {pill.x + 30, std::round(y3 + wr->ascent())}, th.fg_dim);
            float end = pill.right() - 10;
            if (others) {
                auto mr = p.text.shape("+" + std::to_string(others), small, p.scale);
                end -= mr->width();
                p.dl.text(mr, {end, std::round(y3 + mr->ascent())}, th.fg_muted);
                end -= 8;
            }
            const float tx = pill.x + 30 + std::max(wr->width() + 8, 64.0f);
            paint_marked(p, first->text, first->marks, role_for(th, TextStyle::mono_small), tx, y3, end - tx,
                         closed ? th.fg_muted : th.fg);
        }
    }
}

void HistoryController::build_details() {
    if (!details_) return;
    details_->clear_children();
    if (!marked_.empty()) return build_multi_details();
    const Item* it = selected();
    if (!it) {
        auto l = label(view_.shown ? tr_s("Select a session to see its details.") : tr_s("No sessions match."), TextStyle::body,
                       Tone::muted, "empty");
        l->set_wrap(true);
        l->style().margin = Insets{0, 20, 0, 0};
        details_->add_child(std::move(l));
        return;
    }
    const uint64_t now = env_.now_ms ? env_.now_ms() : 0;
    const history::Clock& c = env_.clock;
    const auto& e = it->e;

    // Header: title (click to rename), state, project and folder.
    auto head = row("head", 6);
    auto title = label(history::name_of(e), TextStyle::title, Tone::normal, "title");
    title->style().grow = 1;
    head->add(std::move(title));
    if (env_.rename) head->add(Button::icon_button(Icon::edit, tr_s("Rename"), [this] { ask_rename(); }, "rename"));
    details_->add_child(std::move(head));
    std::string where = e.project_name();
    if (!e.cwd_last.empty()) where += (where.empty() ? "" : " \xC2\xB7 ") + e.cwd_last;
    auto st = label(state_text(*it, now) + (where.empty() ? "" : " \xC2\xB7 " + where), TextStyle::small, Tone::muted, "where");
    st->set_elide(Label::Elide::middle);
    st->set_tooltip(e.cwd_last);
    details_->add_child(std::move(st));

    // Actions.
    auto acts = row("actions", 6);
    acts->style().margin = Insets{0, 6, 0, 4};
    switch (it->state) {
        case State::live:
            acts->add_child(std::make_unique<Button>(tr_s("Show"), later([this] { activate(); }), Button::Kind::primary, "show"));
            break;
        case State::background:
            acts->add_child(std::make_unique<Button>(tr_s("Attach"), later([this] { activate(); }), Button::Kind::primary, "attach"));
            break;
        case State::closed:
            acts->add_child(std::make_unique<Button>(tr_s("Restore"), later([this] { activate(); }), Button::Kind::primary, "restore"));
            break;
    }
    acts->add_child(std::make_unique<Button>(e.pinned ? tr_s("Unpin") : tr_s("Pin"), later([this, id = e.id, pinned = e.pinned] {
        if (env_.pin) run(env_.pin(id, !pinned));
    }), Button::Kind::secondary, "pin"));
    if (env_.export_output && (it->state != State::closed || e.stats.stored_bytes > 0)) {
        auto ex = std::make_unique<Button>(tr_s("Export…"), [this] { ask_export(); }, Button::Kind::secondary, "export");
        ex->set_tooltip(tr_s("Save the stored output as text, raw with escape sequences, or an asciicast recording"));
        acts->add_child(std::move(ex));
    }
    if (it->state == State::closed && env_.remove)
        acts->add_child(std::make_unique<Button>(tr_s("Delete"), [this] { ask_delete(); }, Button::Kind::danger, "delete"));
    details_->add_child(std::move(acts));

    // What the search found in this session.
    const size_t at = static_cast<size_t>(it - items_.data());
    if (at < view_.matches.size() && !view_.matches[at].empty()) {
        details_->add_child(heading(tr_s("MATCHES")));
        for (const auto& sn : view_.matches[at]) {
            std::function<void()> open;
            if (sn.field == history::Snippet::Field::output && env_.open_output)
                open = later([this, line = commands::HistoryLine{sn.seq, sn.text}] {
                    if (const Item* cur = selected()) run(env_.open_output(*cur, output_text_, line));
                });
            details_->add_child(std::make_unique<MatchLine>(sn, field_text(sn, c), std::move(open)));
        }
    }

    // Times.
    auto fact = [&](const std::string& id, const std::string& k, const std::string& v) {
        auto r = row(id, 8);
        auto kl = label(k, TextStyle::small, Tone::muted);
        kl->set_width_samples({tr_s("Last used"), tr_s("Restored")});
        r->add(std::move(kl));
        auto vl = label(v, TextStyle::small, Tone::normal, "value");
        vl->style().grow = 1;
        r->add(std::move(vl));
        details_->add_child(std::move(r));
    };
    details_->add_child(heading(tr_s("TIMES")));
    fact("created", tr_s("Created"), history::when_text(c, e.created_ms, now));
    fact("last_used", tr_s("Last used"), it->state == State::closed ? history::when_text(c, e.last_used_ms, now) : tr_s("now"));
    if (e.ended_ms) fact("ended", tr_s("Ended"), history::when_text(c, *e.ended_ms, now));
    fact("duration", tr_s("Duration"), history::duration_text(history::duration_ms(*it, now)));
    if (!e.restores.empty())
        fact("restored", tr_s("Restored"),
             std::to_string(e.restores.size()) + (e.restores.size() == 1 ? tr_s(" time, last ") : tr_s(" times, last ")) +
                 history::when_text(c, e.restores.back(), now));

    // Agent runs, each with its own Resume.
    if (!e.agents.empty()) {
        details_->add_child(heading(tr_s("AGENT RUNS")));
        for (size_t i = 0; i < e.agents.size(); ++i) {
            const auto& a = e.agents[i];
            auto r = row("run", 8);
            r->style().align_items = LayoutStyle::Align::Center;
            auto texts = column("text", 1);
            texts->style().grow = 1;
            texts->style().shrink = 1;
            texts->add(label(agent_label(a.agent), TextStyle::body, Tone::normal));
            std::string span = history::when_text(c, a.started_ms, now) + " \xE2\x86\x92 " +
                               (a.ended_ms ? history::clock_text(c, *a.ended_ms) : tr_s("now"));
            texts->add(label(span, TextStyle::small, Tone::muted));
            r->add(std::move(texts));
            const bool can = !a.conversation.empty() && (it->state == State::closed ? bool(env_.restore) : bool(env_.resume));
            auto b = std::make_unique<Button>(tr_s("Resume"), later([this, i] {
                const Item* cur = selected();
                if (!cur) return;
                if (cur->state == State::closed) restore(*cur, i);
                else run(env_.resume(*cur, i));
            }), Button::Kind::secondary, "resume");
            b->set_enabled(can);
            if (a.conversation.empty()) b->set_tooltip(tr_s("The agent did not report its session id"));
            r->add(std::move(b));
            details_->add_child(std::move(r));
        }
    }

    // Commands (OSC 133), the last ones.
    if (env_.commands && commands_of_ != e.id) {
        commands_ = env_.commands(e.id);
        commands_of_ = e.id;
    }
    if (!commands_.empty()) {
        details_->add_child(heading(tr_s("COMMANDS")));
        constexpr size_t kShown = 40;
        const size_t from = commands_.size() > kShown ? commands_.size() - kShown : 0;
        if (from) details_->add_child(label(std::to_string(from) + tr_s(" earlier commands"), TextStyle::small, Tone::dim));
        for (size_t i = from; i < commands_.size(); ++i) {
            const auto& cmd = commands_[i];
            // A closed session's command opens its stored output there.
            std::unique_ptr<Box> r;
            if (it->state == State::closed && env_.open_output && !cmd.command.empty()) {
                r = std::make_unique<ClickRow>("command", later([this, line = commands::HistoryLine{cmd.seq, cmd.command}] {
                    if (const Item* cur = selected()) run(env_.open_output(*cur, line.text, line));
                }));
                r->set_tooltip(tr_s("Show in the session's output: ") + cmd.command);
            } else {
                r = row("command", 6);
            }
            // Only the command gives way to a narrow column: the time and the status keep their width.
            auto fixed = [](std::unique_ptr<Label> l) {
                l->style().shrink = 0;
                return l;
            };
            r->add(fixed(label(history::clock_text(c, cmd.started_ms), TextStyle::mono_small, Tone::muted, "time")));
            const bool failed = cmd.exit_code && *cmd.exit_code != 0;
            r->add(fixed(label(failed ? "\xE2\x9C\x97" : cmd.exit_code ? "\xE2\x9C\x93" : " ", TextStyle::mono_small,
                               failed ? Tone::failure : Tone::success)));
            auto t = label(cmd.command, TextStyle::mono_small, Tone::normal, "text");
            t->style().grow = 1;
            if (r->tooltip().empty()) t->set_tooltip(cmd.command);
            r->add(std::move(t));
            if (failed) r->add(fixed(label("(" + std::to_string(*cmd.exit_code) + ")", TextStyle::mono_small, Tone::failure)));
            details_->add_child(std::move(r));
        }
    }

    // Tags, sizes.
    details_->add_child(heading(tr_s("TAGS")));
    auto tags = row("tags", 6);
    tags->style().align_items = LayoutStyle::Align::Center;
    for (const auto& t : e.tags) {
        auto b = std::make_unique<Button>(t, later([this, id = e.id, t] {
            if (env_.tag) run(env_.tag(id, {}, {t}));
        }), Button::Kind::secondary, "tag");
        b->set_icon(Icon::close, 10);
        b->set_tooltip(tr_s("Remove the tag"));
        tags->add_child(std::move(b));
    }
    if (env_.tag) tags->add_child(Button::icon_button(Icon::plus, tr_s("Add a tag"), [this] { ask_tag(); }, "add_tag"));
    details_->add_child(std::move(tags));
    if (e.stats.raw_bytes || e.stats.stored_bytes) {
        details_->add_child(heading(tr_s("OUTPUT")));
        std::string s = format_bytes(e.stats.raw_bytes) + tr_s(" of output, ") + format_bytes(e.stats.stored_bytes) + tr_s(" stored");
        details_->add_child(label(s, TextStyle::small, Tone::normal, "sizes"));
    }
    if (e.pinned) details_->add_child(label(tr_s("Pinned: never deleted automatically."), TextStyle::small, Tone::muted, "pinned"));
}

void HistoryController::build_multi_details() {
    const auto items = chosen();
    size_t closed = 0, pinned = 0;
    uint64_t stored = 0;
    std::map<std::string, size_t> tags;   // tag -> sessions that have it
    for (const Item* it : items) {
        closed += it->state == State::closed;
        pinned += it->e.pinned;
        stored += it->e.stats.stored_bytes;
        for (const auto& t : it->e.tags) ++tags[t];
    }
    auto head = row("head", 6);
    head->add(label(std::to_string(items.size()) + tr_s(" sessions selected"), TextStyle::title, Tone::normal, "title"));
    details_->add_child(std::move(head));
    std::string what;
    if (closed) what = std::to_string(closed) + tr_s(" closed");
    if (closed < items.size()) what += (what.empty() ? "" : " \xC2\xB7 ") + std::to_string(items.size() - closed) + tr_s(" running");
    if (stored) what += " \xC2\xB7 " + format_bytes(stored) + tr_s(" stored");
    details_->add_child(label(what, TextStyle::small, Tone::muted, "where"));

    auto acts = row("actions", 6);
    acts->style().margin = Insets{0, 6, 0, 4};
    if (env_.pin) {
        const bool pin = pinned < items.size();   // some are not pinned: pin them all
        acts->add_child(std::make_unique<Button>(pin ? tr_s("Pin") : tr_s("Unpin"), later([this, pin] {
            std::vector<const Item*> todo;
            for (const Item* it : chosen())
                if (it->e.pinned != pin) todo.push_back(it);
            run_each(todo, [this, pin](const Item& it) { return env_.pin(it.e.id, pin); });
        }), Button::Kind::secondary, "pin"));
    }
    if (closed && env_.remove) {
        auto del = std::make_unique<Button>(closed == items.size() ? tr_s("Delete") : tr_s("Delete ") + std::to_string(closed) + tr_s(" closed"),
                                            [this] { ask_delete(); }, Button::Kind::danger, "delete");
        acts->add_child(std::move(del));
    }
    details_->add_child(std::move(acts));

    details_->add_child(heading(tr_s("TAGS")));
    auto trow = row("tags", 6);
    trow->style().align_items = LayoutStyle::Align::Center;
    for (const auto& [t, n] : tags) {
        const std::string text = n == items.size() ? t : t + " " + std::to_string(n) + "/" + std::to_string(items.size());
        auto b = std::make_unique<Button>(text, later([this, t] {
            std::vector<const Item*> todo;
            for (const Item* it : chosen())
                if (std::find(it->e.tags.begin(), it->e.tags.end(), t) != it->e.tags.end()) todo.push_back(it);
            run_each(todo, [this, t](const Item& it) { return env_.tag(it.e.id, {}, {t}); });
        }), Button::Kind::secondary, "tag");
        b->set_icon(Icon::close, 10);
        b->set_tooltip(tr_s("Remove the tag from every selected session that has it"));
        if (!env_.tag) b->set_enabled(false);
        trow->add_child(std::move(b));
    }
    if (env_.tag) trow->add_child(Button::icon_button(Icon::plus, tr_s("Add tags to every selected session"), [this] { ask_tag(); }, "add_tag"));
    details_->add_child(std::move(trow));
    // No modifier names: the goldens are the same on every OS.
    auto h = label(tr_s("Esc goes back to one session."), TextStyle::small, Tone::muted, "hint");
    h->set_wrap(true);
    h->style().margin = Insets{0, 10, 0, 0};
    details_->add_child(std::move(h));
}

void HistoryController::build_footer() {
    if (!footer_) return;
    uint64_t raw = 0, stored = 0;
    for (const auto& it : items_) {
        raw += it.e.stats.raw_bytes;
        stored += it.e.stats.stored_bytes;
    }
    std::string s = std::to_string(items_.size()) + (items_.size() == 1 ? tr_s(" session") : tr_s(" sessions"));
    if (view_.shown != items_.size()) s += " (" + std::to_string(view_.shown) + tr_s(" shown") + ")";
    if (output_busy_) s += " \xC2\xB7 " + tr_s("searching the stored output\xE2\x80\xA6");
    if (raw || stored) s += " \xC2\xB7 " + format_bytes(raw) + tr_s(" of output stored in ") + format_bytes(stored);
    if (env_.retention_note) {
        const std::string n = env_.retention_note();
        if (!n.empty()) s += " \xC2\xB7 " + n;
    }
    footer_->set_text(s);
}

void HistoryController::activate() {
    const Item* it = selected();
    if (!it) return;
    switch (it->state) {
        case State::live:
            if (env_.show) run(env_.show(*it));
            break;
        case State::background:
            if (env_.attach) run(env_.attach(*it));
            break;
        case State::closed:
            if (env_.restore) restore(*it, std::nullopt);
            break;
    }
}

void HistoryController::restore(const Item& it, std::optional<std::size_t> run_index) {
    auto r = env_.restore(it, run_index, [this, alive = std::weak_ptr<bool>(alive_)](const Result<void>& out) {
        if (alive.expired() || !*alive.lock() || !window()) return false;
        if (!out) toast(tr_s("The session was not restored: ") + out.error().what, true);
        reload();
        return true;
    });
    if (r) toast(tr_s("Restoring ") + it.e.display_title() + "\xE2\x80\xA6", false);
    run(r);
}

void HistoryController::run(const Result<void>& r) {
    if (!r) toast(r.error().what, true);
    reload();
}

void HistoryController::toast(std::string text, bool failure) {
    if (Window* w = window()) show_toast(*w, ToastSpec{std::move(text), {}, failure ? Icon::alert : Icon::check, failure ? Tone::failure : Tone::normal});
}

void HistoryController::ask_rename() {
    const Item* it = selected();
    Window* w = window();
    if (!it || !w || !env_.rename) return;
    auto value = std::make_shared<std::string>(it->e.display_title());
    auto field = std::make_unique<TextField>(tr_s("Title"), "title_field");
    field->set_text(*value);
    field->select_all();
    field->on_change = [value](const std::string& v) { *value = v; };
    TextField* raw = field.get();
    DialogSpec d;
    d.title = tr_s("Rename Session");
    d.message = tr_s("An empty title brings the automatic one back.");
    d.body = std::move(field);
    d.buttons.push_back({tr_s("Cancel"), {}, Button::Kind::secondary, false, true});
    d.buttons.push_back({tr_s("Rename"), [this, value, id = it->e.id, alive = std::weak_ptr<bool>(alive_)] {
                             if (alive.expired() || !*alive.lock()) return;
                             run(env_.rename(id, *value));
                         },
                         Button::Kind::primary, true, false});
    show_dialog(*w, std::move(d));
    w->set_focus(raw, true);
}

void HistoryController::ask_tag() {
    const Item* it = selected();
    Window* w = window();
    if (!it || !w || !env_.tag) return;
    std::vector<std::string> ids;
    for (const Item* c : chosen()) ids.push_back(c->e.id);
    if (ids.empty()) ids.push_back(it->e.id);
    auto value = std::make_shared<std::string>();
    auto field = std::make_unique<TextField>(tr_s("Tag"), "tag_field");
    field->on_change = [value](const std::string& v) { *value = v; };
    TextField* raw = field.get();
    DialogSpec d;
    d.title = ids.size() > 1 ? tr_s("Add Tags to ") + std::to_string(ids.size()) + tr_s(" Sessions") : tr_s("Add a Tag");
    d.message = tr_s("Several tags: separate them with spaces.");
    d.body = std::move(field);
    d.buttons.push_back({tr_s("Cancel"), {}, Button::Kind::secondary, false, true});
    d.buttons.push_back({tr_s("Add"), [this, value, ids, alive = std::weak_ptr<bool>(alive_)] {
                             if (alive.expired() || !*alive.lock()) return;
                             std::vector<std::string> add;
                             std::string cur;
                             for (char ch : *value + " ") {
                                 if (ch == ' ' || ch == ',') {
                                     if (!cur.empty()) add.push_back(cur);
                                     cur.clear();
                                 } else {
                                     cur += ch;
                                 }
                             }
                             if (add.empty()) return;
                             std::optional<Error> first;
                             for (const auto& id : ids) {
                                 if (auto r = env_.tag(id, add, {}); !r && !first) first = r.error();
                             }
                             if (first) toast(first->what, true);
                             reload();
                         },
                         Button::Kind::primary, true, false});
    show_dialog(*w, std::move(d));
    w->set_focus(raw, true);
}

void HistoryController::ask_delete() {
    if (!marked_.empty()) {
        Window* w = window();
        if (!w || !env_.remove) return;
        std::vector<std::string> ids;
        uint64_t bytes = 0;
        size_t running = 0;
        for (const Item* c : chosen()) {
            if (c->state != State::closed) {
                ++running;
                continue;
            }
            ids.push_back(c->e.id);
            bytes += c->e.stats.stored_bytes;
        }
        if (ids.empty()) return;
        DialogSpec d;
        d.title = tr_s("Delete ") + std::to_string(ids.size()) + (ids.size() == 1 ? tr_s(" session?") : tr_s(" sessions?"));
        d.message = std::to_string(ids.size()) + (ids.size() == 1 ? tr_s(" closed session") : tr_s(" closed sessions")) +
                    tr_s(" and their stored output (") + format_bytes(bytes) + tr_s(") go away for good.");
        if (running) d.message += " " + std::to_string(running) + (running == 1 ? tr_s(" running one stays.") : tr_s(" running ones stay."));
        d.buttons.push_back({tr_s("Cancel"), {}, Button::Kind::secondary, false, true});
        d.buttons.push_back({tr_s("Delete"), [this, ids, alive = std::weak_ptr<bool>(alive_)] {
                                 if (alive.expired() || !*alive.lock()) return;
                                 std::optional<Error> first;
                                 for (const auto& id : ids) {
                                     if (auto r = env_.remove(id); !r && !first) first = r.error();
                                 }
                                 if (first) toast(first->what, true);
                                 marked_.clear();
                                 st_.selected.clear();
                                 reload();
                             },
                             Button::Kind::danger, true, false});
        show_dialog(*w, std::move(d));
        return;
    }
    const Item* it = selected();
    Window* w = window();
    if (!it || !w || !env_.remove || it->state != State::closed) return;
    DialogSpec d;
    d.title = tr_s("Delete this session?");
    const std::string name = history::name_of(it->e);
    d.message = "\"" + name + "\"" + tr_s(" and its stored output (") + format_bytes(it->e.stats.stored_bytes) +
                tr_s(") go away for good.");
    d.buttons.push_back({tr_s("Cancel"), {}, Button::Kind::secondary, false, true});
    d.buttons.push_back({tr_s("Delete"), [this, id = it->e.id, alive = std::weak_ptr<bool>(alive_)] {
                             if (alive.expired() || !*alive.lock()) return;
                             auto r = env_.remove(id);
                             if (!r) toast(r.error().what, true);
                             st_.selected.clear();
                             reload();
                         },
                         Button::Kind::danger, true, false});
    show_dialog(*w, std::move(d));
}

void HistoryController::ask_export() {
    const Item* it = selected();
    Window* w = window();
    if (!it || !w || !env_.export_output) return;
    ask_export_format(*w, [this, item = *it, alive = std::weak_ptr<bool>(alive_)](session::ExportFormat f) {
        if (alive.expired() || !*alive.lock()) return;
        env_.export_output(item, f, [this, alive](Result<ExportResult> r) {
            if (alive.expired() || !*alive.lock()) return;
            if (Window* win = window()) show_export_result(*win, r);
        });
    });
}

void ask_export_format(Window& w, std::function<void(session::ExportFormat)> chosen) {
    DialogSpec d;
    d.title = tr_s("Export the output");
    d.message = tr_s("Text: the lines as the output viewer shows them. Raw: what the programs wrote, with the escape "
                     "sequences (cat it in a terminal). asciicast: a recording to play with asciinema.");
    d.width = 460;
    auto pick = [chosen = std::make_shared<std::function<void(session::ExportFormat)>>(std::move(chosen))](session::ExportFormat f) {
        return [chosen, f] { (*chosen)(f); };
    };
    d.buttons.push_back({tr_s("Cancel"), {}, Button::Kind::secondary, false, true});
    d.buttons.push_back({tr_s("Raw"), pick(session::ExportFormat::raw), Button::Kind::secondary, false, false});
    d.buttons.push_back({tr_s("asciicast"), pick(session::ExportFormat::asciicast), Button::Kind::secondary, false, false});
    d.buttons.push_back({tr_s("Text"), pick(session::ExportFormat::text), Button::Kind::primary, true, false});
    show_dialog(w, std::move(d));
}

void show_export_result(Window& w, const Result<ExportResult>& r) {
    ToastSpec t;
    if (!r) {
        t.title = tr_s("The output could not be exported");
        t.subtitle = r.error().what;
        t.icon = Icon::alert;
        t.tone = Tone::failure;
    } else {
        t.title = tr_s("Exported to ") + r->path;
        if (r->unsized) t.subtitle = tr_s("No terminal size was stored for it: the recording plays at 80x24");
        t.icon = Icon::check;
    }
    show_toast(w, t);
}

}  // namespace kt::app
