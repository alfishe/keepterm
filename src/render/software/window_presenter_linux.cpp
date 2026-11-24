// Linux window presenters: X11 (MIT-SHM / XPutImage). The Wayland presenter (wl_shm) comes
// with the Wayland backend of the platform layer.
#include "platform_gui/native_surface.h"
#include "render/software/presenter.h"

namespace kt::render::sw {

Result<std::unique_ptr<ISoftwarePresenter>> create_x11_presenter(const platform::NativeSurface&);

Result<std::unique_ptr<ISoftwarePresenter>> create_window_presenter(const platform::NativeSurface& s) {
    switch (s.ws) {
        case platform::WindowSystem::x11: return create_x11_presenter(s);
        case platform::WindowSystem::wayland:
            return make_error(Errc::not_supported, "the wl_shm presenter is not implemented yet");
        default: return make_error(Errc::invalid_argument, "not a Linux surface");
    }
}

}  // namespace kt::render::sw
