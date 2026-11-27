// Auto-detection, the probe cache and the run-time fallback chain.
//
//   create_renderer("auto" | "software" | "metal" | "d3d11" | "vulkan" | "opengl", surface, ...)
//
// is what the application calls. It resolves the backend (forced name, remote session rule,
// probe cache, out-of-process probes on a cache miss), creates it, attaches it to the surface
// (or to an offscreen target when surface is null) and wraps it in a ManagedRenderer, which
// keeps CPU copies of all textures and switches backend on Errc::lost: the same backend is
// recreated once, a second loss within 60 s moves to the next backend in platform order.
//
// The probe child (`keepterm --probe-renderer <kind>`, src/render/probe/probe_command.h) runs
// probe_offscreen() plus a hidden-window present test and prints one JSON result.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "platform_gui/native_surface.h"
#include "render/backend_factory.h"

namespace kt::render {

// Bumped whenever the probe or its pattern changes: invalidates every cache.
inline constexpr int kProbeVersion = 1;

struct RenderConfig {
    std::string renderer = "auto";           // config key `renderer`
    std::string renderer_remote = "software";  // config key `renderer_remote`: software | auto
    std::optional<bool> remote;              // remote session; nullopt = is_remote_session()
    BackendOptions backend;                  // allow_software_adapter: tests only
    std::filesystem::path exe;               // probe child executable; empty = this process
    // Folder of render-probe.json and the crash marker. Empty = default_state_dir().
    std::filesystem::path state_dir;
    std::chrono::milliseconds probe_timeout{3000};
    // Probe in this process instead of a child (tests, and builds without the probe command).
    bool probe_in_process = false;
};

// One backend's probe outcome: a results[] entry of the cache and the child's stdout.
struct ProbeResult {
    BackendKind backend = BackendKind::software;
    bool ok = false;
    bool unavailable = false;     // API, library or usable device missing (exit code 2)
    std::string reason;           // empty when ok; "probe-timeout", "probe-crash", "bad-output", ...
    std::string adapter;
    uint32_t vendor = 0, device = 0;
    double ms = 0;
    bool dual_source_blend = false;
    int max_diff = -1;            // largest channel difference of the test frame, -1 = not compared
    bool window_tested = false;   // the hidden-window attach + present step ran (needs a display)
};

std::string to_json(const ProbeResult&);
std::optional<ProbeResult> parse_probe_result(std::string_view json);

struct ProbeCache {
    std::string key;                    // machine_key() at probe time
    std::vector<ProbeResult> results;   // in probe order
};

// OS version, probe version and the GPU adapters with their driver versions where cheap to get.
std::string machine_key();
std::optional<ProbeCache> load_probe_cache(const std::filesystem::path& file);
bool save_probe_cache(const std::filesystem::path& file, const ProbeCache&);   // atomic

// The probe cache folder: platform::paths::get(Kind::cache) of the default profile;
// a per-user folder under the temp directory only when that folder cannot be created.
std::filesystem::path default_state_dir();
// The cache file in `dir` for this configuration. Probes that allowed software adapters (tests)
// keep their own file, so they never decide a normal run.
std::filesystem::path probe_cache_file(const std::filesystem::path& dir, const RenderConfig&);
// True when select_backends(config) needs no probe: a forced backend, the remote rule, or a
// cache that matches this machine. The application shows its first frames with software and
// probes in the background otherwise.
bool selection_is_cheap(const RenderConfig&);

// RDP / forwarded X11. macOS: always false (screen sharing shows the console).
bool is_remote_session();

// Crash marker `render-attempt`.
void write_crash_marker(const std::filesystem::path& state_dir, BackendKind);
void clear_crash_marker(const std::filesystem::path& state_dir);
std::optional<BackendKind> read_crash_marker(const std::filesystem::path& state_dir);

// The probe frame in this process: create the backend, render draw_probe_scene() offscreen,
// read it back and compare with the software backend (golden tolerance).
ProbeResult probe_offscreen(BackendKind, const BackendOptions&);

// `exe --probe-renderer <kind>` as a child with a timeout; parses its JSON.
ProbeResult run_probe_child(const std::filesystem::path& exe, BackendKind, bool allow_software_adapter,
                            std::chrono::milliseconds timeout);

std::filesystem::path current_executable();

// Golden-image tolerance on two BGRA images of equal size.
struct ImageCompare {
    bool same_size = false;
    int max_diff = 0;
    size_t over_small = 0, over_large = 0, pixels = 0;
    bool within_tolerance() const;
};
ImageCompare compare_bgra(const Image& a, const Image& b);

// The ordered candidates for a configuration (forced name first, then the cached or probed
// winner, then the rest of platform_order(), software last), with the reasons logged.
struct Selection {
    std::vector<BackendKind> order;   // never empty; ends with software
    std::string why;
};
// prober: how one backend is probed on a cache miss (default: child or in-process per config).
using Prober = std::function<ProbeResult(BackendKind)>;
Selection select_backends(const RenderConfig&, const Prober& prober = {});

// ---------------------------------------------------------------- managed renderer

class ManagedRenderer final : public IRenderer {
public:
    // Creates a backend for `kind` (factory) — replaceable for tests.
    using Factory = std::function<Result<std::unique_ptr<IRenderer>>(BackendKind)>;
    // Called after a backend switch (from, to); the application requests a full redraw.
    using SwitchListener = std::function<void(BackendKind from, BackendKind to, const std::string& reason)>;
    // Clock for the 60 s rule; replaceable for tests.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    ManagedRenderer(std::vector<BackendKind> order, Factory factory, std::filesystem::path state_dir);
    ~ManagedRenderer() override;

    // Creates the first working backend of the order and attaches it. surface null: offscreen.
    Result<void> start(const platform::NativeSurface* surface, PxSize size, float scale);

    BackendKind active() const { return kind_; }
    const std::vector<BackendKind>& order() const { return order_; }
    // Replaces the order and moves to its first backend that starts (a finished background probe,
    // a changed `renderer` key). Textures are re-created from their CPU copies, the switch
    // listener runs, and the next frame is drawn in full. No-op when the first entry already runs.
    Result<void> switch_order(std::vector<BackendKind> order, const std::string& reason);
    void on_switch(SwitchListener l) { listener_ = std::move(l); }
    void set_clock(Clock c) { clock_ = std::move(c); }
    IRenderer* inner() { return inner_.get(); }

    // IRenderer
    const Caps& caps() const override { return inner_->caps(); }
    Result<void> attach(const platform::NativeSurface&, PxSize size, float scale) override;
    Result<void> attach_offscreen(PxSize size, float scale) override;
    void resize(PxSize size, float scale) override;
    float scale() const { return scale_; }
    void detach() override;
    Result<TextureId> create_texture(const TextureDesc&) override;
    void update_texture(TextureId, IRect region, const void* pixels, uint32_t stride) override;
    void destroy_texture(TextureId) override;
    Result<bool> begin_frame(const Damage&) override;
    void submit(const DrawList&) override;
    Result<void> end_frame() override;
    Result<void> present() override;
    void copy_rect(IRect src, int32_t dst_x, int32_t dst_y) override;
    Result<Image> read_back(IRect) override;

private:
    struct Tex {
        TextureDesc desc;
        std::vector<uint8_t> pixels;   // tight rows, the CPU copy
        TextureId inner = 0;
    };

    Result<void> create_at(size_t index, const std::string& reason);
    Result<void> attach_inner();
    // Handles an error of the inner backend: true when it was a loss and a new backend runs.
    bool handle(const Error&);

    std::vector<BackendKind> order_;
    size_t index_ = 0;
    Factory factory_;
    std::filesystem::path state_dir_;
    BackendKind kind_ = BackendKind::software;
    std::unique_ptr<IRenderer> inner_;
    std::optional<platform::NativeSurface> surface_;
    PxSize size_;
    float scale_ = 1.0f;
    bool attached_ = false;
    bool marker_pending_ = false;
    bool force_full_ = false;
    bool in_frame_ = false;
    bool identity_ = true;                 // public texture id == inner id for every texture
    std::unordered_map<TextureId, Tex> textures_;
    TextureId next_id_ = 1;
    DrawList remapped_;
    std::optional<std::chrono::steady_clock::time_point> last_loss_;
    BackendKind last_loss_kind_ = BackendKind::software;
    SwitchListener listener_;
    Clock clock_;
};

// The application entry point. name: auto | software | metal | d3d11 | vulkan | opengl.
// surface null: offscreen target of `size`.
Result<std::unique_ptr<ManagedRenderer>> create_renderer(std::string_view name, const platform::NativeSurface* surface,
                                                         PxSize size, float scale, RenderConfig config = {});

}  // namespace kt::render
