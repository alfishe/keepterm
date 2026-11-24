// Vulkan backend: Linux and Windows; libvulkan.so.1 / vulkan-1.dll loaded at
// run time, own loader over vkGetInstanceProcAddr. Vulkan 1.0 core plus VK_KHR_surface,
// VK_KHR_swapchain and the window system's surface extension.
#pragma once

#include <cstdint>
#include <memory>

#include "render/backend_factory.h"

namespace kt::render::vulkan {

// Errc::unavailable when the loader, a driver or a usable device is missing. CPU devices
// (lavapipe, llvmpipe, SwiftShader) only with BackendOptions::allow_software_adapter.
// KEEPTERM_VK_VALIDATION=1 enables VK_LAYER_KHRONOS_validation when the loader finds it;
// KEEPTERM_VK_DEVICE=<index> picks a physical device by enumeration index.
Result<std::unique_ptr<IRenderer>> create(const BackendOptions&);

// Validation warnings and errors reported since process start (tests fail on any).
uint32_t validation_messages();
// True when the last created instance has the validation layer enabled.
bool validation_enabled();

}  // namespace kt::render::vulkan
