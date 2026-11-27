#include "app/ui_window_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
    zoom_ = std::clamp(spec_.zoom, UiWindowHost::kZoomMin, UiWindowHost::kZoomMax);
    px_ = win_->client_size_px();
    auto surf = win_->create_surface(platform::SurfaceRequest{});
    if (!surf) return Unexpected(surf.error());
    native_view_ = surf->handle;
    // A window without a native surface (a headless platform) renders offscreen.
    const bool native = surf->handle || surf->layer || surf->xid;
    auto r = render::create_renderer(native ? spec_.renderer : std::string("software"), native ? &*surf : nullptr,
                                     render::PxSize{px_.w, px_.h}, scale_, spec_.render_config);
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
        if (win_) win_->set_ime_cursor_rect(platform::DipRect{rc.x * zoom_, rc.y * zoom_, rc.w * zoom_, rc.h * zoom_});
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
    uiw_->resize(ui::Size{static_cast<float>(px_.w) / ui_scale(), static_cast<float>(px_.h) / ui_scale()}, ui_scale());
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
            win_->set_ime_cursor_rect(platform::DipRect{c.x * zoom_, c.y * zoom_, c.w * zoom_, c.h * zoom_});
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

void UiWindowHost::apply_scale() {
    if (!r_ || !uiw_) return;
    r_->resize(render::PxSize{px_.w, px_.h}, scale_);   // the display's: Metal sets it as the layer's contentsScale
    uiw_->resize(ui::Size{static_cast<float>(px_.w) / ui_scale(), static_cast<float>(px_.h) / ui_scale()}, ui_scale());
    full_next_ = true;
    request_frame();
    if (spec_.scale_changed && ui_scale() != scale_sent_) {
        scale_sent_ = ui_scale();
        spec_.scale_changed(scale_sent_);
    }
}

void UiWindowHost::on_resized(platform::PxSize px, platform::DipSize) {
    px_ = px;
    apply_scale();
}

void UiWindowHost::on_scale_changed(float scale, platform::DipRect) {
    if (!win_ || scale <= 0) return;
    scale_ = scale;
    px_ = win_->client_size_px();
    apply_scale();
}

void UiWindowHost::set_zoom(float zoom) {
    zoom = std::clamp(std::round(zoom * 10.0f) / 10.0f, kZoomMin, kZoomMax);
    if (zoom == zoom_) return;
    zoom_ = zoom;
    apply_scale();
    if (spec_.zoom_changed) spec_.zoom_changed(zoom_);
}

void UiWindowHost::step_zoom(int delta) { set_zoom(delta == 0 ? 1.0f : zoom_ + kZoomStep * static_cast<float>(delta)); }

float UiWindowHost::render_scale() const { return r_ ? r_->scale() : 0.0f; }

bool UiWindowHost::menu_command(std::string_view id) {
    if (id == "terminal.font_bigger") step_zoom(1);
    else if (id == "terminal.font_smaller") step_zoom(-1);
    else if (id == "terminal.font_reset") step_zoom(0);
    else return false;
    return true;
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

// Pointer positions come in DIPs; the widgets are laid out in DIPs / zoom.
void UiWindowHost::on_mouse(const platform::MouseEvent& m) {
    if (!uiw_) return;
    platform::MouseEvent z = m;
    z.pos = {m.pos.x / zoom_, m.pos.y / zoom_};
    uiw_->on_mouse(z);
}

void UiWindowHost::on_scroll(const platform::ScrollEvent& s) {
    if (!uiw_) return;
    platform::ScrollEvent z = s;
    z.pos = {s.pos.x / zoom_, s.pos.y / zoom_};
    uiw_->on_scroll(z);
}

}  // namespace kt::app
