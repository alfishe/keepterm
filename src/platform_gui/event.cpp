#include "platform_gui/event.h"

#include <utility>

namespace kt::platform {

EventAdapter::EventAdapter(std::function<void(const Event&)> sink, std::function<void(Clock::time_point)> on_frame)
    : sink_(std::move(sink)), on_frame_(std::move(on_frame)) {}

void EventAdapter::on_close_requested() { sink_(CloseRequested{}); }
void EventAdapter::on_destroyed() { sink_(Destroyed{}); }
void EventAdapter::on_resized(PxSize px, DipSize dip) { sink_(Resized{px, dip}); }
void EventAdapter::on_scale_changed(float scale, DipRect suggested) { sink_(ScaleChanged{scale, suggested}); }
void EventAdapter::on_focus(bool focused) { sink_(FocusChanged{focused}); }
void EventAdapter::on_occluded(bool occluded) { sink_(OcclusionChanged{occluded}); }
void EventAdapter::on_expose() { sink_(Exposed{}); }
void EventAdapter::on_frame(Clock::time_point target_present) {
    if (on_frame_) on_frame_(target_present);
}
void EventAdapter::on_key(const KeyEvent& e) { sink_(e); }
void EventAdapter::on_text(std::u8string_view committed) { sink_(TextEvent{std::u8string(committed)}); }

void EventAdapter::on_composition(const Composition& c) {
    ImeEvent e;
    if (c.text.empty()) {
        e.kind = ImeEvent::Kind::end;
        composing_ = false;
    } else {
        e.kind = composing_ ? ImeEvent::Kind::update : ImeEvent::Kind::start;
        composing_ = true;
    }
    e.composition = c;
    sink_(e);
}

void EventAdapter::on_mouse(const MouseEvent& e) { sink_(e); }
void EventAdapter::on_scroll(const ScrollEvent& e) { sink_(e); }
void EventAdapter::on_drop(const DropEvent& e) { sink_(e); }

DropEffect EventAdapter::on_drag_over(const DragOverEvent& e) {
    sink_(e);
    return drop_effect_;
}

}  // namespace kt::platform
