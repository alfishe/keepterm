#include "app/settings_controller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>

#include "agents/registry.h"
#include "commands/layout.h"
#include "commands/registry.h"
#include "theme/library.h"
#include "theme/theme_set.h"

namespace kt::app {

using namespace kt::ui;
using config::KeyId;
using config::KeySpec;
using config::Value;
using config::ValueType;
using settings::Control;
using settings::RowDef;

std::string hex_of(std::uint32_t rgb) {
    char b[8];
    std::snprintf(b, sizeof b, "#%06X", rgb & 0xFFFFFFu);
    return b;
}

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool contains_folded(std::string_view hay, std::string_view needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

// Every word of the query is in one of the fields.
bool matches(std::string_view query, std::initializer_list<std::string_view> fields) {
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && query[i] == ' ') ++i;
        size_t j = i;
        while (j < query.size() && query[j] != ' ') ++j;
        if (j > i) {
            const std::string_view w = query.substr(i, j - i);
            bool any = false;
            for (auto f : fields) any = any || contains_folded(f, w);
            if (!any) return false;
        }
        i = j;
    }
    return true;
}

Color color_of(std::uint32_t rgb) { return Color::rgb(rgb & 0xFFFFFFu); }

// A text field that also commits when it loses focus.
class CommitField final : public TextField {
public:
    CommitField(std::string placeholder, std::string id) : TextField(std::move(placeholder), std::move(id)) {}
    std::function<void()> on_blur;
    void on_focus(FocusChange c) override {
        TextField::on_focus(c);
        if (c == FocusChange::Lost && on_blur) on_blur();
    }
};

std::string percent(double v) {
    char b[16];
    std::snprintf(b, sizeof b, "%d %%", static_cast<int>(std::lround(v * 100)));
    return b;
}

std::string trim_number(double v, double step) {
    char b[32];
    int d = 0;
    while (d < 3 && std::fabs(step * std::pow(10.0, d) - std::round(step * std::pow(10.0, d))) > 1e-6) ++d;
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}

std::string origin_note(const config::Origin& o) {
    switch (o.layer) {
        case config::Layer::Cli: return "Set by --set on the command line; the file's value applies after a restart without it.";
        case config::Layer::Include: return "From " + o.file + "; a change here is saved to config.toml and wins over it.";
        default: return o.fallback ? "The file's value is invalid; the default applies." : std::string();
    }
}

bool depends_met(const config::Config& c, std::string_view dep) {
    if (dep.empty()) return true;
    const size_t eq = dep.find('=');
    const KeySpec* s = config::find_key(dep.substr(0, eq));
    if (!s) return true;
    if (eq == std::string_view::npos) return s->type == ValueType::Bool ? c.get_bool(s->id) : true;
    return c.get_string(s->id) == dep.substr(eq + 1);
}

Control resolve(const RowDef& r, const KeySpec& s) {
    if (r.control != Control::Auto) return r.control;
    switch (s.type) {
        case ValueType::Bool: return Control::Toggle;
        case ValueType::Enum: return s.values.size() <= 4 ? Control::Segmented : Control::Dropdown;
        case ValueType::Float: return s.range ? Control::Slider : Control::Text;
        case ValueType::Int: return s.range && s.range->hi - s.range->lo <= 100 ? Control::Stepper : Control::Text;
        case ValueType::Color: return Control::Color;
        case ValueType::StringMap: return Control::WideText;
        default: return Control::Text;
    }
}

// A list that scrolls its selection into view once it has a size (rows are selected before
// the first layout).
class RevealList final : public ListView {
public:
    RevealList() : ListView({}, "list") {}
    bool reveal = true;
    void arrange(const Rect& f) override {
        ListView::arrange(f);
        if (reveal && f.h > 0 && selected()) {
            reveal = false;
            scroll_to_row(std::min(count() - 1, *selected() + 3));
            scroll_to_row(*selected());
        }
    }
};

// A set of characters (terminal.word_separators) in a text field: a space and a tab are
// invisible there and a plain field trims them, so they show as visible marks instead.
constexpr std::string_view kSpaceMark = "\xE2\x90\xA3";   // U+2423 OPEN BOX
constexpr std::string_view kTabMark = "\xE2\x87\xA5";     // U+21E5 RIGHTWARDS ARROW TO BAR

bool is_char_set_key(std::string_view key) { return key == "terminal.word_separators"; }

std::string char_set_shown(std::string_view v) {
    std::string out;
    for (char c : v) {
        if (c == ' ') out += kSpaceMark;
        else if (c == '\t') out += kTabMark;
        else out += c;
    }
    return out;
}

std::string char_set_value(std::string_view shown) {
    std::string out;
    for (size_t i = 0; i < shown.size();) {
        if (shown.substr(i, kSpaceMark.size()) == kSpaceMark) {
            out += ' ';
            i += kSpaceMark.size();
        } else if (shown.substr(i, kTabMark.size()) == kTabMark) {
            out += '\t';
            i += kTabMark.size();
        } else {
            out += shown[i++];
        }
    }
    return out;
}

std::unique_ptr<Box> page_box() {
    auto b = column("page", 18);
    return b;
}

}  // namespace

// One row bound to a key: how to show a value in its control.
struct SettingsController::Bound {
    const KeySpec* spec = nullptr;
    const RowDef* def = nullptr;
    FormRow* row = nullptr;
    TextField* field = nullptr;                    // text controls: skip updates while typing
    std::function<void(const Value&)> show;
    std::string local_error;                       // from the writer, until the next good write
};

// The theme picker of Appearance.
struct SettingsController::Theme {
    std::vector<theme::ThemeInfo> all;
    std::vector<size_t> shown;                     // indices into all, filtered
    std::string filter;
    bool dark_side = true;
    ListView* list = nullptr;
    SchemePreview* preview = nullptr;
    SegmentedControl* side = nullptr;
    Label* chosen = nullptr;
    std::string current;                           // the chosen theme of the side
};

SettingsController::SettingsController(SettingsEnv env)
    : env_(std::move(env)), writer_(env_.config_file) {
    cfg_ = env_.config ? env_.config() : config::Config();
    keymap_ = commands::Keymap::from_config(cfg_, env_.os).keymap;
    writer_.on_written = [this] {
        if (env_.written) env_.written();
    };
}

SettingsController::~SettingsController() { *alive_ = false; }

std::unique_ptr<SettingsView> SettingsController::make_view(std::string_view pane) {
    std::vector<SectionItem> items;
    for (const auto& p : settings::panes()) {
        if (!(p.os & settings::os_bit(env_.os))) continue;
        items.push_back({std::string(p.id), std::string(p.title), p.icon, {}});
    }
    auto v = std::make_unique<SettingsView>(std::move(items));
    view_ = v.get();
    v->on_section = [this](size_t i) {
        const auto& it = view_->sections().items();
        if (i >= it.size()) return;
        if (!query_.empty()) {
            query_.clear();
            view_->search().set_text("");
        }
        show_pane(it[i].id);
    };
    v->on_search = [this](const std::string& q) { search(q); };
    v->on_close = [this] {
        if (on_close) on_close();
    };
    const std::string first = pane.empty() ? view_->sections().items().front().id : std::string(pane);
    const auto idx = view_->sections().index_of(first);
    view_->sections().set_selected(idx.value_or(0), false);
    show_pane(idx ? first : view_->sections().items().front().id);
    return v;
}

void SettingsController::closed() {
    view_ = nullptr;
    bound_.clear();
    key_rows_.clear();
    agent_rows_.clear();
    hook_toggle_ = nullptr;
    hook_row_ = nullptr;
    theme_.reset();
    key_list_ = nullptr;
}

void SettingsController::show_page(std::unique_ptr<Widget> w, std::string title, bool keep_scroll) {
    if (!view_) return;
    view_->set_page(std::move(title), std::move(w), keep_scroll);
    update_banner();
    update_rows();
}

void SettingsController::show_pane(std::string_view id) {
    if (!view_) return;
    const settings::PaneDef* p = settings::find_pane(id);
    if (!p) return;
    const bool same = pane_ == id && query_.empty();
    pane_ = std::string(id);
    if (auto i = view_->sections().index_of(id); i && view_->sections().selected() != i)
        view_->sections().set_selected(*i, false);
    // The old page's controls live until the new page replaces them.
    auto old_bound = std::move(bound_);
    auto old_theme = std::move(theme_);
    bound_.clear();
    key_rows_.clear();
    agent_rows_.clear();
    hook_toggle_ = nullptr;
    hook_row_ = nullptr;
    key_list_ = nullptr;
    auto page = build_pane(*p);
    show_page(std::move(page), std::string(p->title), same);
}

void SettingsController::search(const std::string& q) {
    if (!view_) return;
    query_ = q;
    if (q.empty()) {
        const std::string p = pane_;
        pane_.clear();
        show_pane(p.empty() ? std::string(settings::panes().front().id) : p);
        return;
    }
    auto old_bound = std::move(bound_);
    auto old_theme = std::move(theme_);
    bound_.clear();
    key_rows_.clear();
    agent_rows_.clear();
    hook_toggle_ = nullptr;
    hook_row_ = nullptr;
    key_list_ = nullptr;
    show_page(build_search(q), "Search Results", false);
}

std::unique_ptr<Widget> SettingsController::build_search(const std::string& q) {
    auto page = page_box();
    size_t found = 0;
    for (const auto& p : settings::panes()) {
        if (!(p.os & settings::os_bit(env_.os))) continue;
        std::unique_ptr<FormGroup> g;
        for (const auto& gd : p.groups) {
            for (const auto& r : gd.rows) {
                if (!settings::row_visible(r, env_.os)) continue;
                const KeySpec* s = config::find_key(r.key);
                if (!s) continue;
                if (!matches(q, {r.label, r.description, r.key, p.title, gd.title, s->doc})) continue;
                if (!g) g = std::make_unique<FormGroup>(std::string(p.title), std::string(p.id));
                g->add_row(build_row(r));
                ++found;
            }
        }
        if (g) page->add_child(std::move(g));
    }
    // Commands, for "Keymaps" searches by command name.
    std::unique_ptr<FormGroup> cg;
    for (const auto& c : commands::builtin_registry().all()) {
        if (!c.bindable) continue;
        if (!matches(q, {c.title, c.id, c.description})) continue;
        if (!cg) cg = std::make_unique<FormGroup>("Keymaps", "keymaps");
        auto caps = keymap_.key_caps(c.id);
        auto b = std::make_unique<Button>(caps ? commands::format_sequence(*caps, env_.os, commands::Format::Text) : std::string("Not bound"),
                                          [this] {
                                              query_.clear();
                                              view_->search().set_text("");
                                              show_pane("keymaps");
                                          },
                                          Button::Kind::secondary, "show");
        auto row = std::make_unique<FormRow>(std::string(c.title), std::string(c.id), std::move(b), std::string(c.id));
        cg->add_row(std::move(row));
        ++found;
        if (cg->rows() >= 12) break;
    }
    if (cg) page->add_child(std::move(cg));
    if (found == 0) page->emplace<Label>("No settings match \xE2\x80\x9C" + q + "\xE2\x80\x9D.", TextStyle::body, Tone::muted, "empty");
    return page;
}

std::unique_ptr<Widget> SettingsController::build_pane(const settings::PaneDef& p) {
    auto page = page_box();
    switch (p.special) {
        case settings::Special::Appearance: add_appearance(*page); break;
        case settings::Special::Storage: add_storage(*page); break;
        default: break;
    }
    add_groups(*page, p, {});
    switch (p.special) {
        case settings::Special::Keymaps: add_keymaps(*page); break;
        case settings::Special::Agents: add_agents(*page); break;
        case settings::Special::Advanced: add_advanced(*page); break;
        case settings::Special::History: add_history(*page); break;
        default: break;
    }
    return page;
}

void SettingsController::add_groups(Widget& page, const settings::PaneDef& p, const std::string&) {
    for (const auto& gd : p.groups) {
        auto g = std::make_unique<FormGroup>(std::string(gd.title), lower(gd.title));
        for (const auto& r : gd.rows) {
            if (!settings::row_visible(r, env_.os) || !config::find_key(r.key)) continue;
            g->add_row(build_row(r));
        }
        if (!gd.footnote.empty()) g->set_footnote(std::string(gd.footnote));
        if (g->rows() > 0) page.add_child(std::move(g));
    }
}

std::unique_ptr<FormRow> SettingsController::build_row(const RowDef& r) {
    const KeySpec& s = *config::find_key(r.key);
    auto b = std::make_unique<Bound>();
    Bound* bp = b.get();
    b->spec = &s;
    b->def = &r;
    const KeyId k = s.id;
    std::unique_ptr<Widget> ctl;
    bool wide = false;
    const Control kind = resolve(r, s);
    auto commit = [this, k](Value v) { (void)set_value(k, v); };

    switch (kind) {
        case Control::Toggle: {
            auto t = std::make_unique<Toggle>(false, [commit](bool on) { commit(Value(on)); }, "control");
            Toggle* tp = t.get();
            b->show = [tp](const Value& v) {
                if (auto* x = std::get_if<bool>(&v)) tp->set_on(*x, false);
            };
            ctl = std::move(t);
            break;
        }
        case Control::Segmented:
        case Control::Dropdown: {
            std::vector<std::string> labels;
            for (auto v : s.values) labels.push_back(settings::option_label(r, v));
            const auto* vals = &s.values;
            auto pick = [commit, vals](size_t i) {
                if (i < vals->size()) commit(Value(std::string((*vals)[i])));
            };
            auto index_of = [vals](const Value& v) {
                const auto* str = std::get_if<std::string>(&v);
                for (size_t i = 0; str && i < vals->size(); ++i)
                    if ((*vals)[i] == *str) return i;
                return SegmentedControl::npos;
            };
            if (kind == Control::Segmented) {
                auto c = std::make_unique<SegmentedControl>(labels, size_t{0}, "control");
                SegmentedControl* cp = c.get();
                c->on_change = pick;
                b->show = [cp, index_of](const Value& v) { cp->set_selected(index_of(v)); };
                ctl = std::move(c);
            } else {
                std::vector<DropdownOption> opts;
                for (size_t i = 0; i < labels.size(); ++i)
                    opts.push_back({labels[i], Value(std::string(s.values[i])) == s.default_value ? "Default" : "", std::nullopt});
                auto c = std::make_unique<Dropdown>(std::move(opts), size_t{0}, "control");
                Dropdown* cp = c.get();
                c->on_change = pick;
                b->show = [cp, index_of](const Value& v) { cp->set_selected(index_of(v)); };
                ctl = std::move(c);
            }
            break;
        }
        case Control::Preset: {
            // The fixed values of `options` that parse for the key, in their order.
            auto values = std::make_shared<std::vector<std::pair<Value, std::string>>>();
            for (std::string_view o = r.options; !o.empty();) {
                const size_t bar = o.find('|');
                const std::string_view item = o.substr(0, bar);
                const size_t eq = item.find('=');
                if (eq != std::string_view::npos) {
                    if (auto v = config::parse_input(s, std::string(item.substr(0, eq)))) values->emplace_back(*v, std::string(item.substr(eq + 1)));
                }
                if (bar == std::string_view::npos) break;
                o.remove_prefix(bar + 1);
            }
            auto shown = std::make_shared<std::vector<Value>>();
            auto c = std::make_unique<Dropdown>(std::vector<DropdownOption>{}, size_t{0}, "control");
            Dropdown* cp = c.get();
            c->on_change = [this, k, shown](size_t i) {
                if (i < shown->size()) commit_choice(k, (*shown)[i]);
            };
            b->show = [cp, values, shown, sp = &s](const Value& v) {
                shown->clear();
                std::vector<DropdownOption> opts;
                size_t sel = SIZE_MAX;
                for (const auto& [val, label] : *values) {
                    if (val == v) sel = opts.size();
                    shown->push_back(val);
                    opts.push_back({label, val == sp->default_value ? "Default" : "", std::nullopt});
                }
                if (sel == SIZE_MAX) {
                    sel = opts.size();
                    shown->push_back(v);
                    opts.push_back({"Custom: " + config::input_text(v, sp->type), "", std::nullopt});
                }
                cp->set_options(std::move(opts), sel);
            };
            ctl = std::move(c);
            break;
        }
        case Control::FontFamily: {
            auto c = std::make_unique<Dropdown>(std::vector<DropdownOption>{}, size_t{0}, "control");
            Dropdown* cp = c.get();
            auto fams = std::make_shared<std::vector<std::string>>();
            fams->push_back("");
            if (env_.font_families)
                for (auto& f : env_.font_families()) fams->push_back(f);
            c->on_change = [commit, fams](size_t i) {
                if (i < fams->size()) commit(Value((*fams)[i]));
            };
            b->show = [cp, fams](const Value& v) {
                const std::string cur = std::get_if<std::string>(&v) ? std::get<std::string>(v) : std::string();
                std::vector<DropdownOption> opts;
                size_t sel = 0;
                bool have = false;
                for (size_t i = 0; i < fams->size(); ++i) {
                    opts.push_back({i == 0 ? std::string("System Monospace") : (*fams)[i], i == 0 ? "Default" : "", std::nullopt});
                    if ((*fams)[i] == cur) {
                        sel = i;
                        have = true;
                    }
                }
                if (!have) {
                    fams->push_back(cur);
                    opts.push_back({cur, "Not installed", std::nullopt});
                    sel = opts.size() - 1;
                }
                cp->set_options(std::move(opts), sel);
            };
            ctl = std::move(c);
            break;
        }
        case Control::Slider:
        case Control::Percent: {
            const double lo = s.range ? s.range->lo : 0, hi = s.range ? s.range->hi : 1;
            const double step = r.step > 0 ? r.step : (hi - lo) / 100;
            auto c = std::make_unique<Slider>(lo, hi, step, lo, "control");
            Slider* cp = c.get();
            if (kind == Control::Percent) c->set_format(percent);
            else c->set_format([step](double v) { return trim_number(v, step); });
            c->style().width = 240.0f;
            c->on_commit = [commit](double v) { commit(Value(v)); };
            b->show = [cp](const Value& v) {
                if (auto* d = std::get_if<double>(&v)) cp->set_value(*d);
                else if (auto* i = std::get_if<std::int64_t>(&v)) cp->set_value(static_cast<double>(*i));
            };
            ctl = std::move(c);
            break;
        }
        case Control::Stepper: {
            const double lo = s.range ? s.range->lo : 0, hi = s.range ? s.range->hi : 1e9;
            const double step = r.step > 0 ? r.step : 1;
            auto c = std::make_unique<Stepper>(lo, hi, step, lo, "control");
            Stepper* cp = c.get();
            const bool is_int = s.type == ValueType::Int;
            c->set_format([step](double v) { return trim_number(v, step); });
            c->on_change = [commit, is_int](double v) {
                commit(is_int ? Value(static_cast<std::int64_t>(std::llround(v))) : Value(v));
            };
            b->show = [cp](const Value& v) {
                if (auto* d = std::get_if<double>(&v)) cp->set_value(*d);
                else if (auto* i = std::get_if<std::int64_t>(&v)) cp->set_value(static_cast<double>(*i));
            };
            ctl = std::move(c);
            break;
        }
        case Control::Color: {
            auto c = std::make_unique<ColorWell>("", "control");
            ColorWell* cp = c.get();
            c->set_presets({{"Black", Color::rgb(0x000000)}, {"White", Color::rgb(0xFFFFFF)}, {"Red", Color::rgb(0xE06C75)},
                            {"Green", Color::rgb(0x98C379)}, {"Yellow", Color::rgb(0xE5C07B)}, {"Blue", Color::rgb(0x61AFEF)},
                            {"Magenta", Color::rgb(0xC678DD)}, {"Cyan", Color::rgb(0x56B6C2)}, {"Orange", Color::rgb(0xF0A050)}});
            c->on_change = [this, k, bp](const std::string& hex) {
                auto v = config::parse_input(*bp->spec, hex);
                if (!v) {
                    bp->local_error = v.error().what;
                    bp->row->set_error(bp->local_error);
                    return;
                }
                (void)set_value(k, *v);
            };
            bp->field = &c->field();
            b->show = [cp](const Value& v) {
                if (auto* str = std::get_if<std::string>(&v)) cp->set_value(*str);
            };
            ctl = std::move(c);
            break;
        }
        case Control::Text:
        case Control::WideText:
        default: {
            wide = kind == Control::WideText;
            const bool char_set = is_char_set_key(r.key) && s.type == ValueType::String;
            auto field_text = [char_set, type = s.type](const Value& v) {
                const auto* str = std::get_if<std::string>(&v);
                return char_set && str ? char_set_shown(*str) : config::input_text(v, type);
            };
            auto f = std::make_unique<CommitField>(field_text(s.default_value), "control");
            CommitField* fp = f.get();
            if (!wide) f->style().width = 220.0f;
            auto submit = [this, k, bp, fp, char_set] {
                const std::string text = fp->text();
                auto v = char_set ? Result<Value>(Value{char_set_value(text)}) : config::parse_input(*bp->spec, text);
                if (!v) {
                    bp->local_error = v.error().what;
                    bp->row->set_error(bp->local_error);
                    return;
                }
                if (*v == cfg_.value(k) && cfg_.origin(k).layer != config::Layer::Default) {
                    bp->local_error.clear();
                    bp->row->set_error({});
                    return;
                }
                if (*v == cfg_.value(k) && cfg_.origin(k).layer == config::Layer::Default) return;
                (void)set_value(k, *v);
            };
            f->on_submit = [submit](const std::string&) { submit(); };
            f->on_blur = submit;
            f->on_cancel = [this, k, fp, bp, field_text] {
                fp->set_text(field_text(cfg_.value(k)));
                bp->local_error.clear();
                bp->row->set_error({});
            };
            b->field = fp;
            b->show = [fp, field_text](const Value& v) { fp->set_text(field_text(v)); };
            ctl = std::move(f);
            break;
        }
    }
    ctl->set_tooltip(std::string(s.doc));
    auto row = std::make_unique<FormRow>(std::string(r.label), std::string(r.description), std::move(ctl), std::string(r.key), wide);
    row->set_tooltip(std::string(s.path) + "\n" + std::string(s.doc));
    b->row = row.get();
    row->on_context = [this, k, bp](Vec2 at) {
        Window* w = view_ ? view_->window() : nullptr;
        if (!w) return;
        std::vector<MenuItem> items;
        MenuItem reset;
        reset.label = "Reset to Default";
        reset.id = "reset";
        reset.enabled = cfg_.origin(k).layer == config::Layer::User;
        reset.action = [this, k] { (void)reset_value(k); };
        items.push_back(std::move(reset));
        MenuItem copy;
        copy.label = "Copy Key Name";
        copy.id = "copy";
        copy.action = [w, path = std::string(bp->spec->path)] {
            if (w->set_clipboard) w->set_clipboard(path);
        };
        items.push_back(std::move(copy));
        open_menu(*w, std::move(items), at);
    };
    bound_.push_back(std::move(b));
    return row;
}

// ---------------------------------------------------------------- writes

void SettingsController::write_done(KeyId k, const Result<void>& r) {
    for (auto& b : bound_) {
        if (b->spec->id != k) continue;
        b->local_error = r ? std::string() : r.error().what;
        b->row->set_error(b->local_error);
    }
}

Result<void> SettingsController::set_value(KeyId k, const Value& v) {
    if (read_only_) return make_error(Errc::invalid_argument, read_only_reason_);
    auto r = writer_.set(k, v);
    write_done(k, r);
    return r;
}

Result<void> SettingsController::reset_value(KeyId k) {
    if (read_only_) return make_error(Errc::invalid_argument, read_only_reason_);
    auto r = writer_.reset(k);
    write_done(k, r);
    return r;
}

// ---------------------------------------------------------------- refresh

void SettingsController::refresh(const config::Config& c) {
    cfg_ = c;
    keymap_ = commands::Keymap::from_config(cfg_, env_.os).keymap;
    if (!view_) return;
    update_banner();
    update_rows();
    update_themes();
    update_keymaps();
    update_agents();
}

void SettingsController::update_rows() {
    std::map<std::string, std::string, std::less<>> diag;
    for (const auto& d : cfg_.diagnostics()) {
        if (d.key.empty() || d.level == config::Diagnostic::Level::Info) continue;
        diag.emplace(d.key, d.message + (d.hint.empty() ? std::string() : " (" + d.hint + ")"));
    }
    for (auto& b : bound_) {
        const KeyId k = b->spec->id;
        if (!(b->field && b->field->focused())) b->show(cfg_.value(k));
        std::string err = b->local_error;
        if (err.empty())
            if (auto it = diag.find(b->spec->path); it != diag.end()) err = it->second;
        b->row->set_error(err);
        b->row->set_note(origin_note(cfg_.origin(k)));
        const bool dim = !depends_met(cfg_, b->def->depends);
        b->row->set_dimmed(dim || read_only_);
    }
}

void SettingsController::update_banner() {
    if (!view_) return;
    auto w = writer_.writable();
    read_only_ = !w;
    read_only_reason_ = w ? std::string() : w.error().what;
    if (read_only_) {
        view_->set_banner(read_only_reason_, Tone::failure);
        return;
    }
    size_t n = 0;
    std::string first;
    for (const auto& d : cfg_.diagnostics()) {
        if (d.level == config::Diagnostic::Level::Info) continue;
        if (n++ == 0) first = config::format(d);
    }
    if (n == 0) view_->set_banner({});
    else if (n == 1) view_->set_banner(first, Tone::warning);
    else view_->set_banner(first + " (and " + std::to_string(n - 1) + " more; see keepterm --check-config)", Tone::warning);
}

void SettingsController::rebuild_later() {
    Window* w = view_ ? view_->window() : nullptr;
    if (!w) return;
    std::weak_ptr<bool> alive = alive_;
    w->add_timer(Duration::zero(), [this, alive] {
        if (alive.expired() || !view_) return;
        const std::string p = pane_;
        pane_.clear();
        show_pane(p);
    });
}

// ---------------------------------------------------------------- Appearance: themes

void SettingsController::add_appearance(Widget& page) {
    theme_ = std::make_unique<Theme>();
    Theme& t = *theme_;
    if (env_.themes) t.all = env_.themes->list();
    const auto sel = theme::selection(cfg_);
    t.dark_side = theme::effective_appearance(cfg_, env_.os_dark ? theme::Appearance::dark : theme::Appearance::light) ==
                  theme::Appearance::dark;

    auto g = std::make_unique<FormGroup>("Color Theme", "themes");
    auto top = row("theme_header", 10);
    top->style().padding = Insets::hv(12, 10);
    top->style().align_items = LayoutStyle::Align::Center;
    top->emplace<Label>("Editing", TextStyle::body, Tone::normal, "label");
    auto side = std::make_unique<SegmentedControl>(std::vector<std::string>{"Dark Theme", "Light Theme"}, t.dark_side ? size_t{0} : size_t{1}, "side");
    t.side = side.get();
    side->on_change = [this](size_t i) {
        if (!theme_) return;
        theme_->dark_side = i == 0;
        update_themes();
    };
    top->add(std::move(side));
    t.chosen = &top->emplace<Label>("", TextStyle::small, Tone::muted, "chosen");
    t.chosen->style().grow = 1;
    auto filter = std::make_unique<TextField>("Filter themes", "filter");
    filter->style().width = 160.0f;
    TextField* fp = filter.get();
    fp->on_change = [this](const std::string& q) {
        if (!theme_) return;
        theme_->filter = q;
        update_themes();
    };
    top->add(std::move(filter));
    g->body().add_child(std::move(top));

    auto prev = std::make_unique<SchemePreview>("preview");
    prev->style().margin = Insets::hv(12, 4);
    t.preview = prev.get();
    g->body().add_child(std::move(prev));

    auto list = std::make_unique<RevealList>();
    list->style().grow = 1;
    list->set_row_height(26);
    t.list = list.get();
    list->set_paint_row([this](size_t i, PaintContext& p, Rect r, ListView::RowState st) {
        if (!theme_ || i >= theme_->shown.size()) return;
        const ui::Theme& th = p.theme;
        const theme::ThemeInfo& info = theme_->all[theme_->shown[i]];
        const bool cur = info.name == theme_->current;
        const Rect pill = r.inset(Insets::hv(4, 1));
        if (st.selected || cur) p.dl.round_rect(pill, th.radius_control, st.focused && st.selected ? th.accent.with_alpha(0.25f) : th.selection);
        else if (st.hovered) p.dl.round_rect(pill, th.radius_control, th.hover);
        if (env_.themes) {
            if (const theme::Theme* tt = env_.themes->find(info.name)) {
                const Rect sw{pill.x + 8, pill.y + (pill.h - 14) / 2, 28, 14};
                p.dl.round_rect(sw, 3, color_of(tt->background), 1, th.border);
                for (int c = 0; c < 4; ++c)
                    p.dl.rect(Rect{sw.x + 4 + static_cast<float>(c) * 5.5f, sw.y + 4, 4, 6}, color_of(tt->palette[static_cast<size_t>(c + 1)]));
            }
        }
        auto run = p.text.ellipsize(info.name, role_for(th, TextStyle::body), p.scale, pill.w - 140);
        p.dl.text(run, {pill.x + 46, std::round(pill.y + (pill.h - run->height()) / 2 + run->ascent())}, th.fg);
        std::string tag = std::string(theme::to_string(info.appearance));
        if (!info.builtin) tag += ", custom";
        auto tr = p.text.shape(tag, role_for(th, TextStyle::small), p.scale);
        p.dl.text(tr, {pill.right() - tr->width() - (cur ? 28 : 10), std::round(pill.y + (pill.h - tr->height()) / 2 + tr->ascent())}, th.fg_muted);
        if (cur) p.dl.icon(Icon::check, 14, {pill.right() - 22, pill.y + (pill.h - 14) / 2}, th.accent);
    });
    list->on_activate = [this](size_t i) {
        if (!theme_ || i >= theme_->shown.size()) return;
        (void)choose_theme(theme_->all[theme_->shown[i]].name, theme_->dark_side);
    };
    list->on_select = list->on_activate;
    // The list paints its rows itself; the frame clips the partly scrolled ones.
    auto frame = std::make_unique<Box>("list_frame", LayoutStyle::Dir::Column);
    frame->style().height = 208.0f;
    frame->style().margin = Insets{4, 4, 4, 6};
    frame->style().clip = true;
    frame->add(std::move(list));
    g->body().add_child(std::move(frame));
    g->set_footnote("Themes come with keepterm and from the themes folder next to config.toml.");
    page.add_child(std::move(g));
    (void)sel;
    update_themes();
}

void SettingsController::update_themes() {
    if (!theme_) return;
    Theme& t = *theme_;
    const auto sel = theme::selection(cfg_);
    t.current = t.dark_side ? sel.dark : sel.light;
    t.side->set_selected(t.dark_side ? 0 : 1);
    t.shown.clear();
    for (size_t i = 0; i < t.all.size(); ++i) {
        if (!t.filter.empty() && !contains_folded(t.all[i].name, t.filter)) continue;
        t.shown.push_back(i);
    }
    // The side's own appearance first, then the rest.
    std::stable_sort(t.shown.begin(), t.shown.end(), [&](size_t a, size_t b) {
        const auto want = t.dark_side ? theme::Appearance::dark : theme::Appearance::light;
        return (t.all[a].appearance == want) > (t.all[b].appearance == want);
    });
    t.list->set_count(t.shown.size());
    std::optional<size_t> at;
    for (size_t i = 0; i < t.shown.size(); ++i)
        if (t.all[t.shown[i]].name == t.current) at = i;
    if (t.list->selected() != at) t.list->set_selected(at, t.list->frame().h > 0);
    t.list->invalidate_paint();
    t.chosen->set_text(t.current.empty() ? std::string() : t.current);
    SchemeColors sc;
    sc.name = t.current;
    const theme::Theme* th = env_.themes ? env_.themes->find(t.current) : nullptr;
    if (th) {
        sc.background = color_of(th->background);
        sc.foreground = color_of(th->foreground);
        sc.cursor = color_of(th->cursor);
        sc.selection = color_of(th->selection_background);
        for (size_t i = 0; i < 16; ++i) sc.palette[i] = color_of(th->palette[i]);
    } else {
        sc.background = Color::rgb(0x302F3A);
        sc.foreground = Color::rgb(0xE0DEF4);
        sc.cursor = sc.foreground;
        sc.selection = Color::rgb(0x33405A);
        for (auto& c : sc.palette) c = sc.foreground;
    }
    t.preview->set_scheme(std::move(sc));
}

Result<void> SettingsController::choose_theme(const std::string& name, bool dark_side) {
    if (read_only_) return make_error(Errc::invalid_argument, read_only_reason_);
    // theme.name replaces both sides; split it into theme.light / theme.dark first.
    if (!cfg_.theme_name().empty()) {
        const auto sel = theme::selection(cfg_);
        if (auto r = writer_.reset(KeyId::theme_name); !r) return r;
        if (auto r = writer_.set(KeyId::theme_light, Value(dark_side ? sel.light : name)); !r) return r;
        return writer_.set(KeyId::theme_dark, Value(dark_side ? name : sel.dark));
    }
    return writer_.set(dark_side ? KeyId::theme_dark : KeyId::theme_light, Value(name));
}

// ---------------------------------------------------------------- Keymaps

void SettingsController::add_keymaps(Widget& page) {
    auto filter = std::make_unique<SearchField>("Search by action or keybind", "key_search");
    SearchField* fp = filter.get();
    fp->set_text(key_filter_);
    fp->on_change = [this](const std::string& q) {
        key_filter_ = q;
        update_keymaps();
    };
    page.add_child(std::move(filter));
    auto list = column("commands", 18);
    key_list_ = list.get();
    page.add_child(std::move(list));
    update_keymaps();
}

void SettingsController::update_keymaps() {
    if (!key_list_) return;
    const commands::CommandRegistry& reg = commands::builtin_registry();
    // Conflicts per command: "Conflicts with Split Right (⌘D)".
    std::map<std::string, std::string, std::less<>> conflict;
    const auto bs = keymap_.bindings();
    for (const auto& c : keymap_.conflicts()) {
        if (c.winner >= bs.size() || c.loser >= bs.size()) continue;
        const auto& a = bs[c.winner];
        const auto& b = bs[c.loser];
        if (a.command == b.command) continue;
        const std::string chord = commands::format_sequence(a.seq, env_.os, commands::Format::Text);
        auto title = [&](const commands::Binding& x) { return x.cmd ? reg.title(*x.cmd) : std::string(x.command); };
        // A user binding over a default takes the chord away from the default's command.
        if (c.shadowed_default) {
            conflict[std::string(a.command)] = "Conflicts with " + title(b) + " (" + chord + "), which no longer has it.";
            conflict[std::string(b.command)] = "Conflicts with " + title(a) + ", which took " + chord + ".";
        } else {
            conflict[std::string(a.command)] = "Conflicts with " + title(b) + " (" + chord + "); this binding wins.";
            conflict[std::string(b.command)] = "Conflicts with " + title(a) + " (" + chord + "), which wins.";
        }
    }
    // The rows: build once per filter, update in place otherwise.
    std::vector<const commands::Command*> want;
    for (const auto& c : reg.all()) {
        if (!c.bindable) continue;
        auto caps = keymap_.key_caps(c.id);
        const std::string text = caps ? commands::format_sequence(*caps, env_.os, commands::Format::Text) : std::string();
        const std::string words = caps ? commands::format_sequence(*caps, env_.os, commands::Format::Words) : std::string();
        if (!key_filter_.empty() && !matches(key_filter_, {reg.title(c), c.id, text, words, commands::category_title(c.category)})) continue;
        want.push_back(&c);
    }
    bool same = want.size() == key_rows_.size();
    for (size_t i = 0; same && i < want.size(); ++i) same = key_rows_[i].command == want[i]->id;
    if (!same) {
        key_rows_.clear();
        key_list_->clear_children();
        std::map<commands::Category, FormGroup*> groups;
        for (const commands::Command* c : want) {
            FormGroup*& g = groups[c->category];
            if (!g) g = &key_list_->add(std::make_unique<FormGroup>(std::string(commands::category_title(c->category)),
                                                                    lower(commands::category_title(c->category))));
            auto line = row("keys", 6);
            line->style().align_items = LayoutStyle::Align::Center;
            auto rec = std::make_unique<KeyRecorder>("", "recorder");
            KeyRecorder* rp = rec.get();
            const std::string id(c->id);
            rec->on_record = [this, id](const platform::KeyEvent& e) { (void)bind(id, e); };
            rec->on_clear = [this, id] { (void)unbind(id); };
            line->add(std::move(rec));
            auto more = Button::icon_button(Icon::more, "More", {}, "more", 22);
            Button* mp = more.get();
            mp->set_on_click([this, id, mp] {
                Window* w = view_ ? view_->window() : nullptr;
                if (!w) return;
                std::vector<MenuItem> items;
                MenuItem rec_item;
                rec_item.label = "Record Shortcut";
                rec_item.id = "record";
                rec_item.action = [this, id] {
                    for (auto& kr : key_rows_)
                        if (kr.command == id) kr.rec->start();
                };
                items.push_back(std::move(rec_item));
                MenuItem un;
                un.label = "Remove Shortcut";
                un.id = "unbind";
                un.action = [this, id] { (void)unbind(id); };
                items.push_back(std::move(un));
                MenuItem rs;
                rs.label = "Reset to Default";
                rs.id = "reset";
                bool user = false;
                for (const auto& kb : cfg_.keys()) user = user || kb.command == id;
                rs.enabled = user;
                rs.action = [this, id] { (void)reset_binding(id); };
                items.push_back(std::move(rs));
                open_menu_below(*w, std::move(items), mp->bounds_in_window());
            });
            line->add(std::move(more));
            auto fr = std::make_unique<FormRow>(reg.title(*c), std::string(c->description), std::move(line), id);
            fr->set_tooltip(id);
            FormRow* frp = &g->add_row(std::move(fr));
            key_rows_.push_back({id, frp, rp});
        }
        if (want.empty())
            key_list_->emplace<Label>("No commands match \xE2\x80\x9C" + key_filter_ + "\xE2\x80\x9D.", TextStyle::body, Tone::muted, "empty");
    }
    for (auto& kr : key_rows_) {
        auto caps = keymap_.key_caps(kr.command);
        kr.rec->set_caps(caps ? commands::format_sequence(*caps, env_.os, commands::Format::Text) : std::string());
        auto it = conflict.find(kr.command);
        kr.rec->set_conflict(it != conflict.end());
        std::string note = it != conflict.end() ? it->second : std::string();
        if (caps && note.empty() && commands::is_reserved_chord(caps->chords[0], env_.os))
            note = commands::reserved_warning(caps->chords[0], env_.os);
        if (kr.row->error().empty() || !note.empty()) kr.row->set_note(note, Tone::warning);
    }
}

Result<void> SettingsController::bind(std::string_view command, const platform::KeyEvent& e) {
    auto row_of = [&]() -> FormRow* {
        for (auto& kr : key_rows_)
            if (kr.command == command) return kr.row;
        return nullptr;
    };
    auto fail = [&](std::string msg) -> Result<void> {
        if (FormRow* r = row_of()) r->set_error(msg);
        return make_error(Errc::invalid_argument, msg);
    };
    if (read_only_) return fail(read_only_reason_);
    const commands::EventChords ch = commands::map_event(e, env_.os);
    commands::Chord c = ch.logical.key.code() != 0 ? ch.logical : ch.physical;
    if (ch.modifier_key || c.key.code() == 0) return fail("That key cannot be bound.");
    const bool fkey = c.key.code() != 0 && commands::format_chord(commands::Chord{0, c.key}, env_.os, commands::Format::Config).starts_with("f");
    if (c.mods == 0 && !(fkey && commands::format_chord(commands::Chord{0, c.key}, env_.os, commands::Format::Config).size() <= 3))
        return fail("Add a modifier key such as " + std::string(env_.os == commands::Os::MacOS ? "Cmd" : "Ctrl") + ".");
    const std::string text = commands::format_sequence(commands::Sequence::of(c), env_.os, commands::Format::Config);
    auto r = writer_.set_binding(command, "\"" + text + "\"");
    if (!r) return fail(r.error().what);
    if (FormRow* row = row_of()) row->set_error({});
    return r;
}

Result<void> SettingsController::unbind(std::string_view command) {
    if (read_only_) return make_error(Errc::invalid_argument, read_only_reason_);
    auto r = writer_.set_binding(command, "\"none\"");
    for (auto& kr : key_rows_)
        if (kr.command == command) kr.row->set_error(r ? std::string() : r.error().what);
    return r;
}

Result<void> SettingsController::reset_binding(std::string_view command) {
    if (read_only_) return make_error(Errc::invalid_argument, read_only_reason_);
    auto r = writer_.reset_binding(command);
    for (auto& kr : key_rows_)
        if (kr.command == command) kr.row->set_error(r ? std::string() : r.error().what);
    return r;
}

// ---------------------------------------------------------------- Agents

void SettingsController::add_agents(Widget& page) {
    auto g = std::make_unique<FormGroup>("Recognized Agents", "recognized");
    const agents::AgentRegistry reg = agents::AgentRegistry::builtin();
    for (const auto& d : reg.all()) {
        const std::string id(d.id);
        std::unique_ptr<Widget> ctl;
        Toggle* tp = nullptr;
        if (d.resumable()) {
            auto t = std::make_unique<Toggle>(true, [this, id](bool on) { (void)set_agent_restore(id, on); }, "control");
            tp = t.get();
            ctl = std::move(t);
        } else {
            ctl = std::make_unique<Label>("Icon only", TextStyle::small, Tone::muted, "control");
        }
        const std::string desc = d.resumable() ? "Resumes with \xE2\x80\x9C" + std::string(d.command) + "\xE2\x80\x9D after a restart."
                                               : "Shown with its icon; keepterm cannot resume its sessions.";
        auto fr = std::make_unique<FormRow>(std::string(d.display_name), desc, std::move(ctl), id);
        FormRow* frp = &g->add_row(std::move(fr));
        agent_rows_.push_back({id, tp, frp});
    }
    g->set_footnote("Custom agents are defined with [[agents.custom]] in config.toml.");
    page.add_child(std::move(g));

    // Claude Code's statusLine relay (agents/context_hook.h).
    auto cg = std::make_unique<FormGroup>("Context", "context");
    std::optional<agents::HookStatus> hs;
    if (env_.statusline_hook) hs = env_.statusline_hook()->status();
    auto t = std::make_unique<Toggle>(false, [this](bool on) { (void)set_claude_statusline(on); }, "control");
    hook_toggle_ = t.get();
    auto fr = std::make_unique<FormRow>("Claude Code: exact context via status line",
                                        hs ? "Edits " + hs->settings_file : std::string(), std::move(t), "claude_statusline");
    hook_row_ = &cg->add_row(std::move(fr));
    cg->set_footnote("Claude Code then reports its own context figures to keepterm; a status line you already use "
                     "keeps running and is restored when this is turned off.");
    page.add_child(std::move(cg));
    if (hs) {
        update_statusline_row(*hs);
    } else {
        hook_row_->set_note("Not available in this build.");
        hook_row_->set_dimmed(true);
    }
    update_agents();
}

void SettingsController::update_statusline_row(const agents::HookStatus& s) {
    if (!hook_row_ || !hook_toggle_) return;
    const bool on = s.state == agents::HookState::Installed || s.state == agents::HookState::Repaired ||
                    s.state == agents::HookState::Stale;
    hook_toggle_->set_on(on, false);
    hook_row_->set_note(agents::describe(s));
    hook_row_->set_error(s.error);
}

Result<agents::HookStatus> SettingsController::set_claude_statusline(bool on) {
    if (!env_.statusline_hook) return make_error(Errc::not_supported, "Not available in this build.");
    auto hook = env_.statusline_hook();
    auto r = on ? hook->install() : hook->uninstall();
    agents::HookStatus s = hook->status();
    if (!r) s.error = r.error().what;
    else if (r->state == agents::HookState::Repaired) s.state = agents::HookState::Repaired;
    update_statusline_row(s);
    if (!r) return Unexpected(r.error());
    return s;
}

void SettingsController::update_agents() {
    const auto& skip = cfg_.agents_restore_skip();
    const bool on = cfg_.agents_restore();
    for (auto& a : agent_rows_) {
        if (!a.toggle) continue;
        const bool skipped = std::find(skip.begin(), skip.end(), a.id) != skip.end();
        a.toggle->set_on(on && !skipped, false);
        a.row->set_dimmed(!on || read_only_);
    }
}

Result<void> SettingsController::set_agent_restore(std::string_view agent, bool on) {
    std::vector<std::string> skip = cfg_.agents_restore_skip();
    auto it = std::find(skip.begin(), skip.end(), agent);
    if (on && it != skip.end()) skip.erase(it);
    if (!on && it == skip.end()) skip.emplace_back(agent);
    if (skip.empty()) return reset_value(KeyId::agents_restore_skip);
    return set_value(KeyId::agents_restore_skip, Value(skip));
}

// ---------------------------------------------------------------- Storage

void SettingsController::add_storage(Widget& page) {
    auto g = std::make_unique<FormGroup>("Usage", "usage");
    if (!env_.storage_usage) {
        g->set_footnote("The session store is not open.");
        page.add_child(std::move(g));
        return;
    }
    auto u = env_.storage_usage();
    if (!u) {
        g->add_row(std::make_unique<FormRow>("Session store", u.error().what, nullptr, "error"));
        page.add_child(std::move(g));
        return;
    }
    auto bar = [](const store::LimitUsage& l, std::string text) {
        auto c = column("usage", 4);
        c->style().width = 260.0f;
        auto gauge = std::make_unique<Gauge>("bar");
        gauge->set_fraction(l.limit ? static_cast<float>(std::min(1.0, l.percent / 100)) : 0.0f);
        gauge->style().align_self = LayoutStyle::Align::Stretch;
        c->add(std::move(gauge));
        c->emplace<Label>(std::move(text), TextStyle::small, l.percent >= 95 ? Tone::failure : l.percent >= 80 ? Tone::warning : Tone::muted,
                          "text");
        return c;
    };
    auto sz = [](uint64_t b) {
        static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
        double v = static_cast<double>(b);
        size_t unit = 0;
        while (v >= 1024 && unit + 1 < std::size(kUnits)) {
            v /= 1024;
            ++unit;
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, unit == 0 || v >= 100 ? "%.0f %s" : "%.1f %s", v, kUnits[unit]);
        return std::string(buf);
    };
    g->add_row(std::make_unique<FormRow>("All sessions", "", bar(u->total, sz(u->total.used) + " used / " + sz(u->total.remaining) + " remaining of " + sz(u->total.limit)), "total"));
    if (u->disk.capacity)
        g->add_row(std::make_unique<FormRow>("Disk", "Writes stop below the free space limit.",
                                             bar(u->disk.fill, sz(u->disk.available) + " free of " + sz(u->disk.capacity) +
                                                                   (u->disk.below ? " \xE2\x80\x94 writes stopped" : "")),
                                             "disk"));
    static constexpr const char* kKindTitles[] = {"Raw output", "Text", "Commands", "Events"};
    for (size_t i = 0; i < store::kKindCount; ++i) {
        store::LimitUsage l;
        l.used = u->kind_bytes[i];
        l.limit = u->total.used ? u->total.used : 1;
        l.percent = u->total.used ? 100.0 * static_cast<double>(u->kind_bytes[i]) / static_cast<double>(u->total.used) : 0;
        g->add_row(std::make_unique<FormRow>(kKindTitles[i], "", bar(l, sz(u->kind_bytes[i])),
                                             std::string(store::to_string(static_cast<store::Kind>(i)))));
    }
    {
        auto l = std::make_unique<Label>(sz(u->index_bytes + u->catalog_bytes), TextStyle::small, Tone::muted, "control");
        g->add_row(std::make_unique<FormRow>("Indexes and catalog", "", std::move(l), "index"));
    }
    if (u->pinned_bytes) g->set_footnote("Pinned sessions (" + sz(u->pinned_bytes) + ") are never removed by the limits.");
    page.add_child(std::move(g));

    if (u->sessions.empty()) return;
    auto sg = std::make_unique<FormGroup>("Largest Sessions", "sessions");
    size_t n = 0;
    for (const auto& s : u->sessions) {
        if (n++ >= 8) break;
        auto line = row("actions", 6);
        line->style().align_items = LayoutStyle::Align::Center;
        line->emplace<Label>(sz(s.cap.used), TextStyle::small, Tone::muted, "size");
        const std::string id = s.id;
        if (env_.pin_session) {
            line->add(std::make_unique<Button>(s.pinned ? "Unpin" : "Pin", [this, id, pinned = s.pinned] {
                if (auto r = env_.pin_session(id, !pinned); !r) {
                    if (Window* w = view_ ? view_->window() : nullptr) show_toast(*w, ToastSpec{r.error().what, {}, Icon::alert, Tone::failure});
                }
                rebuild_later();
            }, Button::Kind::secondary, "pin"));
        }
        if (env_.forget_session && !s.live) {
            line->add(std::make_unique<Button>("Forget", [this, id] {
                Window* w = view_ ? view_->window() : nullptr;
                if (!w) return;
                DialogSpec d;
                d.title = "Forget this session?";
                d.message = "Its recorded output and history are deleted. This cannot be undone.";
                d.buttons.push_back({"Cancel", {}, Button::Kind::secondary, false, true});
                d.buttons.push_back({"Forget", [this, id] {
                                         if (auto r = env_.forget_session(id); !r) {
                                             if (Window* ww = view_ ? view_->window() : nullptr) show_toast(*ww, ToastSpec{r.error().what, {}, Icon::alert, Tone::failure});
                                         }
                                         rebuild_later();
                                     },
                                     Button::Kind::danger, true, false});
                show_dialog(*w, std::move(d));
            }, Button::Kind::danger, "forget"));
        }
        std::string title = s.project.empty() ? std::string("No project") : s.project;
        std::string desc = s.id.substr(0, std::min<size_t>(12, s.id.size()));
        if (s.live) desc += " \xC2\xB7 live";
        if (s.pinned) desc += " \xC2\xB7 pinned";
        sg->add_row(std::make_unique<FormRow>(std::move(title), std::move(desc), std::move(line), "session[" + s.id + "]"));
    }
    page.add_child(std::move(sg));
}

// ---------------------------------------------------------------- History

namespace {

std::string size_text(uint64_t b) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(b);
    size_t unit = 0;
    while (v >= 1024 && unit + 1 < std::size(kUnits)) {
        v /= 1024;
        ++unit;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, unit == 0 || v >= 100 ? "%.0f %s" : "%.1f %s", v, kUnits[unit]);
    return buf;
}

std::string sessions_text(size_t n) { return std::to_string(n) + (n == 1 ? " closed session" : " closed sessions"); }

void failure_toast(Window* w, const std::string& what) {
    if (w) show_toast(*w, ToastSpec{what, {}, Icon::alert, Tone::failure});
}

}  // namespace

void SettingsController::commit_choice(KeyId k, const Value& v) {
    Window* w = view_ ? view_->window() : nullptr;
    if (k != KeyId::storage_keep_closed || !env_.history_due || !w) {
        (void)set_value(k, v);
        return;
    }
    const auto* ms = std::get_if<std::int64_t>(&v);
    HistoryDeletion d = env_.history_due(ms ? *ms : 0);
    if (d.ids.empty()) {
        (void)set_value(k, v);
        return;
    }
    // Nothing is deleted before the confirmation; Cancel shows the old value again.
    DialogSpec dlg;
    dlg.title = "Delete " + sessions_text(d.ids.size()) + "?";
    dlg.message = "This deletes " + sessions_text(d.ids.size()) + " (" + size_text(d.stored_bytes) +
                  ") now. Pinned sessions stay.";
    dlg.buttons.push_back({"Cancel", [this] { update_rows(); }, Button::Kind::secondary, false, true});
    dlg.buttons.push_back({"Delete", [this, k, v, ids = std::move(d.ids)] {
                               if (!set_value(k, v)) return;
                               if (env_.delete_history) {
                                   if (auto r = env_.delete_history(ids); !r) failure_toast(view_ ? view_->window() : nullptr, r.error().what);
                               }
                               rebuild_later();
                           },
                           Button::Kind::danger, true, false});
    show_dialog(*w, std::move(dlg));
}

void SettingsController::add_history(Widget& page) {
    if (!env_.history_summary) return;
    auto g = std::make_unique<FormGroup>("Store", "store");
    auto sum = env_.history_summary();
    if (!sum) {
        g->add_row(std::make_unique<FormRow>("Session history", sum.error().what, nullptr, "error"));
        page.add_child(std::move(g));
        return;
    }
    std::string figures = size_text(sum->raw_bytes) + " of output in " + size_text(sum->stored_bytes) + " \xC2\xB7 " +
                          std::to_string(sum->sessions) + (sum->sessions == 1 ? " session" : " sessions");
    if (sum->pinned) figures += " \xC2\xB7 " + std::to_string(sum->pinned) + " pinned";
    g->add_row(std::make_unique<FormRow>("Recorded", "", std::make_unique<Label>(figures, TextStyle::small, Tone::muted, "control"), "figures"));
    auto line = row("actions", 6);
    if (env_.open_session_history) {
        line->add(std::make_unique<Button>("Open Session History", [this] { env_.open_session_history(); }, Button::Kind::secondary, "open"));
    }
    if (env_.history_closed_before && env_.delete_history) {
        line->add(std::make_unique<Button>("Delete History\xE2\x80\xA6", [this] { delete_history_dialog(); }, Button::Kind::danger, "delete"));
    }
    g->add_row(std::make_unique<FormRow>("", "", std::move(line), "buttons"));
    page.add_child(std::move(g));
}

void SettingsController::delete_history_dialog() {
    Window* w = view_ ? view_->window() : nullptr;
    if (!w) return;
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    constexpr uint64_t kMonth = 30ull * 24 * 3600 * 1000;
    HistoryDeletion old = env_.history_closed_before(now > kMonth ? now - kMonth : 1);
    HistoryDeletion all = env_.history_closed_before(0);
    DialogSpec dlg;
    dlg.title = "Delete History";
    if (all.ids.empty()) {
        dlg.message = "There are no closed sessions to delete. Running and pinned sessions are never deleted here.";
        dlg.buttons.push_back({"OK", {}, Button::Kind::primary, true, true});
        show_dialog(*w, std::move(dlg));
        return;
    }
    dlg.message = "Closed more than a month ago: " + sessions_text(old.ids.size()) + " (" + size_text(old.stored_bytes) + ").\n" +
                  "All closed: " + sessions_text(all.ids.size()) + " (" + size_text(all.stored_bytes) + ").\n" +
                  "Running and pinned sessions stay. This cannot be undone.";
    auto del = [this](std::vector<std::string> ids) {
        return [this, ids = std::move(ids)] {
            if (auto r = env_.delete_history(ids); !r) failure_toast(view_ ? view_->window() : nullptr, r.error().what);
            rebuild_later();
        };
    };
    dlg.buttons.push_back({"Cancel", {}, Button::Kind::secondary, false, true});
    if (!old.ids.empty() && old.ids.size() < all.ids.size()) {
        dlg.buttons.push_back({"Older Than a Month", del(std::move(old.ids)), Button::Kind::danger, false, false});
    }
    dlg.buttons.push_back({"All Closed", del(std::move(all.ids)), Button::Kind::danger, false, false});
    dlg.width = 440;
    show_dialog(*w, std::move(dlg));
}

// ---------------------------------------------------------------- Advanced

void SettingsController::add_advanced(Widget& page) {
    {
        auto g = std::make_unique<FormGroup>("Config File", "file");
        auto line = row("file", 8);
        line->style().align_items = LayoutStyle::Align::Center;
        if (env_.open_config_file)
            line->add(std::make_unique<Button>("Open in Editor", [this] { env_.open_config_file(); }, Button::Kind::secondary, "open"));
        const std::string shown = env_.config_file_display.empty() ? env_.config_file.string() : env_.config_file_display;
        g->add_row(std::make_unique<FormRow>(env_.config_file.filename().string(), shown, std::move(line), "config_file"));
        if (env_.renderer) {
            g->add_row(std::make_unique<FormRow>("Active renderer", "", std::make_unique<Label>(env_.renderer(), TextStyle::body, Tone::muted, "control"),
                                                 "active_renderer"));
        }
        page.add_child(std::move(g), 0);
    }
    auto g = std::make_unique<FormGroup>("Edit in File", "edit_in_file");
    for (KeyId k : settings::keys_without_row()) {
        const KeySpec& s = config::spec(k);
        std::string v = config::input_text(cfg_.value(k), s.type);
        if (s.type == ValueType::TableArray) v = std::to_string(cfg_.get_tables(k).size()) + " entries";
        if (v.size() > 40) v = v.substr(0, 37) + "...";
        auto l = std::make_unique<Label>(v.empty() ? std::string("\xE2\x80\x94") : v, TextStyle::mono_small, Tone::muted, "control");
        auto fr = std::make_unique<FormRow>(std::string(s.path), std::string(s.doc), std::move(l), std::string(s.path));
        fr->label().set_text_style(TextStyle::mono_small);
        g->add_row(std::move(fr));
    }
    g->set_footnote("These keys have no control here; set them in config.toml.");
    page.add_child(std::move(g));
}

}  // namespace kt::app
