// Vulkan backend on the shared GPU pipeline (gpu/gpu_common.h).
//
// Loader: libvulkan.so.1 (then libvulkan.so) / vulkan-1.dll opened at run time; every entry
// point comes from vkGetInstanceProcAddr / vkGetDeviceProcAddr into the tables below (the
// headers are built with VK_NO_PROTOTYPES, so nothing links against the loader).
// Instance: Vulkan 1.0 with VK_KHR_surface and the surface extension of the window system when
// present (offscreen rendering needs none). Device: graphics queue; VK_KHR_swapchain when
// available. CPU devices are skipped unless explicitly allowed.
//
// Images (the persistent target, its scratch copy and the textures) stay in
// VK_IMAGE_LAYOUT_GENERAL after creation; phases of a command buffer (uploads, render pass,
// copies) are separated by global memory barriers. This keeps the synchronization simple and
// is cheap at the handful of phases a terminal frame has.
//
// Frames: 2 in flight, one command buffer, fence and staging/instance buffers per slot.
// end_frame submits the frame; present acquires a swapchain image, copies the target into it
// (vkCmdCopyImage, or vkCmdBlitImage when the swapchain format is RGBA) and presents (FIFO).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <vulkan/vulkan_xlib.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/log.h"
#include "platform_gui/native_surface.h"
#include "render/damage.h"
#include "render/gpu/gpu_common.h"
#include "render/shaders/gen/quad_spirv.h"
#include "render/vulkan/vulkan_backend.h"

namespace kt::render::vulkan {

namespace {

constexpr int kSlots = 2;              // frames in flight
constexpr int32_t kMaxSide = 16384;
constexpr uint64_t kFenceTimeout = 5'000'000'000ull;   // 5 s: a hung GPU counts as lost

std::atomic<uint32_t> g_validation_messages{0};
std::atomic<bool> g_validation_enabled{false};

// ---------------------------------------------------------------- loader

#ifdef _WIN32
#define KT_VK_SURFACE_EXTENSION VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#define KT_VK_PLATFORM_INSTANCE_FUNCS(X) X(vkCreateWin32SurfaceKHR)
#else
#define KT_VK_SURFACE_EXTENSION VK_KHR_XLIB_SURFACE_EXTENSION_NAME
#define KT_VK_PLATFORM_INSTANCE_FUNCS(X) X(vkCreateXlibSurfaceKHR)
#endif

#define KT_VK_GLOBAL_FUNCS(X)                  \
    X(vkCreateInstance)                        \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define KT_VK_INSTANCE_FUNCS(X)                     \
    X(vkDestroyInstance)                            \
    X(vkEnumeratePhysicalDevices)                   \
    X(vkGetPhysicalDeviceProperties)                \
    X(vkGetPhysicalDeviceQueueFamilyProperties)     \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceFormatProperties)          \
    X(vkEnumerateDeviceExtensionProperties)         \
    X(vkCreateDevice)                               \
    X(vkGetDeviceProcAddr)                          \
    X(vkDestroySurfaceKHR)                          \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)         \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)    \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)         \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)    \
    X(vkCreateDebugUtilsMessengerEXT)               \
    X(vkDestroyDebugUtilsMessengerEXT)              \
    KT_VK_PLATFORM_INSTANCE_FUNCS(X)

#define KT_VK_DEVICE_FUNCS(X)          \
    X(vkDestroyDevice)                 \
    X(vkGetDeviceQueue)                \
    X(vkDeviceWaitIdle)                \
    X(vkQueueSubmit)                   \
    X(vkQueueWaitIdle)                 \
    X(vkCreateCommandPool)             \
    X(vkDestroyCommandPool)            \
    X(vkAllocateCommandBuffers)        \
    X(vkFreeCommandBuffers)            \
    X(vkResetCommandBuffer)            \
    X(vkBeginCommandBuffer)            \
    X(vkEndCommandBuffer)              \
    X(vkCreateFence)                   \
    X(vkDestroyFence)                  \
    X(vkWaitForFences)                 \
    X(vkResetFences)                   \
    X(vkCreateSemaphore)               \
    X(vkDestroySemaphore)              \
    X(vkCreateBuffer)                  \
    X(vkDestroyBuffer)                 \
    X(vkGetBufferMemoryRequirements)   \
    X(vkBindBufferMemory)              \
    X(vkCreateImage)                   \
    X(vkDestroyImage)                  \
    X(vkGetImageMemoryRequirements)    \
    X(vkBindImageMemory)               \
    X(vkAllocateMemory)                \
    X(vkFreeMemory)                    \
    X(vkMapMemory)                     \
    X(vkUnmapMemory)                   \
    X(vkCreateImageView)               \
    X(vkDestroyImageView)              \
    X(vkCreateShaderModule)            \
    X(vkDestroyShaderModule)           \
    X(vkCreateDescriptorSetLayout)     \
    X(vkDestroyDescriptorSetLayout)    \
    X(vkCreatePipelineLayout)          \
    X(vkDestroyPipelineLayout)         \
    X(vkCreateRenderPass)              \
    X(vkDestroyRenderPass)             \
    X(vkCreateGraphicsPipelines)       \
    X(vkDestroyPipeline)               \
    X(vkCreateFramebuffer)             \
    X(vkDestroyFramebuffer)            \
    X(vkCreateDescriptorPool)          \
    X(vkDestroyDescriptorPool)         \
    X(vkAllocateDescriptorSets)        \
    X(vkFreeDescriptorSets)            \
    X(vkUpdateDescriptorSets)          \
    X(vkCmdBeginRenderPass)            \
    X(vkCmdEndRenderPass)              \
    X(vkCmdBindPipeline)               \
    X(vkCmdBindVertexBuffers)          \
    X(vkCmdBindDescriptorSets)         \
    X(vkCmdSetViewport)                \
    X(vkCmdSetScissor)                 \
    X(vkCmdDraw)                       \
    X(vkCmdPipelineBarrier)            \
    X(vkCmdCopyBufferToImage)          \
    X(vkCmdCopyImage)                  \
    X(vkCmdCopyImageToBuffer)          \
    X(vkCmdBlitImage)                  \
    X(vkCmdClearColorImage)            \
    X(vkCreateSwapchainKHR)            \
    X(vkDestroySwapchainKHR)           \
    X(vkGetSwapchainImagesKHR)         \
    X(vkAcquireNextImageKHR)           \
    X(vkQueuePresentKHR)

#define KT_VK_DECLARE(name) PFN_##name name = nullptr;

struct Loader {
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    KT_VK_GLOBAL_FUNCS(KT_VK_DECLARE)
    std::string error;
};

const Loader& loader() {
    static const Loader l = [] {
        Loader r;
        void* proc = nullptr;
#ifdef _WIN32
        HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (lib) proc = reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
#else
        void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (lib) proc = dlsym(lib, "vkGetInstanceProcAddr");
#endif
        if (!lib) {
            r.error = "the Vulkan loader is not installed";
            return r;
        }
        r.get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
        if (!r.get_instance_proc) {
            r.error = "vkGetInstanceProcAddr is missing";
            return r;
        }
#define KT_VK_LOAD_GLOBAL(name) r.name = reinterpret_cast<PFN_##name>(r.get_instance_proc(nullptr, #name));
        KT_VK_GLOBAL_FUNCS(KT_VK_LOAD_GLOBAL)
#undef KT_VK_LOAD_GLOBAL
        if (!r.vkCreateInstance || !r.vkEnumerateInstanceExtensionProperties) r.error = "the Vulkan loader is incomplete";
        return r;
    }();
    return l;
}

struct InstanceFns {
    KT_VK_INSTANCE_FUNCS(KT_VK_DECLARE)
};
struct DeviceFns {
    KT_VK_DEVICE_FUNCS(KT_VK_DECLARE)
};

const char* vk_result_name(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult";
    }
}

Unexpected<Error> vk_error(const char* what, VkResult r) {
    const Errc code = r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR ? Errc::lost : Errc::unavailable;
    return make_error(code, std::string(what) + ": " + vk_result_name(r) + " (" + std::to_string(static_cast<int>(r)) + ")", r);
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        g_validation_messages.fetch_add(1);
        log::warn("render.vulkan", data && data->pMessage ? data->pMessage : "validation message");
    }
    return VK_FALSE;
}

// Environment value or empty (GetEnvironmentVariableA on Windows: the CRT getenv is deprecated
// there and warns).
std::string env_value(const char* name) {
#ifdef _WIN32
    char buf[256];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string();
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

bool env_on(const char* name) {
    const std::string v = env_value(name);
    return !v.empty() && v != "0";
}

VkFormat vk_format(TexFormat f) {
    switch (f) {
        case TexFormat::a8: return VK_FORMAT_R8_UNORM;
        case TexFormat::rgba8_lcd: return VK_FORMAT_R8G8B8A8_UNORM;
        case TexFormat::bgra8_premul: return VK_FORMAT_B8G8R8A8_UNORM;
    }
    return VK_FORMAT_R8_UNORM;
}

// ---------------------------------------------------------------- resources

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceSize size = 0;
};

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};

struct Texture {
    Img img;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    TextureDesc desc;
    bool initialized = false;   // layout GENERAL and cleared
};

struct PendingUpload {
    TextureId id;
    IRect region;
    std::vector<uint8_t> data;   // tight rows
};

// Objects to destroy once the GPU passed the slot that last used them.
struct Garbage {
    std::vector<Buffer> buffers;
    std::vector<Img> images;
    std::vector<std::pair<VkDescriptorPool, VkDescriptorSet>> sets;
};

struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBuffer present_cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;           // frame submission
    VkFence present_fence = VK_NULL_HANDLE;   // present copy submission
    bool fence_pending = false, present_pending = false;
    VkSemaphore acquired = VK_NULL_HANDLE;
    Buffer instances;
    VkDeviceSize instances_used = 0;
    Buffer staging;
    VkDeviceSize staging_used = 0;
    Garbage garbage;
};

class VulkanRenderer final : public IRenderer {
public:
    ~VulkanRenderer() override { destroy_all(); }

    Result<void> init(const BackendOptions& opts) {
        tg_ = opts.text_gamma;
        allow_cpu_ = opts.allow_software_adapter;
        const Loader& L = loader();
        if (!L.error.empty()) return make_error(Errc::unavailable, L.error);
        if (auto ok = create_instance(); !ok) return ok;
        if (auto ok = pick_device(allow_cpu_, VK_NULL_HANDLE); !ok) return ok;
        if (auto ok = create_device_objects(); !ok) return ok;
        return {};
    }

    Result<void> create_device_objects() {
        if (auto ok = create_device(); !ok) return ok;
        if (auto ok = create_pipeline(); !ok) return ok;
        if (auto ok = create_frame_resources(); !ok) return ok;
        caps_.kind = BackendKind::vulkan;
        caps_.adapter_name = props_.deviceName;
        caps_.vendor_id = props_.vendorID;
        caps_.device_id = props_.deviceID;
        caps_.max_texture_size = std::min<uint32_t>(props_.limits.maxImageDimension2D, 16384);
        caps_.dual_source_blend = false;   // LCD text is grayscale until per-channel blending lands
        caps_.partial_present = false;     // VK_KHR_incremental_present is a later step
        caps_.scroll_blit = true;
        caps_.premultiplied_window_alpha = false;
        caps_.vsync_control = true;
        caps_.frames_in_flight = kSlots;
        return {};
    }

    const Caps& caps() const override { return caps_; }

    Result<void> attach(const platform::NativeSurface& s, PxSize size, float scale) override {
        detach();
        if (!surface_ext_) return make_error(Errc::unavailable, "the Vulkan surface extensions are missing");
        if (!swapchain_ext_) return make_error(Errc::unavailable, "VK_KHR_swapchain is missing");
        VkResult r = VK_ERROR_INITIALIZATION_FAILED;
#ifdef _WIN32
        if (s.ws != platform::WindowSystem::win32 || !s.handle) return make_error(Errc::invalid_argument, "Vulkan needs a Win32 HWND here");
        VkWin32SurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        ci.hinstance = GetModuleHandleW(nullptr);
        ci.hwnd = static_cast<HWND>(s.handle);
        r = in_.vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &surface_);
#else
        if (s.ws != platform::WindowSystem::x11 || !s.display || !s.xid)
            return make_error(Errc::not_supported, "Vulkan needs an X11 surface (Wayland is a later step)");
        VkXlibSurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        ci.dpy = static_cast<Display*>(s.display);
        ci.window = static_cast<Window>(s.xid);
        r = in_.vkCreateXlibSurfaceKHR(instance_, &ci, nullptr, &surface_);
#endif
        if (r != VK_SUCCESS) return vk_error("vkCreate*SurfaceKHR", r);
        VkBool32 supported = VK_FALSE;
        in_.vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, surface_, &supported);
        if (!supported) {
            // usable = can present to the surface. Before any texture exists
            // (the normal order: attach first), move to a device that can, e.g. the GPU that
            // drives this X screen on a hybrid machine.
            const std::string first = props_.deviceName;
            VkSurfaceKHR keep = surface_;
            surface_ = VK_NULL_HANDLE;
            if (textures_.size() > 1) {
                in_.vkDestroySurfaceKHR(instance_, keep, nullptr);
                return make_error(Errc::unavailable, first + " cannot present to this surface");
            }
            destroy_device_objects();
            phys_ = VK_NULL_HANDLE;
            Result<void> ok = pick_device(allow_cpu_, keep);
            if (ok) ok = create_device_objects();
            if (!ok) {
                in_.vkDestroySurfaceKHR(instance_, keep, nullptr);
                lost_ = true;   // no device left: the caller moves to the next backend
                lost_reason_ = first + " cannot present to this surface; " + ok.error().what;
                return make_error(Errc::unavailable, lost_reason_);
            }
            surface_ = keep;
            log::info("render.vulkan", first + " cannot present to this surface; using " + props_.deviceName);
        }
        size_ = clamp_size(size);
        scale_ = scale;
        if (auto ok = create_target(); !ok) {
            detach();
            return ok;
        }
        if (auto ok = create_swapchain(); !ok) {
            detach();
            return ok;
        }
        return {};
    }

    Result<void> attach_offscreen(PxSize size, float scale) override {
        detach();
        offscreen_ = true;
        size_ = clamp_size(size);
        scale_ = scale;
        return create_target();
    }

    void resize(PxSize size, float scale) override {
        scale_ = scale;
        size = clamp_size(size);
        if (size == size_ && target_.image) return;
        size_ = size;
        (void)create_target();
        swapchain_dirty_ = true;
    }

    void detach() override {
        if (!device_) return;
        if (in_frame_) end_frame_internal();
        wait_idle();
        destroy_swapchain();
        if (surface_) in_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        destroy_target();
        offscreen_ = false;
        size_ = PxSize{};
    }

    Result<TextureId> create_texture(const TextureDesc& desc) override {
        if (desc.w == 0 || desc.h == 0) return make_error(Errc::invalid_argument, "empty texture");
        if (desc.w > caps_.max_texture_size || desc.h > caps_.max_texture_size) return make_error(Errc::invalid_argument, "texture too large");
        Texture t;
        t.desc = desc;
        if (auto ok = create_image(t.img, desc.w, desc.h, vk_format(desc.format), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT); !ok)
            return Unexpected<Error>(ok.error());
        if (auto ok = allocate_set(t.img.view, t.set, t.pool); !ok) {
            destroy_image(t.img);
            return Unexpected<Error>(ok.error());
        }
        const TextureId id = next_texture_++;
        textures_.emplace(id, std::move(t));
        return id;
    }

    void update_texture(TextureId id, IRect region, const void* pixels, uint32_t stride) override {
        auto it = textures_.find(id);
        if (it == textures_.end()) return;
        const Texture& t = it->second;
        const uint32_t bpp = gpu::texel_bytes(t.desc.format);
        auto src = static_cast<const uint8_t*>(pixels);
        if (!gpu::clip_upload(region, src, stride, t.desc.w, t.desc.h, bpp)) return;
        PendingUpload up{id, region, {}};
        const size_t row = static_cast<size_t>(region.w) * bpp;
        up.data.resize(row * static_cast<size_t>(region.h));
        for (int32_t y = 0; y < region.h; ++y) std::memcpy(up.data.data() + row * static_cast<size_t>(y), src + static_cast<size_t>(y) * stride, row);
        uploads_.push_back(std::move(up));
    }

    void destroy_texture(TextureId id) override {
        auto it = textures_.find(id);
        if (it == textures_.end()) return;
        Garbage& g = garbage_now();
        g.images.push_back(it->second.img);
        g.sets.emplace_back(it->second.pool, it->second.set);
        std::erase_if(uploads_, [&](const PendingUpload& u) { return u.id == id; });
        textures_.erase(it);
    }

    Result<bool> begin_frame(const Damage& damage) override {
        if (lost_) return make_error(Errc::lost, lost_reason_);
        if (!target_.image || (!surface_ && !offscreen_)) return false;
        if (surface_ && (swapchain_dirty_ || !swapchain_)) {
            if (auto ok = create_swapchain(); !ok) {
                if (ok.error().code == Errc::lost) return Unexpected<Error>(ok.error());
                return false;
            }
            if (!swapchain_) return false;   // zero extent (minimized)
        }
        if (auto ok = begin_slot(); !ok) return Unexpected<Error>(ok.error());
        frames_.begin(size_, damage);
        in_frame_ = true;
        return true;
    }

    void submit(const DrawList& dl) override {
        if (!in_frame_) return;
        frames_.add(dl, [&](TextureId id, uint16_t& w, uint16_t& h) {
            auto it = textures_.find(id);
            if (it == textures_.end()) return false;
            w = it->second.desc.w;
            h = it->second.desc.h;
            return true;
        });
    }

    Result<void> end_frame() override {
        if (!in_frame_) return make_error(Errc::invalid_argument, "end_frame without begin_frame");
        end_frame_internal();
        if (lost_) return make_error(Errc::lost, lost_reason_);
        return {};
    }

    Result<void> present() override {
        if (lost_) return make_error(Errc::lost, lost_reason_);
        if (!surface_ || !swapchain_ || !frame_submitted_) return {};
        frame_submitted_ = false;
        Slot& s = slots_[slot_];
        uint32_t index = 0;
        VkResult r = dev_.vkAcquireNextImageKHR(device_, swapchain_, 1'000'000'000ull, s.acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain_dirty_ = true;
            return {};
        }
        if (r == VK_TIMEOUT || r == VK_NOT_READY) return {};
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return fail("vkAcquireNextImageKHR", r);
        if (r == VK_SUBOPTIMAL_KHR) swapchain_dirty_ = true;

        VkCommandBuffer cmd = s.present_cmd;
        dev_.vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dev_.vkBeginCommandBuffer(cmd, &bi);
        global_barrier(cmd);
        VkImage img = sc_images_[index];
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const uint32_t w = std::min(sc_extent_.width, target_.w), h = std::min(sc_extent_.height, target_.h);
        if (sc_format_ == VK_FORMAT_B8G8R8A8_UNORM) {
            VkImageCopy c{};
            c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.dstSubresource = c.srcSubresource;
            c.extent = {w, h, 1};
            dev_.vkCmdCopyImage(cmd, target_.image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        } else {
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
            b.dstOffsets[1] = b.srcOffsets[1];
            dev_.vkCmdBlitImage(cmd, target_.image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        }
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        dev_.vkEndCommandBuffer(cmd);

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sc_done_[index];
        dev_.vkResetFences(device_, 1, &s.present_fence);
        r = dev_.vkQueueSubmit(queue_, 1, &si, s.present_fence);
        if (r != VK_SUCCESS) return fail("vkQueueSubmit (present)", r);
        s.present_pending = true;

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sc_done_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &index;
        r = dev_.vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
            return {};
        }
        if (r != VK_SUCCESS) return fail("vkQueuePresentKHR", r);
        return {};
    }

    void copy_rect(IRect src, int32_t dst_x, int32_t dst_y) override {
        if (!in_frame_ || !target_.image) return;
        IRect sr, dr;
        if (!clip_copy(src, dst_x, dst_y, size_, sr, dr)) return;
        flush();
        if (!scratch_.image &&
            !create_image(scratch_, target_.w, target_.h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return;
        VkCommandBuffer cmd = slots_[slot_].cmd;
        if (!scratch_initialized_) {
            image_barrier(cmd, scratch_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            scratch_initialized_ = true;
        }
        global_barrier(cmd);
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = c.srcSubresource;
        c.srcOffset = {sr.x, sr.y, 0};
        c.dstOffset = {sr.x, sr.y, 0};
        c.extent = {static_cast<uint32_t>(sr.w), static_cast<uint32_t>(sr.h), 1};
        dev_.vkCmdCopyImage(cmd, target_.image, VK_IMAGE_LAYOUT_GENERAL, scratch_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        global_barrier(cmd);
        c.dstOffset = {dr.x, dr.y, 0};
        dev_.vkCmdCopyImage(cmd, scratch_.image, VK_IMAGE_LAYOUT_GENERAL, target_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        frames_.add_copy_damage(dr);
    }

    Result<Image> read_back(IRect rect) override {
        if (lost_) return make_error(Errc::lost, lost_reason_);
        if (!target_.image) return make_error(Errc::unavailable, "no frame");
        if (in_frame_) end_frame_internal();
        const IRect r = intersect(rect, IRect{0, 0, size_.w, size_.h});
        Image img;
        img.order = PixelOrder::bgra;
        if (r.empty()) return img;
        Buffer buf;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(r.w) * static_cast<VkDeviceSize>(r.h) * 4;
        if (auto ok = create_buffer(buf, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true); !ok) return Unexpected<Error>(ok.error());
        auto ok = one_shot([&](VkCommandBuffer cmd) {
            global_barrier(cmd);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageOffset = {r.x, r.y, 0};
            c.imageExtent = {static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h), 1};
            dev_.vkCmdCopyImageToBuffer(cmd, target_.image, VK_IMAGE_LAYOUT_GENERAL, buf.buf, 1, &c);
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            dev_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        });
        if (!ok) {
            destroy_buffer(buf);
            return Unexpected<Error>(ok.error());
        }
        img.w = r.w;
        img.h = r.h;
        img.pixels.resize(static_cast<size_t>(r.w) * static_cast<size_t>(r.h));
        std::memcpy(img.pixels.data(), buf.map, static_cast<size_t>(bytes));
        destroy_buffer(buf);
        return img;
    }

private:
    static PxSize clamp_size(PxSize s) { return PxSize{std::clamp(s.w, 0, kMaxSide), std::clamp(s.h, 0, kMaxSide)}; }

    Unexpected<Error> fail(const char* what, VkResult r) {
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR) {
            lost_ = true;
            lost_reason_ = std::string(what) + ": " + vk_result_name(r);
        }
        return vk_error(what, r);
    }

    // ------------------------------------------------------------ instance and device

    Result<void> create_instance() {
        const Loader& L = loader();
        uint32_t n = 0;
        L.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        L.vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
        auto has = [&](const char* name) {
            return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        std::vector<const char*> enable;
        if (has(VK_KHR_SURFACE_EXTENSION_NAME) && has(KT_VK_SURFACE_EXTENSION)) {
            enable.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            enable.push_back(KT_VK_SURFACE_EXTENSION);
            surface_ext_ = true;
        }
        std::vector<const char*> layers;
        bool debug = false;
        if (env_on("KEEPTERM_VK_VALIDATION") && L.vkEnumerateInstanceLayerProperties) {
            uint32_t ln = 0;
            L.vkEnumerateInstanceLayerProperties(&ln, nullptr);
            std::vector<VkLayerProperties> lp(ln);
            L.vkEnumerateInstanceLayerProperties(&ln, lp.data());
            for (const auto& l : lp) {
                if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
            }
            if (layers.empty()) log::warn("render.vulkan", "KEEPTERM_VK_VALIDATION is set but VK_LAYER_KHRONOS_validation is not installed");
            if (!layers.empty() && has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                enable.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debug = true;
            }
        }
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "keepterm";
        app.pEngineName = "keepterm";
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
        ci.ppEnabledExtensionNames = enable.data();
        ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
        ci.ppEnabledLayerNames = layers.data();
        const VkResult r = L.vkCreateInstance(&ci, nullptr, &instance_);
        if (r != VK_SUCCESS) return vk_error("vkCreateInstance", r);
#define KT_VK_LOAD_INSTANCE(name) in_.name = reinterpret_cast<PFN_##name>(L.get_instance_proc(instance_, #name));
        KT_VK_INSTANCE_FUNCS(KT_VK_LOAD_INSTANCE)
#undef KT_VK_LOAD_INSTANCE
        if (!in_.vkEnumeratePhysicalDevices || !in_.vkCreateDevice || !in_.vkGetDeviceProcAddr)
            return make_error(Errc::unavailable, "Vulkan instance functions are missing");
        g_validation_enabled.store(!layers.empty());
        if (debug && in_.vkCreateDebugUtilsMessengerEXT) {
            VkDebugUtilsMessengerCreateInfoEXT mi{};
            mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_callback;
            in_.vkCreateDebugUtilsMessengerEXT(instance_, &mi, nullptr, &messenger_);
        }
        return {};
    }

    // surface: when set, only devices with a graphics queue family that can present to it.
    Result<void> pick_device(bool allow_cpu, VkSurfaceKHR surface) {
        uint32_t n = 0;
        VkResult r = in_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        if (r != VK_SUCCESS) return vk_error("vkEnumeratePhysicalDevices", r);
        std::vector<VkPhysicalDevice> devs(n);
        in_.vkEnumeratePhysicalDevices(instance_, &n, devs.data());
        if (devs.empty()) return make_error(Errc::unavailable, "no Vulkan device");
        int forced = -1;
        if (const std::string v = env_value("KEEPTERM_VK_DEVICE"); !v.empty()) forced = std::atoi(v.c_str());
        // Preference: integrated (a terminal does not need the discrete GPU), discrete, virtual,
        // other, CPU (only when allowed).
        auto rank = [](VkPhysicalDeviceType t) {
            switch (t) {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 0;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 1;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: return 4;
                default: return 3;
            }
        };
        int best_rank = 99;
        std::string reasons;
        for (uint32_t i = 0; i < n; ++i) {
            VkPhysicalDeviceProperties p{};
            in_.vkGetPhysicalDeviceProperties(devs[i], &p);
            if (forced >= 0 && static_cast<int>(i) != forced) continue;
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !allow_cpu) {
                reasons += std::string("skipped CPU device ") + p.deviceName + "; ";
                continue;
            }
            uint32_t qn = 0;
            in_.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            in_.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf.data());
            int family = -1;
            for (uint32_t q = 0; q < qn; ++q) {
                if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                if (surface) {
                    VkBool32 can = VK_FALSE;
                    in_.vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface, &can);
                    if (!can) continue;
                }
                family = static_cast<int>(q);
                break;
            }
            if (family < 0) {
                if (surface) reasons += std::string(p.deviceName) + " cannot present; ";
                continue;
            }
            VkFormatProperties fp{};
            in_.vkGetPhysicalDeviceFormatProperties(devs[i], VK_FORMAT_B8G8R8A8_UNORM, &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((fp.optimalTilingFeatures & need) != need) continue;
            const int rk = rank(p.deviceType);
            if (rk < best_rank) {
                best_rank = rk;
                phys_ = devs[i];
                props_ = p;
                queue_family_ = static_cast<uint32_t>(family);
            }
        }
        if (!phys_) return make_error(Errc::unavailable, reasons.empty() ? "no usable Vulkan device" : "no usable Vulkan device: " + reasons);
        in_.vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props_);
        return {};
    }

    Result<void> create_device() {
        uint32_t n = 0;
        in_.vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        in_.vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, exts.data());
        std::vector<const char*> enable;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0 && surface_ext_) {
                enable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                swapchain_ext_ = true;
            }
        }
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queue_family_;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qi;
        ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
        ci.ppEnabledExtensionNames = enable.data();
        const VkResult r = in_.vkCreateDevice(phys_, &ci, nullptr, &device_);
        if (r != VK_SUCCESS) return vk_error("vkCreateDevice", r);
#define KT_VK_LOAD_DEVICE(name) dev_.name = reinterpret_cast<PFN_##name>(in_.vkGetDeviceProcAddr(device_, #name));
        KT_VK_DEVICE_FUNCS(KT_VK_LOAD_DEVICE)
#undef KT_VK_LOAD_DEVICE
        if (!dev_.vkQueueSubmit || !dev_.vkCmdDraw) return make_error(Errc::unavailable, "Vulkan device functions are missing");
        dev_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        return {};
    }

    Result<void> create_pipeline() {
        auto module = [&](const unsigned char* code, size_t size, VkShaderModule& out) {
            VkShaderModuleCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            ci.codeSize = size;
            ci.pCode = reinterpret_cast<const uint32_t*>(code);
            return dev_.vkCreateShaderModule(device_, &ci, nullptr, &out);
        };
        VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
        VkResult r = module(shaders::kQuadVsSpirv, sizeof(shaders::kQuadVsSpirv), vs);
        if (r == VK_SUCCESS) r = module(shaders::kQuadPsSpirv, sizeof(shaders::kQuadPsSpirv), ps);
        if (r != VK_SUCCESS) {
            if (vs) dev_.vkDestroyShaderModule(device_, vs, nullptr);
            return vk_error("vkCreateShaderModule", r);
        }

        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings = b;
        r = dev_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layout_);
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout_;
        if (r == VK_SUCCESS) r = dev_.vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_);

        VkAttachmentDescription att{};
        att.format = VK_FORMAT_B8G8R8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // partial redraw: undamaged pixels stay
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments = &att;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        if (r == VK_SUCCESS) r = dev_.vkCreateRenderPass(device_, &rpi, nullptr, &render_pass_);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "vs_main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps;
        stages[1].pName = "ps_main";
        VkVertexInputBindingDescription vb{0, gpu::kInstanceStride, VK_VERTEX_INPUT_RATE_INSTANCE};
        VkVertexInputAttributeDescription va[gpu::kInstanceAttributes]{};
        for (uint32_t a = 0; a < gpu::kInstanceAttributes; ++a) {
            va[a].location = a;
            va[a].binding = 0;
            va[a].format = a == 3 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            va[a].offset = a * 16;
        }
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = gpu::kInstanceAttributes;
        vi.pVertexAttributeDescriptions = va;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount = 2;
        pi.pStages = stages;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vps;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &ds;
        pi.layout = pipeline_layout_;
        pi.renderPass = render_pass_;
        if (r == VK_SUCCESS) r = dev_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_);
        dev_.vkDestroyShaderModule(device_, vs, nullptr);
        dev_.vkDestroyShaderModule(device_, ps, nullptr);
        if (r != VK_SUCCESS) return vk_error("pipeline creation", r);
        return {};
    }

    Result<void> create_frame_resources() {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family_;
        VkResult r = dev_.vkCreateCommandPool(device_, &pci, nullptr, &cmd_pool_);
        if (r != VK_SUCCESS) return vk_error("vkCreateCommandPool", r);
        for (Slot& s : slots_) {
            VkCommandBufferAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = cmd_pool_;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            r = dev_.vkAllocateCommandBuffers(device_, &ai, &s.cmd);
            if (r == VK_SUCCESS) r = dev_.vkAllocateCommandBuffers(device_, &ai, &s.present_cmd);
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (r == VK_SUCCESS) r = dev_.vkCreateFence(device_, &fi, nullptr, &s.fence);
            if (r == VK_SUCCESS) r = dev_.vkCreateFence(device_, &fi, nullptr, &s.present_fence);
            VkSemaphoreCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (r == VK_SUCCESS) r = dev_.vkCreateSemaphore(device_, &si, nullptr, &s.acquired);
            if (r != VK_SUCCESS) return vk_error("frame resources", r);
        }
        if (auto ok = create_buffer(constants_, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false); !ok) return ok;
        write_constants();
        Texture dummy;
        if (auto ok = create_image(dummy.img, 1, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT); !ok) return ok;
        if (auto ok = allocate_set(dummy.img.view, dummy.set, dummy.pool); !ok) return ok;
        dummy.desc = TextureDesc{1, 1, TexFormat::a8, false};
        textures_.emplace(0, std::move(dummy));   // id 0: bound for untextured batches
        return {};
    }

    void write_constants() {
        const gpu::FrameConstants c = gpu::make_constants(size_, 1.0f, tg_);
        std::memcpy(constants_.map, &c, sizeof(c));
    }

    // ------------------------------------------------------------ memory, buffers, images

    int memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & want) == want) return static_cast<int>(i);
        }
        return -1;
    }

    Result<void> create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool readback) {
        VkBufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size = size;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult r = dev_.vkCreateBuffer(device_, &ci, nullptr, &b.buf);
        if (r != VK_SUCCESS) return fail("vkCreateBuffer", r);
        VkMemoryRequirements req{};
        dev_.vkGetBufferMemoryRequirements(device_, b.buf, &req);
        const VkMemoryPropertyFlags base = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        int type = readback ? memory_type(req.memoryTypeBits, base | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : -1;
        if (type < 0) type = memory_type(req.memoryTypeBits, base);
        if (type < 0) {
            destroy_buffer(b);
            return make_error(Errc::unavailable, "no host-visible coherent Vulkan memory");
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(type);
        r = dev_.vkAllocateMemory(device_, &ai, nullptr, &b.mem);
        if (r == VK_SUCCESS) r = dev_.vkBindBufferMemory(device_, b.buf, b.mem, 0);
        if (r == VK_SUCCESS) r = dev_.vkMapMemory(device_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map);
        if (r != VK_SUCCESS) {
            destroy_buffer(b);
            return fail("buffer memory", r);
        }
        b.size = size;
        return {};
    }

    void destroy_buffer(Buffer& b) {
        if (b.map) dev_.vkUnmapMemory(device_, b.mem);
        if (b.buf) dev_.vkDestroyBuffer(device_, b.buf, nullptr);
        if (b.mem) dev_.vkFreeMemory(device_, b.mem, nullptr);
        b = Buffer{};
    }

    Result<void> create_image(Img& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = dev_.vkCreateImage(device_, &ci, nullptr, &img.image);
        if (r != VK_SUCCESS) return fail("vkCreateImage", r);
        VkMemoryRequirements req{};
        dev_.vkGetImageMemoryRequirements(device_, img.image, &req);
        int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = memory_type(req.memoryTypeBits, 0);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(std::max(type, 0));
        r = dev_.vkAllocateMemory(device_, &ai, nullptr, &img.mem);
        if (r == VK_SUCCESS) r = dev_.vkBindImageMemory(device_, img.image, img.mem, 0);
        if (r == VK_SUCCESS && (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            r = dev_.vkCreateImageView(device_, &vi, nullptr, &img.view);
        }
        if (r != VK_SUCCESS) {
            destroy_image(img);
            return fail("image memory", r);
        }
        img.w = w;
        img.h = h;
        return {};
    }

    void destroy_image(Img& img) {
        if (img.view) dev_.vkDestroyImageView(device_, img.view, nullptr);
        if (img.image) dev_.vkDestroyImage(device_, img.image, nullptr);
        if (img.mem) dev_.vkFreeMemory(device_, img.mem, nullptr);
        img = Img{};
    }

    Result<void> allocate_set(VkImageView view, VkDescriptorSet& set, VkDescriptorPool& pool_out) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (pools_.empty() || attempt == 1) {
                VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256}};
                VkDescriptorPoolCreateInfo pi{};
                pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                pi.maxSets = 256;
                pi.poolSizeCount = 2;
                pi.pPoolSizes = sizes;
                VkDescriptorPool p = VK_NULL_HANDLE;
                const VkResult r = dev_.vkCreateDescriptorPool(device_, &pi, nullptr, &p);
                if (r != VK_SUCCESS) return fail("vkCreateDescriptorPool", r);
                pools_.push_back(p);
            }
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pools_.back();
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &set_layout_;
            const VkResult r = dev_.vkAllocateDescriptorSets(device_, &ai, &set);
            if (r == VK_SUCCESS) {
                pool_out = pools_.back();
                VkDescriptorBufferInfo bi{constants_.buf, 0, sizeof(gpu::FrameConstants)};
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w[2]{};
                w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[0].dstSet = w[1].dstSet = set;
                w[0].dstBinding = 0;
                w[0].descriptorCount = 1;
                w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w[0].pBufferInfo = &bi;
                w[1].dstBinding = 1;
                w[1].descriptorCount = 1;
                w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                w[1].pImageInfo = &ii;
                dev_.vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
                return {};
            }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return fail("vkAllocateDescriptorSets", r);
        }
        return make_error(Errc::unavailable, "descriptor pool exhausted");
    }

    // ------------------------------------------------------------ target and swapchain

    Result<void> create_target() {
        wait_idle();
        destroy_target();
        write_constants();
        if (size_.w == 0 || size_.h == 0) return {};
        if (auto ok = create_image(target_, static_cast<uint32_t>(size_.w), static_cast<uint32_t>(size_.h), VK_FORMAT_B8G8R8A8_UNORM,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            !ok)
            return ok;
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = render_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &target_.view;
        fi.width = target_.w;
        fi.height = target_.h;
        fi.layers = 1;
        const VkResult r = dev_.vkCreateFramebuffer(device_, &fi, nullptr, &framebuffer_);
        if (r != VK_SUCCESS) return fail("vkCreateFramebuffer", r);
        // GENERAL layout and transparent black, like a fresh software back buffer.
        return one_shot([&](VkCommandBuffer cmd) {
            image_barrier(cmd, target_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dev_.vkCmdClearColorImage(cmd, target_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        });
    }

    void destroy_target() {
        if (framebuffer_) dev_.vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
        destroy_image(target_);
        destroy_image(scratch_);
        scratch_initialized_ = false;
    }

    Result<void> create_swapchain() {
        wait_idle();
        VkSurfaceCapabilitiesKHR sc{};
        VkResult r = in_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &sc);
        if (r != VK_SUCCESS) return fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
        VkExtent2D extent = sc.currentExtent;
        if (extent.width == 0xFFFFFFFFu) extent = {static_cast<uint32_t>(size_.w), static_cast<uint32_t>(size_.h)};
        extent.width = std::clamp(extent.width, sc.minImageExtent.width, std::max(sc.minImageExtent.width, sc.maxImageExtent.width));
        extent.height = std::clamp(extent.height, sc.minImageExtent.height, std::max(sc.minImageExtent.height, sc.maxImageExtent.height));
        if (extent.width == 0 || extent.height == 0 || size_.w == 0 || size_.h == 0) {
            destroy_swapchain();
            swapchain_dirty_ = false;
            return {};   // minimized: no swapchain until the next resize
        }
        if (!(sc.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return make_error(Errc::unavailable, "swapchain images cannot be transfer destinations");
        uint32_t n = 0;
        in_.vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        in_.vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, formats.data());
        VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            for (const auto& f : formats) {
                if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && chosen.format == VK_FORMAT_UNDEFINED) chosen = f;
            }
        }
        if (chosen.format == VK_FORMAT_UNDEFINED) return make_error(Errc::unavailable, "no BGRA8/RGBA8 UNORM swapchain format");
        uint32_t count = std::max(2u, sc.minImageCount);
        if (sc.maxImageCount > 0) count = std::min(count, sc.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(sc.supportedCompositeAlpha & static_cast<uint32_t>(alpha))) {
            for (uint32_t bit = 1; bit <= static_cast<uint32_t>(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR); bit <<= 1) {
                if (sc.supportedCompositeAlpha & bit) {
                    alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                    break;
                }
            }
        }
        VkSwapchainKHR old = swapchain_;
        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_;
        ci.minImageCount = count;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = sc.currentTransform;
        ci.compositeAlpha = alpha;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync; always available
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;
        VkSwapchainKHR fresh = VK_NULL_HANDLE;
        r = dev_.vkCreateSwapchainKHR(device_, &ci, nullptr, &fresh);
        if (old) {
            swapchain_ = old;
            destroy_swapchain();
        }
        if (r != VK_SUCCESS) return fail("vkCreateSwapchainKHR", r);
        swapchain_ = fresh;
        sc_format_ = chosen.format;
        sc_extent_ = extent;
        n = 0;
        dev_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        sc_images_.resize(n);
        dev_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, sc_images_.data());
        sc_done_.assign(n, VK_NULL_HANDLE);
        for (auto& s : sc_done_) {
            VkSemaphoreCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            dev_.vkCreateSemaphore(device_, &si, nullptr, &s);
        }
        swapchain_dirty_ = false;
        return {};
    }

    void destroy_swapchain() {
        for (VkSemaphore s : sc_done_) {
            if (s) dev_.vkDestroySemaphore(device_, s, nullptr);
        }
        sc_done_.clear();
        sc_images_.clear();
        if (swapchain_) dev_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    // ------------------------------------------------------------ command recording

    void global_barrier(VkCommandBuffer cmd) {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        dev_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                       VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src_access;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dev_.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    template <class F>
    Result<void> one_shot(F&& record) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = cmd_pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkResult r = dev_.vkAllocateCommandBuffers(device_, &ai, &cmd);
        if (r != VK_SUCCESS) return fail("vkAllocateCommandBuffers", r);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dev_.vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        dev_.vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        r = dev_.vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = dev_.vkQueueWaitIdle(queue_);
        dev_.vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
        if (r != VK_SUCCESS) return fail("one-shot submit", r);
        return {};
    }

    Garbage& garbage_now() {
        // Recording: the current slot's command buffer may reference the object. Otherwise the
        // last submitted slot is the latest user.
        return slots_[static_cast<size_t>(in_frame_ ? slot_ : last_submitted_)].garbage;
    }

    void collect(Garbage& g) {
        for (Buffer& b : g.buffers) destroy_buffer(b);
        for (Img& i : g.images) destroy_image(i);
        for (auto& [pool, set] : g.sets) {
            if (pool && set) dev_.vkFreeDescriptorSets(device_, pool, 1, &set);
        }
        g = Garbage{};
    }

    Result<void> wait_slot(Slot& s) {
        VkFence fences[2];
        uint32_t n = 0;
        if (s.fence_pending) fences[n++] = s.fence;
        if (s.present_pending) fences[n++] = s.present_fence;
        if (n > 0) {
            const VkResult r = dev_.vkWaitForFences(device_, n, fences, VK_TRUE, kFenceTimeout);
            if (r == VK_TIMEOUT) {
                lost_ = true;
                lost_reason_ = "GPU did not finish a frame within 5 s";
                return make_error(Errc::lost, lost_reason_);
            }
            if (r != VK_SUCCESS) return fail("vkWaitForFences", r);
        }
        s.fence_pending = s.present_pending = false;
        collect(s.garbage);
        return {};
    }

    void wait_idle() {
        if (!device_) return;
        dev_.vkDeviceWaitIdle(device_);
        for (Slot& s : slots_) {
            s.fence_pending = s.present_pending = false;
            collect(s.garbage);
        }
    }

    Result<void> begin_slot() {
        slot_ = (slot_ + 1) % kSlots;
        Slot& s = slots_[static_cast<size_t>(slot_)];
        if (auto ok = wait_slot(s); !ok) return ok;
        s.instances_used = 0;
        s.staging_used = 0;
        dev_.vkResetCommandBuffer(s.cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        const VkResult r = dev_.vkBeginCommandBuffer(s.cmd, &bi);
        if (r != VK_SUCCESS) return fail("vkBeginCommandBuffer", r);
        return {};
    }

    // Copies pending texture data into the slot's staging buffer and records the copies.
    void record_uploads(VkCommandBuffer cmd) {
        Slot& s = slots_[static_cast<size_t>(slot_)];
        bool any_init = false;
        for (auto& [id, t] : textures_) {
            if (!t.initialized) any_init = true;
        }
        if (uploads_.empty() && !any_init) return;
        global_barrier(cmd);
        for (auto& [id, t] : textures_) {
            if (t.initialized) continue;
            image_barrier(cmd, t.img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dev_.vkCmdClearColorImage(cmd, t.img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            t.initialized = true;
        }
        if (!uploads_.empty()) {
            global_barrier(cmd);   // clears before copies
            VkDeviceSize need = s.staging_used;
            for (const auto& u : uploads_) need += (u.data.size() + 15) & ~VkDeviceSize{15};
            if (need > s.staging.size) {
                if (s.staging.buf) s.garbage.buffers.push_back(s.staging);   // still used by this command buffer
                s.staging = Buffer{};
                if (!create_buffer(s.staging, std::max<VkDeviceSize>(need * 2, VkDeviceSize{1} << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false)) {
                    uploads_.clear();
                    return;
                }
                s.staging_used = 0;
            }
            for (const auto& u : uploads_) {
                auto it = textures_.find(u.id);
                if (it == textures_.end()) continue;
                std::memcpy(static_cast<uint8_t*>(s.staging.map) + s.staging_used, u.data.data(), u.data.size());
                VkBufferImageCopy c{};
                c.bufferOffset = s.staging_used;
                c.bufferRowLength = static_cast<uint32_t>(u.region.w);
                c.bufferImageHeight = static_cast<uint32_t>(u.region.h);
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageOffset = {u.region.x, u.region.y, 0};
                c.imageExtent = {static_cast<uint32_t>(u.region.w), static_cast<uint32_t>(u.region.h), 1};
                dev_.vkCmdCopyBufferToImage(cmd, s.staging.buf, it->second.img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                s.staging_used += (u.data.size() + 15) & ~VkDeviceSize{15};
            }
            uploads_.clear();
        }
        global_barrier(cmd);
    }

    void flush() {
        if (!in_frame_) return;
        Slot& s = slots_[static_cast<size_t>(slot_)];
        VkCommandBuffer cmd = s.cmd;
        record_uploads(cmd);
        if (frames_.draws.empty()) {
            frames_.clear_pending();
            return;
        }
        const VkDeviceSize bytes = frames_.instances.size() * sizeof(gpu::Instance);
        if (s.instances_used + bytes > s.instances.size) {
            if (s.instances.buf) s.garbage.buffers.push_back(s.instances);
            s.instances = Buffer{};
            if (!create_buffer(s.instances, std::max<VkDeviceSize>((s.instances_used + bytes) * 2, VkDeviceSize{1} << 20),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false)) {
                frames_.clear_pending();
                return;
            }
            s.instances_used = 0;
        }
        const VkDeviceSize offset = s.instances_used;
        std::memcpy(static_cast<uint8_t*>(s.instances.map) + offset, frames_.instances.data(), static_cast<size_t>(bytes));
        s.instances_used += (bytes + 255) & ~VkDeviceSize{255};

        global_barrier(cmd);
        VkRenderPassBeginInfo rb{};
        rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rb.renderPass = render_pass_;
        rb.framebuffer = framebuffer_;
        rb.renderArea = {{0, 0}, {target_.w, target_.h}};
        dev_.vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        dev_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        const VkViewport vp{0, 0, static_cast<float>(target_.w), static_cast<float>(target_.h), 0, 1};
        dev_.vkCmdSetViewport(cmd, 0, 1, &vp);
        dev_.vkCmdBindVertexBuffers(cmd, 0, 1, &s.instances.buf, &offset);
        TextureId bound = ~TextureId{0};
        for (const gpu::Draw& d : frames_.draws) {
            if (d.texture != bound) {
                auto it = textures_.find(d.texture);
                if (it == textures_.end()) it = textures_.find(0);
                dev_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &it->second.set, 0, nullptr);
                bound = d.texture;
            }
            const VkRect2D sc{{d.scissor.x, d.scissor.y}, {static_cast<uint32_t>(d.scissor.w), static_cast<uint32_t>(d.scissor.h)}};
            dev_.vkCmdSetScissor(cmd, 0, 1, &sc);
            dev_.vkCmdDraw(cmd, 4, d.count, 0, d.first);
        }
        dev_.vkCmdEndRenderPass(cmd);
        frames_.clear_pending();
    }

    void end_frame_internal() {
        flush();
        in_frame_ = false;
        Slot& s = slots_[static_cast<size_t>(slot_)];
        dev_.vkEndCommandBuffer(s.cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cmd;
        dev_.vkResetFences(device_, 1, &s.fence);
        const VkResult r = dev_.vkQueueSubmit(queue_, 1, &si, s.fence);
        if (r != VK_SUCCESS) {
            (void)fail("vkQueueSubmit", r);
            return;
        }
        s.fence_pending = true;
        last_submitted_ = slot_;
        frame_submitted_ = true;
    }

    // Everything created from the device (not the instance, not the surface).
    void destroy_device_objects() {
        if (!device_) return;
        if (in_frame_) end_frame_internal();
        wait_idle();
        destroy_swapchain();
        destroy_target();
        for (auto& [id, t] : textures_) destroy_image(t.img);
        textures_.clear();
        uploads_.clear();
        next_texture_ = 1;
        for (Slot& s : slots_) {
            destroy_buffer(s.instances);
            destroy_buffer(s.staging);
            if (s.fence) dev_.vkDestroyFence(device_, s.fence, nullptr);
            if (s.present_fence) dev_.vkDestroyFence(device_, s.present_fence, nullptr);
            if (s.acquired) dev_.vkDestroySemaphore(device_, s.acquired, nullptr);
            s = Slot{};
        }
        destroy_buffer(constants_);
        for (VkDescriptorPool p : pools_) dev_.vkDestroyDescriptorPool(device_, p, nullptr);
        pools_.clear();
        if (cmd_pool_) dev_.vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        if (pipeline_) dev_.vkDestroyPipeline(device_, pipeline_, nullptr);
        if (render_pass_) dev_.vkDestroyRenderPass(device_, render_pass_, nullptr);
        if (pipeline_layout_) dev_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (set_layout_) dev_.vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
        render_pass_ = VK_NULL_HANDLE;
        pipeline_layout_ = VK_NULL_HANDLE;
        set_layout_ = VK_NULL_HANDLE;
        dev_.vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        dev_ = DeviceFns{};
        swapchain_ext_ = false;
    }

    void destroy_all() {
        if (device_) detach();
        destroy_device_objects();
        if (messenger_ && in_.vkDestroyDebugUtilsMessengerEXT) in_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        if (instance_) in_.vkDestroyInstance(instance_, nullptr);
    }

    Caps caps_;
    TextGammaParams tg_;
    InstanceFns in_;
    DeviceFns dev_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    bool surface_ext_ = false, swapchain_ext_ = false;
    bool allow_cpu_ = false;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools_;
    Buffer constants_;
    Slot slots_[kSlots];
    int slot_ = 0;
    int last_submitted_ = 0;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat sc_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D sc_extent_{};
    std::vector<VkImage> sc_images_;
    std::vector<VkSemaphore> sc_done_;
    bool swapchain_dirty_ = false;
    bool offscreen_ = false;

    PxSize size_;
    float scale_ = 1.0f;
    Img target_;
    Img scratch_;
    bool scratch_initialized_ = false;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    bool in_frame_ = false;
    bool frame_submitted_ = false;
    bool lost_ = false;
    std::string lost_reason_;
    gpu::FrameBuilder frames_;

    std::unordered_map<TextureId, Texture> textures_;
    std::vector<PendingUpload> uploads_;
    TextureId next_texture_ = 1;
};

}  // namespace

Result<std::unique_ptr<IRenderer>> create(const BackendOptions& opts) {
    auto r = std::make_unique<VulkanRenderer>();
    if (auto ok = r->init(opts); !ok) return Unexpected<Error>(ok.error());
    return std::unique_ptr<IRenderer>(std::move(r));
}

uint32_t validation_messages() { return g_validation_messages.load(); }
bool validation_enabled() { return g_validation_enabled.load(); }

}  // namespace kt::render::vulkan
