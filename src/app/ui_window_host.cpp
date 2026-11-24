#include "app/ui_window_host.h"

#include <algorithm>
#include <chrono>

#include "base/log.h"
#include "text/font.h"
#include "text/font_source.h"
#include "view/terminal/atlas_textures.h"

namespace kt::app {

namespace {

std::string_view as_chars(std::u8string_view s) { return {reinterpret_cast<const char*>(s.data()), s.size()}; }

}  // namespace

UiWindowHost::UiWindowHost(platform::IPlatform& p, UiWindowSpec spec) : p_(p), spec_(std::move(spec)) {}

Result<std::unique_ptr<UiWindowHost>> UiWindowHost::create(platform::IPlatform& p, UiWindowSpec spec) {
    if (!spec.font_source) return make_error(Errc::invalid_argument, "no font source");
    std::unique_ptr<UiWindowHost> h(new UiWindowHost(p, std::move(spec)));
    if (auto r = h->init(); !r) return Unexpected(r.error());
    return h;
}

Result<void> UiWindowHost::init() {
    auto fs = text::FontSystem::create();
    if (!fs) return Unexpected(fs.error());
    fonts_ = std::move(*fs);
    fonts_->set_font_source(spec_.font_source);
    auto uf = ui::resolve_ui_fonts(*spec_.font_source, spec_.ui_font, spec_.ui_mono_font);
    if (!uf) return Unexpected(uf.error());
    text_ = std::make_unique<ui::FontTextService>(*fonts_, std::move(*uf));

    platform::WindowDesc wd;
    wd.title = spec_.title;
    wd.min_size = spec_.min_size;
    wd.placement.rect = platform::DipRect{0, 0, spec_.size.w, spec_.size.h};
    const platform::DipRect& c = spec_.center_over;
    if (c.w > 0 && c.h > 0) {
        // Never at (0, 0) exactly: that origin asks for the OS default position.
        wd.placement.rect.x = std::max(1.0f, c.x + (c.w - spec_.size.w) / 2);
        wd.placement.rect.y = std::max(1.0f, c.y + (c.h - spec_.size.h) / 2);
    }
    auto w = p_.create_window(wd, *this);
    if (!w) return Unexpected(w.error());
    win_ = std::move(*w);
    scale_ = win_->scale();
    px_ = win_->client_size_px();
    auto surf = win_->create_surface(platform::SurfaceRequest{});
    if (!surf) return Unexpected(surf.error());
    auto r = render::create_renderer(spec_.renderer, &*surf, render::PxSize{px_.w, px_.h}, scale_, spec_.render_config);
    if (!r) return Unexpected(r.error());
    r_ = std::move(*r);
    r_->on_switch([this, alive = alive_](render::BackendKind, render::BackendKind, const std::string&) {
        p_.loop().post([this, alive] {
            if (!*alive) return;
            full_next_ = true;
            request_frame();
        });
    });
    tex_ = std::make_unique<view::AtlasTextures>(fonts_->atlas(), *r_);
    text_->set_textures(tex_.get());

    uiw_ = std::make_unique<ui::Window>(*text_, spec_.theme);
    uiw_->set_motion(spec_.motion);
    uiw_->request_frame = [this] { request_frame(); };
    uiw_->set_ime_rect = [this](ui::Rect rc) {
        if (win_) win_->set_ime_cursor_rect(platform::DipRect{rc.x, rc.y, rc.w, rc.h});
    };
    uiw_->set_cursor = [this](platform::CursorShape s) {
        if (win_) win_->set_cursor(s);
    };
    uiw_->keymap = [this](const ui::KeyEvent& k) { return spec_.keymap && spec_.keymap(k); };
    uiw_->timer_changed = [this](ui::TimePoint at) { arm_timer(at); };
    uiw_->get_clipboard = [this] { return spec_.get_clipboard ? spec_.get_clipboard() : std::string(); };
    uiw_->set_clipboard = [this](const std::string& s) {
        if (spec_.set_clipboard) spec_.set_clipboard(s);
    };
    uiw_->resize(ui::Size{static_cast<float>(px_.w) / scale_, static_cast<float>(px_.h) / scale_}, scale_);
    return {};
}

UiWindowHost::~UiWindowHost() {
    *alive_ = false;
    spec_.closed = nullptr;
    drop_render();
    if (win_) {
        auto w = std::move(win_);
        w->close();
    }
}

void UiWindowHost::set_root(std::unique_ptr<ui::Widget> root) {
    uiw_->set_root(std::move(root));
    full_next_ = true;
    request_frame();
}

void UiWindowHost::show() {
    if (!win_) return;
    win_->set_ime_enabled(true);
    win_->show(true);
    uiw_->on_focus(win_->focused());
    full_next_ = true;
    request_frame();
}

void UiWindowHost::activate() {
    if (win_) win_->show(true);
}

void UiWindowHost::close() {
    // Posted: the caller may be a widget of this window (a key, a button), and closing drops the
    // widget tree.
    p_.loop().post([this, alive = alive_] {
        if (*alive && win_) win_->close();   // on_destroyed follows
    });
}

void UiWindowHost::set_theme(const ui::Theme& t) {
    if (!uiw_) return;
    uiw_->set_theme(t);
    full_next_ = true;
    request_frame();
}

void UiWindowHost::set_motion(ui::MotionLevel m) {
    if (uiw_) uiw_->set_motion(m);
}

void UiWindowHost::set_ui_fonts(const std::string& ui_font, const std::string& ui_mono_font) {
    if (!text_ || !spec_.font_source) return;
    if (auto uf = ui::resolve_ui_fonts(*spec_.font_source, ui_font, ui_mono_font)) text_->set_files(std::move(*uf));
    if (uiw_) uiw_->invalidate_all();
    full_next_ = true;
    request_frame();
}

void UiWindowHost::request_frame() {
    if (win_ && !occluded_) win_->request_frame();
}

void UiWindowHost::arm_timer(ui::TimePoint at) {
    if (timer_) p_.loop().cancel_timer(timer_);
    const auto delay = at - ui::Clock::now();
    timer_ = p_.loop().add_timer(platform::Clock::now() + std::chrono::duration_cast<platform::Clock::duration>(delay),
                                 [this, alive = alive_] {
                                     if (!*alive) return;
                                     timer_ = 0;
                                     if (!uiw_) return;
                                     uiw_->run_timers();
                                     if (auto next = uiw_->next_deadline()) arm_timer(*next);
                                 });
}

void UiWindowHost::drop_render() {
    if (timer_) {
        p_.loop().cancel_timer(timer_);
        timer_ = 0;
    }
    if (text_) text_->set_textures(nullptr);
    uiw_.reset();
    tex_.reset();
    if (r_) r_->detach();
    r_.reset();
}

void UiWindowHost::draw() {
    if (!r_ || !uiw_ || px_.w <= 0 || px_.h <= 0) return;
    render::DrawList dl;
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
        log::warn("ui", "secondary window: " + ok.error().what);
        return;
    }
    if (!*ok) {
        full_next_ = true;
        uiw_->invalidate_all();
        return;
    }
    r_->submit(dl);
    if (auto e = r_->end_frame(); !e) log::warn("ui", "secondary window: " + e.error().what);
    if (auto pr = r_->present(); !pr) log::warn("ui", "secondary window: " + pr.error().what);
    full_next_ = false;
    if (uiw_->needs_frame()) request_frame();
}

// ---------------------------------------------------------------- IWindowListener

void UiWindowHost::on_close_requested() {
    if (win_) win_->close();
}

void UiWindowHost::on_destroyed() {
    drop_render();
    win_.reset();   // allowed from inside on_destroyed (the platform's last call)
    if (closed_sent_) return;
    closed_sent_ = true;
    if (auto fn = std::move(spec_.closed)) p_.loop().post(std::move(fn));
}

void UiWindowHost::on_resized(platform::PxSize px, platform::DipSize dip) {
    if (!r_ || !uiw_) return;
    px_ = px;
    r_->resize(render::PxSize{px.w, px.h}, scale_);
    uiw_->resize(ui::Size{dip.w, dip.h}, scale_);
    full_next_ = true;
    request_frame();
}

void UiWindowHost::on_scale_changed(float scale, platform::DipRect) {
    if (!r_ || !uiw_ || scale <= 0) return;
    scale_ = scale;
    px_ = win_->client_size_px();
    r_->resize(render::PxSize{px_.w, px_.h}, scale_);
    uiw_->resize(ui::Size{static_cast<float>(px_.w) / scale_, static_cast<float>(px_.h) / scale_}, scale_);
    full_next_ = true;
    request_frame();
}

void UiWindowHost::on_focus(bool focused) {
    if (!uiw_) return;
    uiw_->on_focus(focused);
    if (spec_.focus_changed) spec_.focus_changed(focused);
    request_frame();
}

void UiWindowHost::on_occluded(bool occluded) {
    occluded_ = occluded;
    if (!occluded) {
        full_next_ = true;
        request_frame();
    }
}

void UiWindowHost::on_expose() {
    full_next_ = true;
    request_frame();
}

void UiWindowHost::on_frame(platform::Clock::time_point) { draw(); }

void UiWindowHost::on_key(const platform::KeyEvent& k) {
    if (uiw_) uiw_->on_key(k);
}

void UiWindowHost::on_text(std::u8string_view text) {
    if (uiw_ && !text.empty()) uiw_->on_text(as_chars(text));
}

void UiWindowHost::on_composition(const platform::Composition& c) {
    if (uiw_) uiw_->on_composition(c);
}

void UiWindowHost::on_mouse(const platform::MouseEvent& m) {
    if (uiw_) uiw_->on_mouse(m);
}

void UiWindowHost::on_scroll(const platform::ScrollEvent& s) {
    if (uiw_) uiw_->on_scroll(s);
}

}  // namespace kt::app
