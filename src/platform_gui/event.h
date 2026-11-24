// Window input as one value type.
#pragma once

#include <functional>
#include <string>
#include <variant>

#include "platform_gui/input.h"
#include "platform_gui/window.h"

namespace kt::platform {

struct CloseRequested {};
struct Destroyed {};
struct Resized          { PxSize px; DipSize dip; };
struct ScaleChanged     { float scale = 1; DipRect suggested; };
struct FocusChanged     { bool focused = false; };
struct OcclusionChanged { bool occluded = false; };
struct Exposed          {};
struct TextEvent        { std::u8string text; };          // committed text (on_text)
struct ImeEvent {                                         // composition (on_composition)
    enum class Kind : uint8_t { start, update, end };     // end: committed text follows as
    Kind kind = Kind::update;                             // TextEvent, or nothing when cancelled
    Composition composition;
};

using Event = std::variant<CloseRequested, Destroyed, Resized, ScaleChanged, FocusChanged,
                           OcclusionChanged, Exposed, KeyEvent, TextEvent, ImeEvent, MouseEvent,
                           ScrollEvent, DropEvent, DragOverEvent>;

// An IWindowListener that turns each callback into an Event. Frames stay a separate callback.
class EventAdapter final : public IWindowListener {
public:
    EventAdapter(std::function<void(const Event&)> sink, std::function<void(Clock::time_point)> on_frame);

    // on_drag_over returns the effect that the sink stored with set_drop_effect().
    void set_drop_effect(DropEffect e) { drop_effect_ = e; }

    void on_close_requested() override;
    void on_destroyed() override;
    void on_resized(PxSize px, DipSize dip) override;
    void on_scale_changed(float scale, DipRect suggested) override;
    void on_focus(bool focused) override;
    void on_occluded(bool occluded) override;
    void on_expose() override;
    void on_frame(Clock::time_point target_present) override;
    void on_key(const KeyEvent&) override;
    void on_text(std::u8string_view committed) override;
    void on_composition(const Composition&) override;
    void on_mouse(const MouseEvent&) override;
    void on_scroll(const ScrollEvent&) override;
    void on_drop(const DropEvent&) override;
    DropEffect on_drag_over(const DragOverEvent&) override;

private:
    std::function<void(const Event&)> sink_;
    std::function<void(Clock::time_point)> on_frame_;
    DropEffect drop_effect_ = DropEffect::copy;
    bool composing_ = false;
};

}  // namespace kt::platform
