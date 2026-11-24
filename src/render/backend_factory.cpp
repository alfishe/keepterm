#include "render/backend_factory.h"

#include "render/software/software_backend.h"

namespace kt::render {

Result<std::unique_ptr<IRenderer>> create_backend(BackendKind kind, const BackendOptions& opts) {
    if (kind == BackendKind::software) {
        sw::SoftwareOptions so;
        so.threads = opts.threads;
        so.text_gamma = opts.text_gamma;
        return std::unique_ptr<IRenderer>(std::make_unique<sw::SoftwareRenderer>(so));
    }
    return detail::create_gpu_backend(kind, opts);
}

const char* backend_name(BackendKind kind) {
    switch (kind) {
        case BackendKind::d3d11: return "d3d11";
        case BackendKind::metal: return "metal";
        case BackendKind::vulkan: return "vulkan";
        case BackendKind::opengl: return "opengl";
        case BackendKind::software: return "software";
    }
    return "?";
}

std::optional<BackendKind> parse_backend(std::string_view name) {
    for (BackendKind k : {BackendKind::d3d11, BackendKind::metal, BackendKind::vulkan, BackendKind::opengl,
                          BackendKind::software}) {
        if (name == backend_name(k)) return k;
    }
    return std::nullopt;
}

}  // namespace kt::render
