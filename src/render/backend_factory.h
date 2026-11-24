// Backend creation. One header; backend_factory_{win,macos,linux}.cpp know which
// backends exist on their OS. Auto-detection, the probe and the fallback chain are in
// render/render_select.h.
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "base/error.h"
#include "render/renderer.h"
#include "render/text_gamma.h"

namespace kt::render {

struct BackendOptions {
    uint32_t threads = 0;            // software: worker threads, 0 = min(hardware_concurrency - 1, 8)
    // GPU backends may use software adapters (WARP, lavapipe, llvmpipe, Basic Render Driver).
    // Tests, benchmarks and `--probe-renderer --allow-software-adapter` only; never auto-detection.
    bool allow_software_adapter = false;
    TextGammaParams text_gamma;
};

// Platform order for auto-detection, best first. software is always last and always present.
std::span<const BackendKind> platform_order();

// Creates a backend object without a surface. Loads the API library on first use.
// Errc::unavailable: the API, its library or a usable device is missing on this machine.
Result<std::unique_ptr<IRenderer>> create_backend(BackendKind, const BackendOptions&);

const char* backend_name(BackendKind);                   // "d3d11", "metal", ..., "software"
std::optional<BackendKind> parse_backend(std::string_view);

namespace detail {
// Per-OS part of create_backend for the GPU kinds (backend_factory_<os>.cpp).
Result<std::unique_ptr<IRenderer>> create_gpu_backend(BackendKind, const BackendOptions&);
}  // namespace detail

}  // namespace kt::render
