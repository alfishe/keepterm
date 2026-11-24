// Direct3D 11 backend: Windows build 14393+, d3d11.dll and dxgi.dll loaded at
// run time.
#pragma once

#include <memory>
#include <string>

#include "render/backend_factory.h"

namespace kt::render::d3d11 {

// Errc::unavailable when d3d11.dll / dxgi.dll are missing or no usable adapter exists.
// Software adapters (WARP, Microsoft Basic Render Driver) are used only with
// BackendOptions::allow_software_adapter (tests, benchmarks, the probe when forced);
// KEEPTERM_D3D11_ADAPTER=warp then skips the hardware adapters (tests of the WARP path).
Result<std::unique_ptr<IRenderer>> create(const BackendOptions&);

// The DXGI part of the probe cache key: "dxgi:<vendor>:<device>:<UMD version>"
// per adapter, ';'-separated. Empty when DXGI is missing.
std::string adapter_key();

}  // namespace kt::render::d3d11
