// Direct3D 11 backend on the shared GPU pipeline (gpu/gpu_common.h).
//
// d3d11.dll and dxgi.dll are loaded with LoadLibraryExW(LOAD_LIBRARY_SEARCH_SYSTEM32) on first
// use and never linked; COM interface ids come from __uuidof, so no import library either.
// Device: feature level 11_1 .. 10_0 on the first hardware adapter (IDXGIFactory6 minimum-power
// order where the OS has it, 1803+); software adapters only when explicitly allowed.
// Target: a persistent B8G8R8A8_UNORM texture; present copies it into the swapchain back
// buffer. Swapchain: DXGI 1.2 CreateSwapChainForHwnd with FLIP_SEQUENTIAL and the frame
// latency waitable object, falling back to FLIP_SEQUENTIAL without the waitable object and
// then to the DXGI 1.1 CreateSwapChain with DXGI_SWAP_EFFECT_DISCARD (bitblt model), for
// drivers and sessions where the flip model is refused.
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform_gui/native_surface.h"
#include "render/d3d11/d3d11_backend.h"
#include "render/damage.h"
#include "render/gpu/gpu_common.h"
#include "render/shaders/gen/quad_dxbc.h"

namespace kt::render::d3d11 {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int32_t kMaxSide = 16384;

using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);

struct Api {
    PFN_D3D11_CREATE_DEVICE create_device = nullptr;
    PFN_CreateDXGIFactory1 create_factory1 = nullptr;
    std::string error;
};

// The libraries stay loaded for the life of the process.
const Api& api() {
    static const Api a = [] {
        Api r;
        HMODULE d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!d3d || !dxgi) {
            r.error = "d3d11.dll or dxgi.dll is not available";
            return r;
        }
        r.create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(d3d, "D3D11CreateDevice")));
        r.create_factory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
        if (!r.create_device || !r.create_factory1) r.error = "D3D11CreateDevice or CreateDXGIFactory1 is missing";
        return r;
    }();
    return a;
}

std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string hr_text(const char* what, HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " (0x%08lX)", static_cast<unsigned long>(hr));
    return std::string(what) + buf;
}

bool is_software(const DXGI_ADAPTER_DESC1& d) {
    // the software flag, or the Microsoft Basic Render Driver.
    return (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || (d.VendorId == 0x1414 && d.DeviceId == 0x008C);
}

bool device_lost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

DXGI_FORMAT dxgi_format(TexFormat f) {
    switch (f) {
        case TexFormat::a8: return DXGI_FORMAT_R8_UNORM;
        case TexFormat::rgba8_lcd: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TexFormat::bgra8_premul: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    return DXGI_FORMAT_R8_UNORM;
}

struct Texture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    TextureDesc desc;
};

class D3D11Renderer final : public IRenderer {
public:
    ~D3D11Renderer() override { detach(); }

    Result<void> init(const BackendOptions& opts) {
        tg_ = opts.text_gamma;
        const Api& a = api();
        if (!a.error.empty()) return make_error(Errc::unavailable, a.error);
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = a.create_factory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
        if (FAILED(hr)) return make_error(Errc::unavailable, hr_text("CreateDXGIFactory1", hr), hr);

        // Candidate adapters: minimum-power order on 1803+ (a terminal does not need the
        // discrete GPU), else enumeration order (the first adapter owns the primary output).
        std::vector<ComPtr<IDXGIAdapter1>> adapters;
        ComPtr<IDXGIFactory6> f6;
        if (SUCCEEDED(factory.As(&f6))) {
            ComPtr<IDXGIAdapter1> ad;
            for (UINT i = 0; SUCCEEDED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_MINIMUM_POWER, __uuidof(IDXGIAdapter1),
                                                                      reinterpret_cast<void**>(ad.ReleaseAndGetAddressOf())));
                 ++i)
                adapters.push_back(ad);
        } else {
            ComPtr<IDXGIAdapter1> ad;
            for (UINT i = 0; factory->EnumAdapters1(i, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) adapters.push_back(ad);
        }
        std::string reasons;
        // KEEPTERM_D3D11_ADAPTER=warp (tests, together with allow_software_adapter): WARP only.
        char forced[16] = {};
        const DWORD fn = GetEnvironmentVariableA("KEEPTERM_D3D11_ADAPTER", forced, sizeof(forced));
        if (opts.allow_software_adapter && fn > 0 && fn < sizeof(forced) && std::strcmp(forced, "warp") == 0) adapters.clear();
        for (auto& ad : adapters) {
            DXGI_ADAPTER_DESC1 d{};
            if (FAILED(ad->GetDesc1(&d))) continue;
            if (is_software(d) && !opts.allow_software_adapter) {
                reasons += "skipped software adapter " + utf8(d.Description) + "; ";
                continue;
            }
            if (auto ok = create_device(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN); ok) {
                caps_.adapter_name = utf8(d.Description);
                caps_.vendor_id = d.VendorId;
                caps_.device_id = d.DeviceId;
                software_adapter_ = is_software(d);
                break;
            } else {
                reasons += ok.error().what + "; ";
            }
        }
        if (!dev_ && opts.allow_software_adapter) {
            // WARP only when software adapters are explicitly allowed.
            if (auto ok = create_device(nullptr, D3D_DRIVER_TYPE_WARP); ok) {
                caps_.adapter_name = "Microsoft WARP";
                caps_.vendor_id = 0x1414;
                caps_.device_id = 0x008C;
                software_adapter_ = true;
            } else {
                reasons += ok.error().what;
            }
        }
        if (!dev_) return make_error(Errc::unavailable, reasons.empty() ? "no Direct3D 11 adapter" : "no usable adapter: " + reasons);

        // The factory that owns the device's adapter creates its swapchains.
        ComPtr<IDXGIDevice> dxdev;
        ComPtr<IDXGIAdapter> dxad;
        if (FAILED(dev_.As(&dxdev)) || FAILED(dxdev->GetAdapter(&dxad)) ||
            FAILED(dxad->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory_.GetAddressOf()))))
            return make_error(Errc::unavailable, "no DXGI factory for the device");
        factory_.As(&factory2_);
        if (FAILED(dxad.As(&adapter3_))) adapter3_.Reset();

        if (auto ok = create_pipeline(); !ok) return ok;

        caps_.kind = BackendKind::d3d11;
        caps_.max_texture_size = level_ >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
        caps_.dual_source_blend = false;   // LCD text is grayscale until per-channel blending lands
        caps_.partial_present = false;     // set per swapchain (flip model)
        caps_.scroll_blit = true;
        caps_.premultiplied_window_alpha = false;   // DirectComposition swapchains are a later step
        caps_.vsync_control = true;
        caps_.frames_in_flight = 1;
        return {};
    }

    const Caps& caps() const override { return caps_; }

    Result<void> attach(const platform::NativeSurface& s, PxSize size, float scale) override {
        if (s.ws != platform::WindowSystem::win32 || !s.handle) return make_error(Errc::invalid_argument, "D3D11 needs a Win32 HWND");
        detach();
        hwnd_ = static_cast<HWND>(s.handle);
        size_ = clamp_size(size);
        scale_ = scale;
        if (auto ok = create_swapchain(); !ok) {
            hwnd_ = nullptr;
            return ok;
        }
        return create_target();
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
        if (size == size_ && target_) return;
        size_ = size;
        if (swapchain_ && size.w > 0 && size.h > 0) {
            ctx_->OMSetRenderTargets(0, nullptr, nullptr);
            ctx_->Flush();
            const HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(size.w), static_cast<UINT>(size.h), DXGI_FORMAT_UNKNOWN, sc_flags_);
            if (device_lost(hr)) lost_ = hr;
        }
        (void)create_target();
    }

    void detach() override {
        if (in_frame_) end_frame_internal();
        if (ctx_) {
            ctx_->OMSetRenderTargets(0, nullptr, nullptr);
            ctx_->ClearState();
            ctx_->Flush();
        }
        if (waitable_) CloseHandle(waitable_);
        waitable_ = nullptr;
        swapchain_.Reset();
        swapchain1_.Reset();
        target_.Reset();
        rtv_.Reset();
        scratch_.Reset();
        hwnd_ = nullptr;
        offscreen_ = false;
        flip_ = false;
        caps_.partial_present = false;
        size_ = PxSize{};
    }

    Result<TextureId> create_texture(const TextureDesc& desc) override {
        if (desc.w == 0 || desc.h == 0) return make_error(Errc::invalid_argument, "empty texture");
        if (desc.w > caps_.max_texture_size || desc.h > caps_.max_texture_size) return make_error(Errc::invalid_argument, "texture too large");
        D3D11_TEXTURE2D_DESC td{};
        td.Width = desc.w;
        td.Height = desc.h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgi_format(desc.format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const UINT pitch = UINT{desc.w} * gpu::texel_bytes(desc.format);
        std::vector<uint8_t> zeros(static_cast<size_t>(pitch) * desc.h, 0);   // textures start as zeros
        D3D11_SUBRESOURCE_DATA init{zeros.data(), pitch, 0};
        Texture t;
        t.desc = desc;
        HRESULT hr = dev_->CreateTexture2D(&td, &init, &t.tex);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
        if (FAILED(hr)) return make_error(device_lost(hr) ? Errc::lost : Errc::unavailable, hr_text("CreateTexture2D", hr), hr);
        const TextureId id = next_texture_++;
        textures_.emplace(id, std::move(t));
        return id;
    }

    void update_texture(TextureId id, IRect region, const void* pixels, uint32_t stride) override {
        auto it = textures_.find(id);
        if (it == textures_.end()) return;
        const Texture& t = it->second;
        auto src = static_cast<const uint8_t*>(pixels);
        if (!gpu::clip_upload(region, src, stride, t.desc.w, t.desc.h, gpu::texel_bytes(t.desc.format))) return;
        const D3D11_BOX box{static_cast<UINT>(region.x), static_cast<UINT>(region.y), 0, static_cast<UINT>(region.right()),
                            static_cast<UINT>(region.bottom()), 1};
        ctx_->UpdateSubresource(t.tex.Get(), 0, &box, src, stride, 0);
    }

    void destroy_texture(TextureId id) override { textures_.erase(id); }

    Result<bool> begin_frame(const Damage& damage) override {
        if (auto l = check_lost(); !l) return Unexpected<Error>(l.error());
        if (!target_ || (!swapchain_ && !offscreen_)) return false;
        if (waitable_) WaitForSingleObjectEx(waitable_, 1000, TRUE);   // frame latency 1
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
        return check_lost();
    }

    Result<void> present() override {
        if (auto l = check_lost(); !l) return l;
        if (!swapchain_) {
            ctx_->Flush();
            return {};
        }
        ComPtr<ID3D11Texture2D> back;
        HRESULT hr = swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.GetAddressOf()));
        if (FAILED(hr)) return make_error(device_lost(hr) ? Errc::lost : Errc::unavailable, hr_text("GetBuffer", hr), hr);
        D3D11_TEXTURE2D_DESC bd{};
        back->GetDesc(&bd);
        if (bd.Width == static_cast<UINT>(size_.w) && bd.Height == static_cast<UINT>(size_.h)) {
            ctx_->CopyResource(back.Get(), target_.Get());
        } else {
            const D3D11_BOX box{0, 0, 0, std::min<UINT>(bd.Width, static_cast<UINT>(size_.w)),
                                std::min<UINT>(bd.Height, static_cast<UINT>(size_.h)), 1};
            ctx_->CopySubresourceRegion(back.Get(), 0, 0, 0, 0, target_.Get(), 0, &box);
        }
        back.Reset();
        if (flip_ && swapchain1_ && !full_present_) {
            // Dirty rectangles reach DWM: the back buffer equals the target everywhere.
            dirty_.clear();
            for (const IRect& r : frames_.present_damage()) dirty_.push_back(RECT{r.x, r.y, r.right(), r.bottom()});
            DXGI_PRESENT_PARAMETERS pp{};
            if (!dirty_.empty() && dirty_.size() <= kMaxDamageRects * 2) {
                pp.DirtyRectsCount = static_cast<UINT>(dirty_.size());
                pp.pDirtyRects = dirty_.data();
            }
            hr = swapchain1_->Present1(1, 0, &pp);
        } else {
            hr = swapchain_->Present(1, 0);
        }
        full_present_ = false;
        if (device_lost(hr)) {
            lost_ = hr;
            return check_lost();
        }
        if (FAILED(hr)) return make_error(Errc::unavailable, hr_text("Present", hr), hr);
        return {};
    }

    void copy_rect(IRect src, int32_t dst_x, int32_t dst_y) override {
        if (!in_frame_ || !target_) return;
        IRect sr, dr;
        if (!clip_copy(src, dst_x, dst_y, size_, sr, dr)) return;
        flush();
        if (!scratch_) {
            D3D11_TEXTURE2D_DESC td{};
            target_->GetDesc(&td);
            td.BindFlags = 0;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &scratch_))) return;
        }
        // Overlapping copies inside one resource are undefined: go through the scratch texture.
        const D3D11_BOX box{static_cast<UINT>(sr.x), static_cast<UINT>(sr.y), 0, static_cast<UINT>(sr.right()),
                            static_cast<UINT>(sr.bottom()), 1};
        ctx_->CopySubresourceRegion(scratch_.Get(), 0, static_cast<UINT>(sr.x), static_cast<UINT>(sr.y), 0, target_.Get(), 0, &box);
        ctx_->CopySubresourceRegion(target_.Get(), 0, static_cast<UINT>(dr.x), static_cast<UINT>(dr.y), 0, scratch_.Get(), 0, &box);
        frames_.add_copy_damage(dr);
    }

    Result<Image> read_back(IRect rect) override {
        if (!target_) return make_error(Errc::unavailable, "no frame");
        if (in_frame_) end_frame_internal();
        const IRect r = intersect(rect, IRect{0, 0, size_.w, size_.h});
        Image img;
        img.order = PixelOrder::bgra;
        if (r.empty()) return img;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(r.w);
        td.Height = static_cast<UINT>(r.h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &staging);
        if (FAILED(hr)) return make_error(device_lost(hr) ? Errc::lost : Errc::unavailable, hr_text("CreateTexture2D (staging)", hr), hr);
        const D3D11_BOX box{static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0, static_cast<UINT>(r.right()), static_cast<UINT>(r.bottom()), 1};
        ctx_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, target_.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr = ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) return make_error(device_lost(hr) ? Errc::lost : Errc::unavailable, hr_text("Map (read-back)", hr), hr);
        img.w = r.w;
        img.h = r.h;
        img.pixels.resize(static_cast<size_t>(r.w) * static_cast<size_t>(r.h));
        for (int32_t y = 0; y < r.h; ++y)
            std::memcpy(&img.pixels[static_cast<size_t>(y) * static_cast<size_t>(r.w)], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                        static_cast<size_t>(r.w) * 4);
        ctx_->Unmap(staging.Get(), 0);
        return img;
    }

private:
    static PxSize clamp_size(PxSize s) { return PxSize{std::clamp(s.w, 0, kMaxSide), std::clamp(s.h, 0, kMaxSide)}; }

    Result<void> create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type) {
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                   D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = E_FAIL;
#ifndef NDEBUG
        // The debug layer when the SDK layers are installed; silently without them.
        hr = api().create_device(adapter, type, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG, levels, 4, D3D11_SDK_VERSION, &dev_, &level_, &ctx_);
#endif
        if (FAILED(hr)) hr = api().create_device(adapter, type, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &dev_, &level_, &ctx_);
        if (hr == E_INVALIDARG)   // runtimes without 11_1
            hr = api().create_device(adapter, type, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, &dev_, &level_, &ctx_);
        if (FAILED(hr)) {
            dev_.Reset();
            ctx_.Reset();
            return make_error(Errc::unavailable, hr_text("D3D11CreateDevice", hr), hr);
        }
        return {};
    }

    Result<void> create_pipeline() {
        HRESULT hr = dev_->CreateVertexShader(shaders::kQuadVsDxbc, sizeof(shaders::kQuadVsDxbc), nullptr, &vs_);
        if (SUCCEEDED(hr)) hr = dev_->CreatePixelShader(shaders::kQuadPsDxbc, sizeof(shaders::kQuadPsDxbc), nullptr, &ps_);
        if (FAILED(hr)) return make_error(Errc::unavailable, hr_text("CreateVertex/PixelShader", hr), hr);
        const D3D11_INPUT_ELEMENT_DESC il[gpu::kInstanceAttributes] = {
            {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"UV", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"CLIP", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLORS", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"PARAMS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        hr = dev_->CreateInputLayout(il, gpu::kInstanceAttributes, shaders::kQuadVsDxbc, sizeof(shaders::kQuadVsDxbc), &layout_);
        if (FAILED(hr)) return make_error(Errc::unavailable, hr_text("CreateInputLayout", hr), hr);

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = dev_->CreateBlendState(&bd, &blend_);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        if (SUCCEEDED(hr)) hr = dev_->CreateRasterizerState(&rd, &raster_);
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(gpu::FrameConstants);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(hr)) hr = dev_->CreateBuffer(&cb, nullptr, &constants_);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint8_t zero[4] = {};
        D3D11_SUBRESOURCE_DATA init{zero, 4, 0};
        ComPtr<ID3D11Texture2D> dummy;
        if (SUCCEEDED(hr)) hr = dev_->CreateTexture2D(&td, &init, &dummy);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(dummy.Get(), nullptr, &dummy_srv_);
        if (FAILED(hr)) return make_error(Errc::unavailable, hr_text("pipeline state", hr), hr);
        return {};
    }

    Result<void> create_swapchain() {
        const UINT w = static_cast<UINT>(std::max(size_.w, 1)), h = static_cast<UINT>(std::max(size_.h, 1));
        HRESULT hr = E_FAIL;
        if (factory2_) {
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Width = w;
            d.Height = h;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 2;
            d.Scaling = DXGI_SCALING_NONE;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;   // keeps the back buffer: dirty-rect presents are valid
            d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            for (UINT flags : {static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), 0u}) {
                for (DXGI_SCALING scaling : {DXGI_SCALING_NONE, DXGI_SCALING_STRETCH}) {
                    d.Flags = flags;
                    d.Scaling = scaling;
                    hr = factory2_->CreateSwapChainForHwnd(dev_.Get(), hwnd_, &d, nullptr, nullptr, swapchain1_.ReleaseAndGetAddressOf());
                    if (SUCCEEDED(hr)) {
                        sc_flags_ = flags;
                        break;
                    }
                }
                if (SUCCEEDED(hr)) break;
            }
            if (SUCCEEDED(hr)) {
                swapchain1_.As(&swapchain_);
                flip_ = true;
            }
        }
        if (FAILED(hr)) {
            // DXGI 1.1 bitblt model: works where the flip model is refused.
            swapchain1_.Reset();
            DXGI_SWAP_CHAIN_DESC d{};
            d.BufferDesc.Width = w;
            d.BufferDesc.Height = h;
            d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 1;
            d.OutputWindow = hwnd_;
            d.Windowed = TRUE;
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            sc_flags_ = 0;
            hr = factory_->CreateSwapChain(dev_.Get(), &d, swapchain_.ReleaseAndGetAddressOf());
            if (FAILED(hr)) return make_error(device_lost(hr) ? Errc::lost : Errc::unavailable, hr_text("CreateSwapChain", hr), hr);
            flip_ = false;
        }
        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        ComPtr<IDXGISwapChain2> sc2;
        if ((sc_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) && SUCCEEDED(swapchain_.As(&sc2))) {
            sc2->SetMaximumFrameLatency(1);
            waitable_ = sc2->GetFrameLatencyWaitableObject();
        } else {
            ComPtr<IDXGIDevice1> d1;
            if (SUCCEEDED(dev_.As(&d1))) d1->SetMaximumFrameLatency(1);
        }
        caps_.partial_present = flip_;
        full_present_ = true;
        return {};
    }

    Result<void> create_target() {
        target_.Reset();
        rtv_.Reset();
        scratch_.Reset();
        if (size_.w == 0 || size_.h == 0) return {};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(size_.w);
        td.Height = static_cast<UINT>(size_.h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &target_);
        if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(target_.Get(), nullptr, &rtv_);
        if (FAILED(hr)) {
            target_.Reset();
            return make_error(device_lost(hr) ? Errc::lost : Errc::unavailable, hr_text("render target", hr), hr);
        }
        const float zero[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(rtv_.Get(), zero);
        const gpu::FrameConstants c = gpu::make_constants(size_, -1.0f, tg_);
        ctx_->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
        full_present_ = true;
        return {};
    }

    Result<void> check_lost() {
        if (lost_ == S_OK && dev_) {
            const HRESULT hr = dev_->GetDeviceRemovedReason();
            if (hr != S_OK) lost_ = hr;
        }
        if (lost_ != S_OK) return make_error(Errc::lost, hr_text("Direct3D 11 device removed", lost_), lost_);
        return {};
    }

    void end_frame_internal() {
        flush();
        in_frame_ = false;
    }

    void flush() {
        if (frames_.draws.empty() || !target_) {
            frames_.clear_pending();
            return;
        }
        const size_t bytes = frames_.instances.size() * sizeof(gpu::Instance);
        if (bytes > instance_capacity_) {
            instance_buffer_.Reset();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = static_cast<UINT>(std::max<size_t>(bytes * 2, size_t{1} << 20));
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            const HRESULT hr = dev_->CreateBuffer(&bd, nullptr, &instance_buffer_);
            if (FAILED(hr)) {
                if (device_lost(hr)) lost_ = hr;
                instance_capacity_ = 0;
                frames_.clear_pending();
                return;
            }
            instance_capacity_ = bd.ByteWidth;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx_->Map(instance_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (FAILED(hr)) {
            if (device_lost(hr)) lost_ = hr;
            frames_.clear_pending();
            return;
        }
        std::memcpy(m.pData, frames_.instances.data(), bytes);
        ctx_->Unmap(instance_buffer_.Get(), 0);

        ID3D11RenderTargetView* rtv = rtv_.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(size_.w), static_cast<float>(size_.h), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(raster_.Get());
        ctx_->IASetInputLayout(layout_.Get());
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11Buffer* vb = instance_buffer_.Get();
        const UINT stride = gpu::kInstanceStride, offset = 0;
        ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ID3D11Buffer* cb = constants_.Get();
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->VSSetConstantBuffers(0, 1, &cb);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, &cb);
        TextureId bound = ~TextureId{0};
        for (const gpu::Draw& d : frames_.draws) {
            if (d.texture != bound) {
                auto it = textures_.find(d.texture);
                ID3D11ShaderResourceView* srv = it != textures_.end() ? it->second.srv.Get() : dummy_srv_.Get();
                ctx_->PSSetShaderResources(0, 1, &srv);
                bound = d.texture;
            }
            const D3D11_RECT sc{d.scissor.x, d.scissor.y, d.scissor.right(), d.scissor.bottom()};
            ctx_->RSSetScissorRects(1, &sc);
            ctx_->DrawInstanced(4, d.count, 0, d.first);
        }
        ID3D11ShaderResourceView* none = nullptr;
        ctx_->PSSetShaderResources(0, 1, &none);
        frames_.clear_pending();
    }

    Caps caps_;
    TextGammaParams tg_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    D3D_FEATURE_LEVEL level_ = D3D_FEATURE_LEVEL_10_0;
    bool software_adapter_ = false;
    ComPtr<IDXGIFactory1> factory_;
    ComPtr<IDXGIFactory2> factory2_;
    ComPtr<IDXGIAdapter3> adapter3_;   // QueryVideoMemoryInfo for the resource monitor, later
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11ShaderResourceView> dummy_srv_;
    ComPtr<ID3D11Buffer> instance_buffer_;
    size_t instance_capacity_ = 0;

    HWND hwnd_ = nullptr;
    bool offscreen_ = false;
    ComPtr<IDXGISwapChain> swapchain_;
    ComPtr<IDXGISwapChain1> swapchain1_;
    UINT sc_flags_ = 0;
    bool flip_ = false;
    bool full_present_ = true;
    HANDLE waitable_ = nullptr;
    std::vector<RECT> dirty_;

    PxSize size_;
    float scale_ = 1.0f;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> scratch_;
    HRESULT lost_ = S_OK;
    bool in_frame_ = false;
    gpu::FrameBuilder frames_;

    std::unordered_map<TextureId, Texture> textures_;
    TextureId next_texture_ = 1;
};

}  // namespace

std::string adapter_key() {
    const Api& a = api();
    if (!a.error.empty()) return {};
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(a.create_factory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) return {};
    std::string key;
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; factory->EnumAdapters1(i, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(ad->GetDesc1(&d))) continue;
        LARGE_INTEGER umd{};
        if (FAILED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) umd.QuadPart = 0;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "dxgi:%u:%u:%llx%s;", d.VendorId, d.DeviceId, static_cast<unsigned long long>(umd.QuadPart),
                      is_software(d) ? ":sw" : "");
        key += buf;
    }
    return key;
}

Result<std::unique_ptr<IRenderer>> create(const BackendOptions& opts) {
    auto r = std::make_unique<D3D11Renderer>();
    if (auto ok = r->init(opts); !ok) return Unexpected<Error>(ok.error());
    return std::unique_ptr<IRenderer>(std::move(r));
}

}  // namespace kt::render::d3d11
