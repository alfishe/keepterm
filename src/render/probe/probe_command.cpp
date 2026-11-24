#include "render/probe/probe_command.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "platform_gui/platform.h"
#include "render/probe_scene.h"
#include "render/render_select.h"

namespace kt::render {

namespace {

// A listener that ignores everything: the probe window is never shown.
class QuietListener final : public platform::IWindowListener {
public:
    void on_close_requested() override {}
    void on_resized(platform::PxSize, platform::DipSize) override {}
    void on_scale_changed(float, platform::DipRect) override {}
    void on_focus(bool) override {}
    void on_frame(platform::Clock::time_point) override {}
    void on_key(const platform::KeyEvent&) override {}
    void on_text(std::u8string_view) override {}
    void on_composition(const platform::Composition&) override {}
    void on_mouse(const platform::MouseEvent&) override {}
    void on_scroll(const platform::ScrollEvent&) override {}
};

// Step 3 of the probe: a hidden 64x64 window, a surface on it, attach, one frame, present.
// Returns an empty string on success, else the reason. show_ms > 0 (manual checks on a test
// machine): the window is shown with the probe pattern for that long instead.
std::string window_test(BackendKind kind, const BackendOptions& opts, int show_ms) {
    auto platform = platform::create_platform();
    if (!platform) return "platform: " + platform.error().what;
    QuietListener listener;
    platform::WindowDesc desc;
    desc.title = u8"keepterm renderer probe";
    desc.placement.rect = show_ms > 0 ? platform::DipRect{80, 80, 256, 160} : platform::DipRect{0, 0, 64, 64};
    desc.min_size = platform::DipSize{16, 16};
    auto window = (*platform)->create_window(desc, listener);
    if (!window) return "window: " + window.error().what;
    // Every backend takes the default surface: Metal adds its own CAMetalLayer under the view's
    // layer, D3D11 and Vulkan use the child HWND / X11 window.
    auto surface = (*window)->create_surface(platform::SurfaceRequest{});
    if (!surface) return "surface: " + surface.error().what;
    if (show_ms > 0) (*window)->show(false);
    auto r = create_backend(kind, opts);
    std::string reason;
    if (!r) {
        reason = r.error().what;
    } else {
        const platform::PxSize px = (*window)->client_size_px();
        const PxSize size{px.w > 0 ? px.w : 64, px.h > 0 ? px.h : 64};
        if (auto ok = (*r)->attach(*surface, size, (*window)->scale()); !ok) {
            reason = "attach: " + ok.error().what;
        } else if (show_ms > 0) {
            // The pattern at the window's size (the scene covers its top-left 256 x 160 px),
            // redrawn on a timer so that resizes and occlusion are visible too.
            auto& loop = (*platform)->loop();
            const auto end = platform::Clock::now() + std::chrono::milliseconds(show_ms);
            std::function<void()> tick = [&] {
                const platform::PxSize now_px = (*window)->client_size_px();
                (*r)->resize(PxSize{now_px.w, now_px.h}, (*window)->scale());
                if (auto drawn = draw_probe_scene(**r); !drawn && reason.empty()) reason = "frame: " + drawn.error().what;
                if (platform::Clock::now() >= end) loop.quit(0);
                else loop.add_timer(platform::Clock::now() + std::chrono::milliseconds(250), tick);
            };
            loop.post(tick);
            loop.run();
            (*r)->detach();
        } else {
            auto begun = (*r)->begin_frame(Damage{{}, true});
            if (!begun) {
                reason = "frame: " + begun.error().what;
            } else if (*begun) {
                DrawList dl;
                dl.quad(solid_quad(0, 0, static_cast<float>(size.w), static_cast<float>(size.h), rgba(20, 20, 20)));
                (*r)->submit(dl);
                if (auto ended = (*r)->end_frame(); !ended) reason = "frame: " + ended.error().what;
                // A hidden window may have no drawable to present into; only a loss counts.
                else if (auto pr = (*r)->present(); !pr && pr.error().code == Errc::lost) reason = "present: " + pr.error().what;
            }
            (*r)->detach();
        }
    }
    (*window)->destroy_surface();
    (*window)->close();
    return reason;
}

}  // namespace

int run_probe_command(int argc, char** argv) {
    const char* name = nullptr;
    BackendOptions opts;
    bool offscreen = false;
    int show_ms = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--probe-renderer") == 0 && i + 1 < argc) name = argv[++i];
        else if (std::strcmp(argv[i], "--allow-software-adapter") == 0) opts.allow_software_adapter = true;
        else if (std::strcmp(argv[i], "--offscreen") == 0) offscreen = true;
        else if (std::strcmp(argv[i], "--show-ms") == 0 && i + 1 < argc) show_ms = std::atoi(argv[++i]);
    }
    if (name && std::strcmp(name, "auto") == 0) {
        // Diagnostics: the auto-detection of create_renderer("auto") with child probes, using
        // and updating the probe cache; prints the resulting order.
        RenderConfig cfg;
        cfg.backend = opts;
        const Selection sel = select_backends(cfg);
        std::string order;
        for (BackendKind k : sel.order) order += std::string(order.empty() ? "\"" : ",\"") + backend_name(k) + "\"";
        std::printf("{\"order\":[%s],\"why\":\"%s\",\"cache\":\"%s\"}\n", order.c_str(), sel.why.c_str(),
                    probe_cache_file(default_state_dir(), cfg).generic_string().c_str());
        return 0;
    }
    const auto kind = name ? parse_backend(name) : std::nullopt;
    if (!kind) {
        std::fprintf(stderr, "usage: keepterm --probe-renderer auto|d3d11|metal|vulkan|opengl|software "
                             "[--allow-software-adapter] [--offscreen] [--show-ms N]\n");
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    ProbeResult res = probe_offscreen(*kind, opts);
    if (res.ok && !offscreen && *kind != BackendKind::software && platform::display_available()) {
        const std::string reason = window_test(*kind, opts, show_ms);
        res.window_tested = true;
        if (!reason.empty()) {
            res.ok = false;
            res.reason = "window: " + reason;
        }
    }
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s\n", to_json(res).c_str());
    std::fflush(stdout);
    if (res.ok) return 0;
    return res.unavailable ? 2 : 1;
}

}  // namespace kt::render
