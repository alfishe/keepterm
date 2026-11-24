// A secondary OS window that shows one widget tree (the Settings window).
//
// The window has its own renderer, glyph atlas and UI text service: an atlas mirrors into the
// textures of one renderer (view::AtlasTextures), so sharing the main window's atlas between
// two renderers would need a shared frame clock and per-renderer eviction. A second atlas costs
// a few megabytes and only while the window is open. The font source (the installed families)
// is shared with the main window.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "platform_gui/platform.h"
#include "render/render_select.h"
#include "ui/text_service.h"
#include "ui/window.h"

namespace kt::text {
class FontSystem;
class IFontSource;
}  // namespace kt::text

namespace kt::view {
class AtlasTextures;
}

namespace kt::app {

struct UiWindowSpec {
    std::u8string title;
    platform::DipSize size{820, 600};
    platform::DipSize min_size{680, 480};
    // Centered over this rectangle (the main window's frame, DIP); empty: the platform default.
    platform::DipRect center_over{};
    std::string renderer = "software";   // a backend name ("d3d11", "metal" ...): the main window's
    render::RenderConfig render_config;
    text::IFontSource* font_source = nullptr;   // must outlive the window
    std::string ui_font, ui_mono_font;          // ui.font, ui.mono_font
    ui::Theme theme = ui::Theme::dark();
    ui::MotionLevel motion = ui::MotionLevel::Full;
    std::function<std::string()> get_clipboard;
    std::function<void(const std::string&)> set_clipboard;
    // The keymap of the window (Cmd/Ctrl+W closes it ...): true consumes the key.
    std::function<bool(const ui::KeyEvent&)> keymap;
    // The window gained or lost the keyboard focus.
    std::function<void(bool)> focus_changed;
    // The window went away (closed by the user or by close()); runs once, on a later loop turn
    // when the host may destroy this object.
    std::function<void()> closed;
};

class UiWindowHost final : public platform::IWindowListener {
public:
    // Creates the window hidden; show() puts it on screen with the root widget.
    static Result<std::unique_ptr<UiWindowHost>> create(platform::IPlatform&, UiWindowSpec);
    ~UiWindowHost() override;
    UiWindowHost(const UiWindowHost&) = delete;
    UiWindowHost& operator=(const UiWindowHost&) = delete;

    ui::Window& ui() { return *uiw_; }
    void set_root(std::unique_ptr<ui::Widget>);
    void show();
    void activate();   // brings an open window to the front
    void close();      // destroys the OS window; `closed` runs
    bool open() const { return win_ != nullptr; }
    bool focused() const { return win_ && win_->focused(); }

    void set_theme(const ui::Theme&);
    void set_motion(ui::MotionLevel);
    void set_ui_fonts(const std::string& ui_font, const std::string& ui_mono_font);

    // platform::IWindowListener
    void on_close_requested() override;
    void on_destroyed() override;
    void on_resized(platform::PxSize px, platform::DipSize dip) override;
    void on_scale_changed(float scale, platform::DipRect suggested) override;
    void on_focus(bool focused) override;
    void on_occluded(bool occluded) override;
    void on_expose() override;
    void on_frame(platform::Clock::time_point) override;
    void on_key(const platform::KeyEvent&) override;
    void on_text(std::u8string_view committed) override;
    void on_composition(const platform::Composition&) override;
    void on_mouse(const platform::MouseEvent&) override;
    void on_scroll(const platform::ScrollEvent&) override;

private:
    UiWindowHost(platform::IPlatform&, UiWindowSpec);
    Result<void> init();
    void draw();
    void arm_timer(ui::TimePoint at);
    void drop_render();
    void request_frame();

    platform::IPlatform& p_;
    UiWindowSpec spec_;
    std::unique_ptr<text::FontSystem> fonts_;
    std::unique_ptr<ui::FontTextService> text_;
    std::unique_ptr<platform::IWindow> win_;
    std::unique_ptr<render::ManagedRenderer> r_;
    std::unique_ptr<view::AtlasTextures> tex_;
    std::unique_ptr<ui::Window> uiw_;
    platform::PxSize px_;
    float scale_ = 1.0f;
    bool full_next_ = true;
    bool occluded_ = false;
    uint64_t timer_ = 0;
    bool closed_sent_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace kt::app
