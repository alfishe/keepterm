// The `software` backend: the own CPU rasterizer.
#pragma once

#include <cstdint>
#include <memory>

#include "render/renderer.h"
#include "render/software/presenter.h"
#include "render/text_gamma.h"

namespace kt::render::sw {

struct Kernels;

struct SoftwareOptions {
    uint32_t threads = 0;              // worker threads; 0 = min(hardware_concurrency - 1, 8)
    const Kernels* kernels = nullptr;  // nullptr = best for this CPU (tests force scalar)
    TextGammaParams text_gamma;
};

struct SoftwareStats {
    uint64_t frames = 0;
    uint32_t tiles = 0;       // tiles rasterized in the last frame
    uint32_t commands = 0;    // tile commands in the last frame
    uint32_t quads = 0;       // quads binned in the last frame
};

class SoftwareRenderer final : public IRenderer {
public:
    static constexpr int kTile = 64;

    explicit SoftwareRenderer(const SoftwareOptions& = {});
    ~SoftwareRenderer() override;

    const Caps& caps() const override;

    // Window surfaces go through create_window_presenter (presenter.h).
    Result<void> attach(const platform::NativeSurface&, PxSize size, float scale) override;
    // Attaches any presenter; the memory presenter is the offscreen target of tests and the probe.
    Result<void> attach_presenter(std::unique_ptr<ISoftwarePresenter>, PxSize size, float scale);
    Result<void> attach_offscreen(PxSize size, float scale) override;   // a MemoryPresenter
    void resize(PxSize size, float scale) override;
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

    void set_text_gamma(const TextGammaParams&);
    const SoftwareStats& stats() const;
    ISoftwarePresenter* presenter() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kt::render::sw
