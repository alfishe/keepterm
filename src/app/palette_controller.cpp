#include "app/palette_controller.h"

#include <cctype>

#include "commands/registry.h"
#include "commands/workspace_target.h"
#include "config/config.h"
#include "ui/window.h"

namespace kt::app {

namespace pal = commands::palette;

namespace {

// The palette's icon names (src/commands) to the toolkit's icons; unknown names draw nothing.
ui::Icon palette_icon(std::string_view name) {
    struct M {
        std::string_view from;
        ui::Icon to;
    };
    static constexpr M map[] = {
        {"project", ui::Icon::folder}, {"files", ui::Icon::folder},     {"tab", ui::Icon::terminal},
        {"tabs", ui::Icon::terminal},  {"tab.new", ui::Icon::plus},     {"session", ui::Icon::terminal},
        {"sessions", ui::Icon::terminal}, {"quick_terminal", ui::Icon::terminal}, {"lock", ui::Icon::pin},
    };
    for (const M& m : map) {
        if (m.from == name) return m.to;
    }
    return ui::icon_from_name(name);
}

}  // namespace

PaletteController::PaletteController(PaletteEnv env) : env_(std::move(env)), fs_(pal::make_std_file_system()) { reconfigure(); }

PaletteController::~PaletteController() {
    *alive_ = false;
    close();
}

void PaletteController::reconfigure() {
    pal::EngineOptions o;
    std::vector<std::string> sources;
    if (env_.config) {
        o.frecency = env_.config->palette_frecency();
        o.recent_projects = static_cast<size_t>(std::max<std::int64_t>(0, env_.config->palette_recent_projects()));
        sources = env_.config->palette_sources();
    }
    engine_ = std::make_unique<pal::Engine>(pal::make_builtin_sources(sources), env_.usage ? env_.usage : &memory_usage_, o);
    rows_.clear();
    if (view_) {
        engine_->open(context());
        refresh();
    }
}

pal::SourceContext PaletteController::context() const {
    pal::SourceContext c;
    c.target = env_.target;
    c.registry = &commands::builtin_registry();
    c.keymap = env_.keymap;
    c.config = env_.config;
    c.os = env_.keymap ? env_.keymap->os() : commands::current_os();
    if (env_.key_context) c.keys = env_.key_context();
    if (env_.target) c.state = env_.target->state();
    c.data = &data_;
    c.fs = fs_.get();
    c.now_ms = env_.now_ms ? env_.now_ms() : 0;
    return c;
}

void PaletteController::open(std::optional<commands::PaletteScope> scope) {
    if (!env_.window || !env_.model) return;
    if (view_) close();
    data_ = commands::palette_data(*env_.model, env_.window_id, env_.tab_title);
    if (env_.fill) env_.fill(data_);
    engine_->open(context());
    if (scope) engine_->push_scope(*scope);
    auto v = std::make_unique<ui::PaletteView>("Search commands, projects, tabs\xE2\x80\xA6");
    view_ = v.get();
    v->on_query = [this](const std::string&) { refresh(); };
    v->on_accept = [this](size_t i, bool alt) { accept(i, alt); };
    v->on_enter_scope = [this](size_t i) { enter(i); };
    v->on_pop_scope = [this] { return pop(); };
    v->on_close = [this] { close(); };
    std::weak_ptr<bool> alive = alive_;
    overlay_ = ui::show_palette(*env_.window, std::move(v), [this, alive] {
        if (alive.expired()) return;
        view_ = nullptr;
        overlay_ = 0;
        rows_.clear();
    });
    refresh();
}

void PaletteController::close() {
    if (!view_ || !env_.window) return;
    const ui::WidgetId id = overlay_;
    view_ = nullptr;
    overlay_ = 0;
    rows_.clear();
    env_.window->close_overlay(id);
}

void PaletteController::toggle() {
    if (view_) close();
    else open();
}

std::string PaletteController::scope_title() const {
    if (engine_->scope() == commands::PaletteScope::Root) return {};
    if (engine_->scope() == commands::PaletteScope::MoveTabToProject) return "Move Tab to";
    std::string s(commands::scope_name(engine_->scope()));
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

void PaletteController::refresh() {
    if (!view_) return;
    const pal::Result& r = engine_->search(view_->field().text());
    rows_.clear();
    std::vector<ui::PaletteRow> rows;
    const commands::Os os = env_.keymap ? env_.keymap->os() : commands::current_os();
    for (const pal::Section& sec : r.sections) {
        for (const pal::Row& row : sec.rows) {
            if (!row.item) continue;
            const pal::Item& it = *row.item;
            ui::PaletteRow pr;
            pr.title = it.title;
            pr.subtitle = it.subtitle;
            pr.detail = it.detail;
            pr.detail_marks = it.detail_marks;
            pr.section = sec.header;
            if (it.keys) pr.keys = commands::format_sequence(*it.keys, os, commands::Format::Text);
            pr.highlights = row.highlights;
            pr.icon = palette_icon(it.icon);
            pr.enabled = it.enabled;
            pr.disabled_reason = it.disabled_reason;
            pr.checked = it.checked;
            pr.opens_scope = it.opens_scope.has_value();
            rows.push_back(std::move(pr));
            rows_.push_back(&row);
        }
    }
    view_->set_scope(scope_title());
    view_->set_rows(std::move(rows), r.matched == 0 && !view_->field().text().empty() ? "No matches" : "");
}

void PaletteController::accept(size_t i, bool alt) {
    if (!view_ || i >= rows_.size()) return;
    const pal::Row* row = rows_[i];
    if (!row->item || !row->item->enabled) return;
    const commands::PaletteScope before = engine_->scope();
    const size_t depth = engine_->scopes().size();
    if (row->item->opens_scope && !row->item->run) {
        enter(i);
        return;
    }
    // Close first: the action may open a dialog or move the focus.
    std::weak_ptr<bool> alive = alive_;
    const bool ran = engine_->run(*row, alt);
    if (alive.expired()) return;
    if (engine_->scope() != before || engine_->scopes().size() != depth) {
        if (view_) {
            view_->field().set_text("");
            refresh();
        }
        return;
    }
    close();
    if (ran && env_.after_run) env_.after_run();
}

void PaletteController::enter(size_t i) {
    if (!view_ || i >= rows_.size()) return;
    if (!engine_->enter(*rows_[i])) return;
    view_->field().set_text("");
    refresh();
}

bool PaletteController::pop() {
    if (!engine_->pop_scope()) return false;
    refresh();
    return true;
}

bool PaletteController::nav(commands::PaletteNav n) {
    if (!view_) return false;
    using N = commands::PaletteNav;
    switch (n) {
        case N::Next: view_->move(1); break;
        case N::Previous: view_->move(-1); break;
        case N::PageDown: view_->page(1); break;
        case N::PageUp: view_->page(-1); break;
        case N::Accept:
        case N::AcceptAlt:
            if (view_->selected()) accept(*view_->selected(), n == N::AcceptAlt);
            break;
        case N::EnterScope:
            if (view_->selected()) enter(*view_->selected());
            break;
        case N::PopScope: (void)pop(); break;
        case N::Close: close(); break;
    }
    return true;
}

void PaletteController::fill_state(commands::TargetState& s) const {
    s.palette_open = view_ != nullptr;
    s.palette_query_empty = !view_ || view_->field().text().empty();
    s.palette_in_scope = engine_ && engine_->scope() != commands::PaletteScope::Root;
}

}  // namespace kt::app
