// Linux: Vulkan, then software. OpenGL / GLES is not built yet
// (step 11).
#include <array>

#include "render/backend_factory.h"
#include "render/vulkan/vulkan_backend.h"

namespace kt::render {

std::span<const BackendKind> platform_order() {
    static constexpr std::array<BackendKind, 2> kOrder{BackendKind::vulkan, BackendKind::software};
    return kOrder;
}

namespace detail {
Result<std::unique_ptr<IRenderer>> create_gpu_backend(BackendKind kind, const BackendOptions& opts) {
    if (kind == BackendKind::vulkan) return vulkan::create(opts);
    return make_error(Errc::unavailable, std::string(backend_name(kind)) + " backend is not built on Linux");
}
}  // namespace detail

}  // namespace kt::render
