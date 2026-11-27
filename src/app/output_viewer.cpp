#include "app/output_viewer.h"

#include <algorithm>

#include "app/output_viewer_text.h"
#include "base/log.h"
#include "ui/widgets.h"

namespace kt::app {

namespace {
constexpr ui::PaneKey kPane{1};
}  // namespace

OutputViewer::OutputViewer(platform::IPlatform& p, OutputViewerSpec spec) : p_(p), spec_(std::move(spec)) {}

Result<std::unique_ptr<OutputViewer>> OutputViewer::open(platform::IPlatform& p, OutputViewerSpec spec) {
    std::unique_ptr<OutputViewer> v(new OutputViewer(p, std::move(spec)));
    if (auto r = v->init(); !r) return Unexpected(r.error());
    return v;
}

OutputViewer::~OutputViewer() {
    *alive_ = false;
    // The terminal draws with the window's fonts and textures: it goes first.
    term_widget_ = nullptr;
    pane_.reset();
    if (font_ && host_ && host_->open()) host_->font_system().release(font_);
    font_ = nullptr;
    host_.reset();
}

Result<void> OutputViewer::init() {
    UiWindowSpec ws = spec_.window;
    ws.keymap = [this, alive = alive_](const ui::KeyEvent& k) { return *alive && key(k); };
    ws.scale_changed = [this, alive = alive_](float s) {
        if (*alive && pane_) load_font(s);
    };
    auto host = UiWindowHost::create(p_, std::move(ws));
    if (!host) return Unexpected(host.error());
    host_ = std::move(*host);
    if (!host_->textures()) return make_error(Errc::unavailable, "the window has no glyph textures");

    env_.loop = &p_.loop();
    env_.clipboard = spec_.clipboard;
    env_.request_frame = [this, alive = alive_] {
        if (*alive && host_) host_->request_frame();
    };
    env_.copy_on_select = [] { return false; };
    env_.scale = host_->ui_scale();
    if (!load_font(host_->ui_scale())) return make_error(Errc::unavailable, "no terminal font");
    PaneEvents ev;
    pane_ = std::make_unique<PaneSession>(env_, kPane, *font_, *host_->textures(), spec_.view, spec_.term, std::move(ev));
    pane_->set_font_set(*font_, spec_.pad_x, spec_.pad_y);
    pane_->set_read_only(true);
    if (spec_.style) spec_.style(*pane_);

    // The find bar, the note, the terminal.
    auto root = ui::column("viewer", 0);
    auto bar = ui::row("findbar", 6);
    bar->style().padding = ui::Insets::hv(10, 8);
    bar->style().align_items = ui::LayoutStyle::Align::Center;
    auto field = std::make_unique<ui::TextField>(std::string(ui::tr("Find in the output")), "find");
    field->style().grow = 1;
    field->on_change = [this](const std::string& t) { find(t, false); };
    field->on_submit = [this](const std::string&) { step(1); };
    field->on_cancel = [this] {
        if (find_ && !find_->text().empty()) {
            find_->set_text({});
            find({}, false);
        } else {
            close();
        }
    };
    field->on_other_key = [this](const ui::KeyEvent& k) {
        if (k.key == platform::Key::enter && (k.mods & platform::mod_shift)) {
            step(-1);
            return true;
        }
        return false;
    };
    find_ = field.get();
    bar->add_child(std::move(field));
    auto count = std::make_unique<ui::Label>(std::string(), ui::TextStyle::small, ui::Tone::muted, "count");
    count->style().shrink = 0;
    count_ = count.get();
    bar->add_child(std::move(count));
    auto prev = std::make_unique<ui::Button>(std::string(ui::tr("Previous")), [this] { step(-1); }, ui::Button::Kind::secondary, "previous");
    prev->set_tooltip(std::string(ui::tr("The previous (older) match: Shift+Enter")));
    bar->add_child(std::move(prev));
    auto next = std::make_unique<ui::Button>(std::string(ui::tr("Next")), [this] { step(1); }, ui::Button::Kind::secondary, "next");
    next->set_tooltip(std::string(ui::tr("The next (newer) match: Enter")));
    bar->add_child(std::move(next));
    if (spec_.export_output) {
        auto ex = std::make_unique<ui::Button>(std::string(ui::tr("Export…")), [this] { ask_export(); }, ui::Button::Kind::secondary, "export");
        ex->set_tooltip(std::string(ui::tr("Save the stored output as text, raw with escape sequences, or an asciicast recording")));
        bar->add_child(std::move(ex));
    }
    root->add_child(std::move(bar));
    auto note_row = ui::row("note_row", 0);
    note_row->style().padding = ui::Insets{12, 0, 12, 6};
    auto note = std::make_unique<ui::Label>(std::string(), ui::TextStyle::small, ui::Tone::muted, "note");
    note_ = note.get();
    note_row->add_child(std::move(note));
    root->add_child(std::move(note_row));
    auto tw = std::make_unique<ui::TerminalWidget>(*this, kPane);
    term_widget_ = tw.get();
    root->add_child(std::move(tw));
    host_->set_root(std::move(root));
    host_->show();
    host_->ui().set_focus(term_widget_, false);
    return {};
}

void OutputViewer::ask_export() {
    if (!host_ || !host_->open() || !spec_.export_output) return;
    ask_export_format(host_->ui(), [this, alive = std::weak_ptr<bool>(alive_)](session::ExportFormat f) {
        if (auto a = alive.lock(); !a || !*a || !host_ || !host_->open()) return;
        spec_.export_output(f, host_->native_view(), [this, alive](Result<ExportResult> r) {
            if (auto a = alive.lock(); !a || !*a || !host_ || !host_->open()) return;
            show_export_result(host_->ui(), r);
        });
    });
}

bool OutputViewer::load_font(float ui_scale) {
    if (!host_ || !host_->open()) return false;
    auto s = host_->font_system().font_set(spec_.font, spec_.files, ui_scale, text::RenderMode::gray);
    if (!s) {
        log::warn("gui", "output viewer font: " + s.error().what);
        return false;
    }
    text::FontSet* old = font_;
    font_ = *s;
    env_.scale = ui_scale;
    if (pane_) {
        pane_->set_scale(ui_scale);
        pane_->set_font_set(*font_, spec_.pad_x, spec_.pad_y);
    }
    if (old && old != font_) host_->font_system().release(old);
    host_->request_frame();
    return true;
}

void OutputViewer::show(const session::HistoryOutput& o, const std::string& query) {
    if (!pane_) return;
    const ViewerText vt = viewer_text(o);
    target_line_ = pane_->show_lines(vt.lines, vt.target);
    std::string note = grouped_number(o.lines.size()) +
                       std::string(ui::tr(o.lines.size() == 1 ? " line" : " lines"));
    if (o.lines_before || o.lines_after)
        note += std::string(ui::tr(" of ")) + grouped_number(o.lines_before + o.lines.size() + o.lines_after);
    if (!o.commands.empty())
        note += " \xC2\xB7 " + grouped_number(o.commands.size()) + std::string(ui::tr(o.commands.size() == 1 ? " command" : " commands"));
    if (note_) note_->set_text(std::move(note));
    if (find_) find_->set_text(query);
    if (!query.empty()) find(query, true);
    else if (target_line_) pane_->reveal_line(*target_line_);
    update_count();
}

void OutputViewer::find(const std::string& text, bool at_target) {
    if (!pane_) return;
    if (text.empty()) {
        pane_->end_find();
    } else if (at_target && target_line_) {
        pane_->find_at(text, *target_line_);
    } else {
        pane_->find(text);
    }
    update_count();
}

void OutputViewer::step(int delta) {
    if (!pane_) return;
    if (pane_->find_count() == 0 && find_ && !find_->text().empty()) pane_->find(find_->text());
    pane_->find_step(delta);
    update_count();
}

void OutputViewer::update_count() {
    if (!count_ || !pane_) return;
    const std::size_t n = pane_->find_count();
    if (!find_ || find_->text().empty()) count_->set_text({});
    else if (n == 0) count_->set_text(std::string(ui::tr("no matches")));
    else count_->set_text(grouped_number(pane_->find_current().value_or(0) + 1) + std::string(ui::tr(" of ")) + grouped_number(n));
}

void OutputViewer::activate() {
    if (host_) host_->activate();
}

void OutputViewer::close() {
    if (host_) host_->close();
}

bool OutputViewer::key(const ui::KeyEvent& k) {
    if (!pane_ || !host_ || k.action == platform::KeyAction::release || k.composing) return false;
    const bool in_find = find_ && host_->ui().focused() == find_;
    const std::string id = spec_.command_of ? spec_.command_of(k) : std::string();
    if (id == "terminal.font_bigger") host_->step_zoom(1);
    else if (id == "terminal.font_smaller") host_->step_zoom(-1);
    else if (id == "terminal.font_reset") host_->step_zoom(0);
    else if (id == "terminal.find") {
        host_->ui().set_focus(find_, true);
        find_->select_all();
    } else if (id == "terminal.find_next") step(1);
    else if (id == "terminal.find_previous") step(-1);
    else if (id == "terminal.copy" && !in_find) {
        const std::string s = pane_->selection_text();
        if (!s.empty() && spec_.clipboard)
            (void)spec_.clipboard->write_text(platform::Selection::clipboard, std::u8string(s.begin(), s.end()));
    } else if (id == "terminal.select_all" && !in_find) pane_->select_all();
    else if (id == "terminal.prompt_previous") (void)pane_->jump_to_prompt(-1);
    else if (id == "terminal.prompt_next") (void)pane_->jump_to_prompt(1);
    else if (id == "terminal.scroll_page_up") pane_->scroll_page(1);
    else if (id == "terminal.scroll_page_down") pane_->scroll_page(-1);
    else if (id == "terminal.scroll_top") pane_->scroll_to_top();
    else if (id == "terminal.scroll_bottom") pane_->scroll_to_bottom();
    else if (id == "pane.close" || id == "window.close" || id == "tab.close") close();
    else if (k.key == platform::Key::enter && (k.mods & ~platform::mod_shift) == 0 && !in_find && find_ && !find_->text().empty())
        step(k.mods & platform::mod_shift ? -1 : 1);   // read-only: Enter steps through the matches from the text too
    else if (k.key == platform::Key::escape && k.mods == 0 && !in_find) {
        if (pane_->has_selection()) pane_->clear_selection();
        else close();
    } else {
        return false;
    }
    update_count();
    return true;
}

}  // namespace kt::app
