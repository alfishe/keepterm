// Metal backend on the shared GPU pipeline (gpu/gpu_common.h).
//
// Device: MTLCreateSystemDefaultDevice. Metal.framework is linked weakly, so a missing
// framework or device makes create() return Errc::unavailable instead of failing at load.
// Target: a persistent private BGRA8Unorm texture; present blits it into the drawable of a
// CAMetalLayer (framebufferOnly = NO, maximumDrawableCount = 2, displaySyncEnabled = YES).
// The layer is the surface layer when the platform created a CAMetalLayer, otherwise a
// CAMetalLayer sublayer of the surface CALayer that this backend owns.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "platform_gui/native_surface.h"
#include "render/damage.h"
#include "render/gpu/gpu_common.h"
#include "render/metal/metal_backend.h"
#include "render/shaders/gen/quad_metallib.h"

namespace kt::render::metal {

namespace {

constexpr int kFramesInFlight = 2;
constexpr int kInstanceSlots = 3;
constexpr int32_t kMaxSide = 16384;

MTLPixelFormat pixel_format(TexFormat f) {
    switch (f) {
        case TexFormat::a8: return MTLPixelFormatR8Unorm;
        case TexFormat::rgba8_lcd: return MTLPixelFormatRGBA8Unorm;
        case TexFormat::bgra8_premul: return MTLPixelFormatBGRA8Unorm;
    }
    return MTLPixelFormatR8Unorm;
}

struct Texture {
    id<MTLTexture> tex;
    TextureDesc desc;
};

class MetalRenderer final : public IRenderer {
public:
    MetalRenderer() = default;
    ~MetalRenderer() override {
        detach();
        wait_idle();
    }

    Result<void> init(const BackendOptions& opts) {
        tg_ = opts.text_gamma;
        if (&MTLCreateSystemDefaultDevice == nullptr) return make_error(Errc::unavailable, "Metal.framework is not available");
        dev_ = MTLCreateSystemDefaultDevice();
        if (!dev_) return make_error(Errc::unavailable, "no Metal device");
        queue_ = [dev_ newCommandQueue];
        if (!queue_) return make_error(Errc::unavailable, "newCommandQueue failed");

        dispatch_data_t data = dispatch_data_create(shaders::kQuadMetallib, sizeof(shaders::kQuadMetallib), nullptr,
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* err = nil;
        id<MTLLibrary> lib = [dev_ newLibraryWithData:data error:&err];
        if (!lib) return make_error(Errc::unavailable, std::string("newLibraryWithData: ") + describe(err));
        id<MTLFunction> vs = [lib newFunctionWithName:@"vs_main"];
        id<MTLFunction> ps = [lib newFunctionWithName:@"ps_main"];
        if (!vs || !ps) return make_error(Errc::unavailable, "shader entry points missing in the metallib");

        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        const MTLVertexFormat formats[gpu::kInstanceAttributes] = {MTLVertexFormatFloat4, MTLVertexFormatFloat4,
                                                                   MTLVertexFormatFloat4, MTLVertexFormatUInt4,
                                                                   MTLVertexFormatFloat4};
        for (NSUInteger a = 0; a < gpu::kInstanceAttributes; ++a) {
            vd.attributes[a].format = formats[a];
            vd.attributes[a].offset = a * 16;
            vd.attributes[a].bufferIndex = 1;
        }
        vd.layouts[1].stride = gpu::kInstanceStride;
        vd.layouts[1].stepFunction = MTLVertexStepFunctionPerInstance;
        vd.layouts[1].stepRate = 1;

        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.label = @"keepterm quad";
        pd.vertexFunction = vs;
        pd.fragmentFunction = ps;
        pd.vertexDescriptor = vd;
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
        ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorOne;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pso_ = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso_) return make_error(Errc::unavailable, std::string("pipeline: ") + describe(err));

        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:1
                                                                                     height:1
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        dummy_ = [dev_ newTextureWithDescriptor:td];
        inflight_ = dispatch_semaphore_create(kFramesInFlight);

        caps_.kind = BackendKind::metal;
        caps_.adapter_name = dev_.name ? std::string(dev_.name.UTF8String) : "Metal device";
        caps_.vendor_id = 0x106B;   // Apple; Metal reports no PCI ids on Apple silicon
        caps_.device_id = static_cast<uint32_t>(dev_.registryID & 0xFFFFFFFFu);
        caps_.max_texture_size = 16384;
        caps_.dual_source_blend = false;   // LCD text is grayscale; off on macOS anyway
        caps_.partial_present = false;
        caps_.scroll_blit = true;
        caps_.premultiplied_window_alpha = true;
        caps_.vsync_control = true;
        caps_.frames_in_flight = kFramesInFlight;
        return {};
    }

    const Caps& caps() const override { return caps_; }

    Result<void> attach(const platform::NativeSurface& s, PxSize size, float scale) override {
        if (s.ws != platform::WindowSystem::cocoa || !s.layer)
            return make_error(Errc::invalid_argument, "Metal needs a Cocoa surface layer");
        detach();
        CALayer* parent = (__bridge CALayer*)s.layer;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        if ([parent isKindOfClass:[CAMetalLayer class]]) {
            layer_ = static_cast<CAMetalLayer*>(parent);
            own_layer_ = false;
        } else {
            layer_ = [CAMetalLayer layer];
            own_layer_ = true;
            layer_.anchorPoint = CGPointZero;
            layer_.frame = parent.bounds;
            layer_.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
            [parent addSublayer:layer_];
        }
        layer_.device = dev_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer_.framebufferOnly = NO;   // the drawable is a blit destination
        layer_.maximumDrawableCount = 2;
        layer_.displaySyncEnabled = YES;
        layer_.opaque = YES;
        layer_.contentsGravity = kCAGravityTopLeft;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer_.colorspace = cs;
        CGColorSpaceRelease(cs);
        [CATransaction commit];
        resize(size, scale);
        return {};
    }

    Result<void> attach_offscreen(PxSize size, float scale) override {
        detach();
        offscreen_ = true;
        resize(size, scale);
        return {};
    }

    void resize(PxSize size, float scale) override {
        scale_ = scale > 0 ? scale : 1.0f;
        size.w = std::clamp(size.w, 0, kMaxSide);
        size.h = std::clamp(size.h, 0, kMaxSide);
        if (layer_) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            layer_.contentsScale = scale_;
            if (own_layer_) layer_.frame = layer_.superlayer ? layer_.superlayer.bounds : layer_.frame;
            if (size.w > 0 && size.h > 0) layer_.drawableSize = CGSizeMake(size.w, size.h);
            [CATransaction commit];
        }
        if (size == size_ && target_) return;
        size_ = size;
        target_ = nil;
        scratch_ = nil;
        if (size.w == 0 || size.h == 0) return;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:static_cast<NSUInteger>(size.w)
                                                                                     height:static_cast<NSUInteger>(size.h)
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        target_ = [dev_ newTextureWithDescriptor:td];
        target_fresh_ = true;   // cleared to transparent black by the first render pass
    }

    void detach() override {
        if (in_frame_) end_frame_internal();
        if (cmd_) commit();
        if (layer_ && own_layer_) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [layer_ removeFromSuperlayer];
            [CATransaction commit];
        }
        layer_ = nil;
        own_layer_ = false;
        offscreen_ = false;
        target_ = nil;
        scratch_ = nil;
        size_ = PxSize{};
    }

    Result<TextureId> create_texture(const TextureDesc& desc) override {
        if (desc.w == 0 || desc.h == 0) return make_error(Errc::invalid_argument, "empty texture");
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format(desc.format)
                                                                                      width:desc.w
                                                                                     height:desc.h
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev_ newTextureWithDescriptor:td];
        if (!t) return make_error(Errc::unavailable, "newTextureWithDescriptor failed");
        // Textures start as zeros, like the software backend's.
        const NSUInteger bpr = NSUInteger{desc.w} * gpu::texel_bytes(desc.format);
        id<MTLBuffer> zeros = [dev_ newBufferWithLength:bpr * desc.h options:MTLResourceStorageModeShared];
        std::memset(zeros.contents, 0, bpr * desc.h);
        [upload_encoder() copyFromBuffer:zeros
                            sourceOffset:0
                       sourceBytesPerRow:bpr
                     sourceBytesPerImage:bpr * desc.h
                              sourceSize:MTLSizeMake(desc.w, desc.h, 1)
                               toTexture:t
                        destinationSlice:0
                        destinationLevel:0
                       destinationOrigin:MTLOriginMake(0, 0, 0)];
        const TextureId id = next_texture_++;
        textures_.emplace(id, Texture{t, desc});
        return id;
    }

    void update_texture(TextureId tex_id, IRect region, const void* pixels, uint32_t stride) override {
        auto it = textures_.find(tex_id);
        if (it == textures_.end()) return;
        const Texture& t = it->second;
        const uint32_t bpp = gpu::texel_bytes(t.desc.format);
        auto src = static_cast<const uint8_t*>(pixels);
        if (!gpu::clip_upload(region, src, stride, t.desc.w, t.desc.h, bpp)) return;
        const size_t row = static_cast<size_t>(region.w) * bpp;
        id<MTLBuffer> staging = [dev_ newBufferWithLength:row * static_cast<size_t>(region.h)
                                                  options:MTLResourceStorageModeShared];
        if (!staging) return;
        auto* dst = static_cast<uint8_t*>(staging.contents);
        for (int32_t y = 0; y < region.h; ++y) std::memcpy(dst + row * static_cast<size_t>(y), src + static_cast<size_t>(y) * stride, row);
        [upload_encoder() copyFromBuffer:staging
                            sourceOffset:0
                       sourceBytesPerRow:row
                     sourceBytesPerImage:row * static_cast<size_t>(region.h)
                              sourceSize:MTLSizeMake(static_cast<NSUInteger>(region.w), static_cast<NSUInteger>(region.h), 1)
                               toTexture:t.tex
                        destinationSlice:0
                        destinationLevel:0
                       destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(region.x), static_cast<NSUInteger>(region.y), 0)];
    }

    void destroy_texture(TextureId id) override { textures_.erase(id); }   // in-flight command buffers retain it

    Result<bool> begin_frame(const Damage& damage) override {
        if (lost_.load()) return make_error(Errc::lost, "Metal command buffer failed (device lost)");
        if (!target_ || (!layer_ && !offscreen_)) return false;
        frames_.begin(size_, damage);
        in_frame_ = true;
        ensure_cmd();
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
        if (lost_.load()) return make_error(Errc::lost, "Metal command buffer failed (device lost)");
        return {};
    }

    Result<void> present() override {
        if (lost_.load()) return make_error(Errc::lost, "Metal command buffer failed (device lost)");
        if (!cmd_) return {};
        if (layer_ && target_) {
            end_upload_encoder();
            id<CAMetalDrawable> drawable = [layer_ nextDrawable];   // as late as possible
            if (drawable) {
                id<MTLTexture> dt = drawable.texture;
                const NSUInteger w = std::min<NSUInteger>(dt.width, target_.width);
                const NSUInteger h = std::min<NSUInteger>(dt.height, target_.height);
                id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
                [blit copyFromTexture:target_
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(w, h, 1)
                            toTexture:dt
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
                [cmd_ presentDrawable:drawable];
            }
        }
        commit();
        return {};
    }

    void copy_rect(IRect src, int32_t dst_x, int32_t dst_y) override {
        if (!in_frame_ || !target_) return;
        IRect sr, dr;
        if (!clip_copy(src, dst_x, dst_y, size_, sr, dr)) return;
        flush();   // the copy sees everything submitted before it
        if (!scratch_) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:target_.width
                                                                                         height:target_.height
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            scratch_ = [dev_ newTextureWithDescriptor:td];
        }
        end_upload_encoder();
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        const MTLSize sz = MTLSizeMake(static_cast<NSUInteger>(sr.w), static_cast<NSUInteger>(sr.h), 1);
        [blit copyFromTexture:target_ sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(sr.x), static_cast<NSUInteger>(sr.y), 0)
                   sourceSize:sz toTexture:scratch_ destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(sr.x), static_cast<NSUInteger>(sr.y), 0)];
        [blit copyFromTexture:scratch_ sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(sr.x), static_cast<NSUInteger>(sr.y), 0)
                   sourceSize:sz toTexture:target_ destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(dr.x), static_cast<NSUInteger>(dr.y), 0)];
        [blit endEncoding];
        frames_.add_copy_damage(dr);
    }

    Result<Image> read_back(IRect rect) override {
        if (!target_) return make_error(Errc::unavailable, "no frame");
        if (in_frame_) end_frame_internal();
        const IRect r = intersect(rect, IRect{0, 0, size_.w, size_.h});
        Image img;
        img.order = PixelOrder::bgra;
        if (r.empty()) return img;
        ensure_cmd();
        end_upload_encoder();
        const NSUInteger bpr = static_cast<NSUInteger>(r.w) * 4;
        id<MTLBuffer> buf = [dev_ newBufferWithLength:bpr * static_cast<NSUInteger>(r.h) options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        [blit copyFromTexture:target_
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(r.x), static_cast<NSUInteger>(r.y), 0)
                          sourceSize:MTLSizeMake(static_cast<NSUInteger>(r.w), static_cast<NSUInteger>(r.h), 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:bpr
            destinationBytesPerImage:bpr * static_cast<NSUInteger>(r.h)];
        [blit endEncoding];
        id<MTLCommandBuffer> cb = cmd_;
        commit();
        [cb waitUntilCompleted];
        if (cb.status == MTLCommandBufferStatusError) return make_error(Errc::lost, "Metal read-back failed");
        img.w = r.w;
        img.h = r.h;
        img.pixels.resize(static_cast<size_t>(r.w) * static_cast<size_t>(r.h));
        std::memcpy(img.pixels.data(), buf.contents, img.pixels.size() * 4);
        return img;
    }

private:
    static std::string describe(NSError* e) { return e ? std::string(e.localizedDescription.UTF8String) : "unknown error"; }

    void ensure_cmd() {
        if (cmd_) return;
        dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER);   // kFramesInFlight command buffers at most
        cmd_ = [queue_ commandBuffer];
        cmd_.label = @"keepterm frame";
        dispatch_semaphore_t sem = inflight_;
        std::atomic<bool>* lost = &lost_;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
          if (cb.status == MTLCommandBufferStatusError) lost->store(true);
          dispatch_semaphore_signal(sem);
        }];
        slot_ = (slot_ + 1) % kInstanceSlots;
        slot_used_ = 0;
    }

    id<MTLBlitCommandEncoder> upload_encoder() {
        ensure_cmd();
        if (!upload_) upload_ = [cmd_ blitCommandEncoder];
        return upload_;
    }

    void end_upload_encoder() {
        if (upload_) {
            [upload_ endEncoding];
            upload_ = nil;
        }
    }

    void commit() {
        end_upload_encoder();
        [cmd_ commit];
        cmd_ = nil;
    }

    void wait_idle() {
        // Every command buffer signals the semaphore once; take all slots to wait for them.
        if (!inflight_) return;
        for (int i = 0; i < kFramesInFlight; ++i) dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER);
        for (int i = 0; i < kFramesInFlight; ++i) dispatch_semaphore_signal(inflight_);
    }

    void end_frame_internal() {
        flush();
        in_frame_ = false;
    }

    // Encodes the pending draws into one render pass on the target.
    void flush() {
        if (!cmd_ || !target_) return;
        const bool clear = target_fresh_;
        if (frames_.draws.empty() && !clear) {
            frames_.clear_pending();
            return;
        }
        end_upload_encoder();
        const size_t bytes = frames_.instances.size() * sizeof(gpu::Instance);
        __strong id<MTLBuffer>& ring = instance_ring_[slot_];
        if (bytes > 0 && (!ring || ring.length < slot_used_ + bytes)) {
            // Grow: the old buffer stays alive while a command buffer references it.
            const NSUInteger want = std::max<NSUInteger>(NSUInteger{1} << 20, (slot_used_ + bytes) * 2);
            ring = [dev_ newBufferWithLength:want options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            slot_used_ = 0;
        }
        const NSUInteger offset = slot_used_;
        if (bytes > 0) {
            std::memcpy(static_cast<uint8_t*>(ring.contents) + offset, frames_.instances.data(), bytes);
            slot_used_ += (bytes + 255) & ~size_t{255};
        }

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target_;
        rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        target_fresh_ = false;
        id<MTLRenderCommandEncoder> enc = [cmd_ renderCommandEncoderWithDescriptor:rp];
        if (!frames_.draws.empty()) {
            const gpu::FrameConstants c = gpu::make_constants(size_, -1.0f, tg_);
            [enc setRenderPipelineState:pso_];
            [enc setVertexBuffer:ring offset:offset atIndex:1];
            [enc setVertexBytes:&c length:sizeof(c) atIndex:0];
            [enc setFragmentBytes:&c length:sizeof(c) atIndex:0];
            TextureId bound = ~TextureId{0};
            for (const gpu::Draw& d : frames_.draws) {
                if (d.texture != bound) {
                    auto it = textures_.find(d.texture);
                    [enc setFragmentTexture:(it != textures_.end() ? it->second.tex : dummy_) atIndex:0];
                    bound = d.texture;
                }
                [enc setScissorRect:MTLScissorRect{static_cast<NSUInteger>(d.scissor.x), static_cast<NSUInteger>(d.scissor.y),
                                                   static_cast<NSUInteger>(d.scissor.w), static_cast<NSUInteger>(d.scissor.h)}];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:d.count baseInstance:d.first];
            }
        }
        [enc endEncoding];
        frames_.clear_pending();
    }

    Caps caps_;
    TextGammaParams tg_;
    id<MTLDevice> dev_;
    id<MTLCommandQueue> queue_;
    id<MTLRenderPipelineState> pso_;
    id<MTLTexture> dummy_;
    dispatch_semaphore_t inflight_ = nil;
    std::atomic<bool> lost_{false};

    CAMetalLayer* layer_ = nil;
    bool own_layer_ = false;
    bool offscreen_ = false;
    PxSize size_;
    float scale_ = 1.0f;
    id<MTLTexture> target_;
    id<MTLTexture> scratch_;
    bool target_fresh_ = false;

    id<MTLCommandBuffer> cmd_;
    id<MTLBlitCommandEncoder> upload_;
    id<MTLBuffer> instance_ring_[kInstanceSlots];
    int slot_ = 0;
    NSUInteger slot_used_ = 0;
    bool in_frame_ = false;
    gpu::FrameBuilder frames_;

    std::unordered_map<TextureId, Texture> textures_;
    TextureId next_texture_ = 1;
};

}  // namespace

Result<std::unique_ptr<IRenderer>> create(const BackendOptions& opts) {
    @autoreleasepool {
        auto r = std::make_unique<MetalRenderer>();
        if (auto ok = r->init(opts); !ok) return Unexpected<Error>(ok.error());
        return std::unique_ptr<IRenderer>(std::move(r));
    }
}

}  // namespace kt::render::metal
