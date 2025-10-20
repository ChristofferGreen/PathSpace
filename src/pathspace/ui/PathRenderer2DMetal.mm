#if defined(__APPLE__) && PATHSPACE_UI_METAL

#import <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>

#include <vector>
#include <unordered_map>

#include <pathspace/ui/PathRenderer2DMetal.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>

namespace SP::UI {

namespace {

struct Vertex {
    simd::float2 position;
    simd::float4 color;
};

auto to_pixel_format(Builders::PixelFormat format) -> MTLPixelFormat {
    switch (format) {
    case Builders::PixelFormat::RGBA8Unorm:
        return MTLPixelFormatRGBA8Unorm;
    case Builders::PixelFormat::BGRA8Unorm:
        return MTLPixelFormatBGRA8Unorm;
    case Builders::PixelFormat::RGBA8Unorm_sRGB:
        return MTLPixelFormatRGBA8Unorm_sRGB;
    case Builders::PixelFormat::BGRA8Unorm_sRGB:
        return MTLPixelFormatBGRA8Unorm_sRGB;
    case Builders::PixelFormat::RGBA16F:
        return MTLPixelFormatRGBA16Float;
    case Builders::PixelFormat::RGBA32F:
        return MTLPixelFormatRGBA32Float;
    }
    return MTLPixelFormatInvalid;
}

auto compile_pipeline(id<MTLDevice> device, MTLPixelFormat color_format) -> id<MTLRenderPipelineState> {
    static std::unordered_map<NSUInteger, id<MTLRenderPipelineState>> cache;
    auto cached = cache.find(static_cast<NSUInteger>(color_format));
    if (cached != cache.end()) {
        return cached->second;
    }

    static NSString* const kShaderSource = @R"(
        #include <metal_stdlib>
        using namespace metal;

        struct VertexIn {
            float2 position [[attribute(0)]];
            float4 color [[attribute(1)]];
        };

        struct VertexOut {
            float4 position [[position]];
            float4 color;
        };

        vertex VertexOut ps_vertex_main(uint vertex_id [[vertex_id]],
                                        const device VertexIn* vertices [[buffer(0)]]) {
            VertexOut out;
            VertexIn vin = vertices[vertex_id];
            out.position = float4(vin.position, 0.0, 1.0);
            out.color = vin.color;
            return out;
        }

        fragment float4 ps_fragment_main(VertexOut in [[stage_in]]) {
            return in.color;
        }
    )";

    NSError* error = nil;
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    options.fastMathEnabled = YES;
    id<MTLLibrary> library = [device newLibraryWithSource:kShaderSource options:options error:&error];
    if (!library) {
        return nil;
    }

    id<MTLFunction> vertex_fn = [library newFunctionWithName:@"ps_vertex_main"];
    id<MTLFunction> fragment_fn = [library newFunctionWithName:@"ps_fragment_main"];
    if (!vertex_fn || !fragment_fn) {
        return nil;
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = @"PathRenderer2DMetal";
    descriptor.vertexFunction = vertex_fn;
    descriptor.fragmentFunction = fragment_fn;
    descriptor.colorAttachments[0].pixelFormat = color_format;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;

    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) {
        return nil;
    }
    cache.emplace(static_cast<NSUInteger>(color_format), pipeline);
    return pipeline;
}

auto encode_clear_color(std::array<float, 4> rgba) -> MTLClearColor {
    return MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
}

} // namespace

struct PathRenderer2DMetal::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLCommandBuffer> command_buffer = nil;
    id<MTLRenderCommandEncoder> encoder = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    Builders::SurfaceDesc desc{};
    std::vector<Vertex> vertices{};
    MTLPixelFormat pixel_format = MTLPixelFormatInvalid;
    bool valid = false;
};

PathRenderer2DMetal::PathRenderer2DMetal()
    : impl_(std::make_unique<Impl>()) {}

PathRenderer2DMetal::~PathRenderer2DMetal() {
    if (impl_) {
        if (impl_->encoder) {
            [impl_->encoder endEncoding];
            impl_->encoder = nil;
        }
        impl_->command_buffer = nil;
    }
}

PathRenderer2DMetal::PathRenderer2DMetal(PathRenderer2DMetal&& other) noexcept = default;
auto PathRenderer2DMetal::operator=(PathRenderer2DMetal&& other) noexcept -> PathRenderer2DMetal& = default;

auto PathRenderer2DMetal::begin_frame(PathSurfaceMetal& surface,
                                      Builders::SurfaceDesc const& desc,
                                      std::array<float, 4> clear_rgba) -> bool {
    auto* impl = impl_.get();
    if (!impl) {
        return false;
    }

    impl->vertices.clear();
    impl->desc = desc;

    auto* device_ptr = surface.device();
    auto* queue_ptr = surface.command_queue();
    if (!device_ptr || !queue_ptr) {
        return false;
    }
    impl->device = (__bridge id<MTLDevice>)device_ptr;
    impl->command_queue = (__bridge id<MTLCommandQueue>)queue_ptr;

    impl->pixel_format = to_pixel_format(desc.pixel_format);
    if (impl->pixel_format == MTLPixelFormatInvalid) {
        return false;
    }

    impl->pipeline = compile_pipeline(impl->device, impl->pixel_format);
    if (!impl->pipeline) {
        return false;
    }

    impl->command_buffer = [impl->command_queue commandBuffer];
    if (!impl->command_buffer) {
        return false;
    }

    auto texture_info = surface.acquire_texture();
    if (!texture_info.texture) {
        return false;
    }
    id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_info.texture;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = encode_clear_color(clear_rgba);

    impl->encoder = [impl->command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!impl->encoder) {
        return false;
    }

    [impl->encoder setRenderPipelineState:impl->pipeline];
    MTLViewport viewport{};
    viewport.originX = 0.0;
    viewport.originY = 0.0;
    viewport.width = std::max(desc.size_px.width, 1);
    viewport.height = std::max(desc.size_px.height, 1);
    viewport.znear = 0.0;
    viewport.zfar = 1.0;
    [impl->encoder setViewport:viewport];

    impl->valid = true;
    return true;
}

auto PathRenderer2DMetal::draw_rect(Rect const& rect,
                                    std::array<float, 4> color_rgba) -> bool {
    auto* impl = impl_.get();
    if (!impl || !impl->valid) {
        return false;
    }

    auto width = std::max(impl->desc.size_px.width, 1);
    auto height = std::max(impl->desc.size_px.height, 1);
    if (width <= 0 || height <= 0) {
        return false;
    }

    auto to_clip_x = [&](float x) -> float {
        return (x / static_cast<float>(width)) * 2.0f - 1.0f;
    };
    auto to_clip_y = [&](float y) -> float {
        return 1.0f - (y / static_cast<float>(height)) * 2.0f;
    };

    float left = to_clip_x(rect.min_x);
    float right = to_clip_x(rect.max_x);
    float top = to_clip_y(rect.min_y);
    float bottom = to_clip_y(rect.max_y);

    Vertex v0{ {left, bottom}, {color_rgba[0], color_rgba[1], color_rgba[2], color_rgba[3]} };
    Vertex v1{ {right, bottom}, {color_rgba[0], color_rgba[1], color_rgba[2], color_rgba[3]} };
    Vertex v2{ {left, top}, {color_rgba[0], color_rgba[1], color_rgba[2], color_rgba[3]} };
    Vertex v3{ {right, top}, {color_rgba[0], color_rgba[1], color_rgba[2], color_rgba[3]} };

    impl->vertices.push_back(v0);
    impl->vertices.push_back(v1);
    impl->vertices.push_back(v2);
    impl->vertices.push_back(v2);
    impl->vertices.push_back(v1);
    impl->vertices.push_back(v3);
    return true;
}

auto PathRenderer2DMetal::finish(PathSurfaceMetal& surface,
                                 std::uint64_t frame_index,
                                 std::uint64_t revision) -> bool {
    auto* impl = impl_.get();
    if (!impl || !impl->valid) {
        return false;
    }

    if (!impl->encoder || !impl->command_buffer) {
        impl->valid = false;
        return false;
    }

    if (!impl->vertices.empty()) {
        std::size_t data_size = impl->vertices.size() * sizeof(Vertex);
        id<MTLBuffer> buffer = [impl->device newBufferWithBytes:impl->vertices.data()
                                                         length:data_size
                                                        options:MTLResourceStorageModeManaged];
        if (buffer) {
            [impl->encoder setVertexBuffer:buffer offset:0 atIndex:0];
            std::size_t vertex_count = impl->vertices.size();
            [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0
                            vertexCount:static_cast<NSUInteger>(vertex_count)];
        }
    }

    [impl->encoder endEncoding];
    impl->encoder = nil;

    surface.present_completed(frame_index, revision);

    [impl->command_buffer commit];
    [impl->command_buffer waitUntilCompleted];
    impl->command_buffer = nil;

    impl->valid = false;
    return true;
}

} // namespace SP::UI

#endif // defined(__APPLE__) && PATHSPACE_UI_METAL
