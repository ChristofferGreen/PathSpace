#if defined(__APPLE__) && PATHSPACE_UI_METAL

#import <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/MaterialShaderKey.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/PathRenderer2DMetal.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>

namespace SP::UI {

namespace {

constexpr std::uint32_t kDrawModeSolid = 0u;
constexpr std::uint32_t kDrawModeRoundedRect = 1u;
constexpr std::uint32_t kDrawModeImage = 2u;

constexpr std::uint32_t kDrawFlagRequiresUnpremultiplied = 1u << 0;
constexpr std::uint32_t kDrawFlagDebugOverdraw = 1u << 1;
constexpr std::uint32_t kDrawFlagDebugWireframe = 1u << 2;
constexpr std::uint32_t kDrawFlagEncodeSrgb = 1u << 3;

struct Vertex {
    simd::float2 position;
    simd::float4 color;
    simd::float2 uv;
    simd::float2 local;
};

struct DrawCall {
    std::uint32_t start = 0;
    std::uint32_t count = 0;
    std::uint32_t mode = kDrawModeSolid;
    std::uint32_t flags = 0;
    simd::float4 params0 = {0.0f, 0.0f, 0.0f, 0.0f};
    simd::float4 params1 = {0.0f, 0.0f, 0.0f, 0.0f};
    MTLTriangleFillMode fill_mode = MTLTriangleFillModeFill;
    __strong id<MTLRenderPipelineState> pipeline = nil;
    __strong id<MTLTexture> texture = nil;
};

struct FrameUniforms {
    std::uint32_t encode_mode = 0;
    std::uint32_t padding0 = 0;
    float padding1 = 0.0f;
    float padding2 = 0.0f;
};

struct DrawUniforms {
    std::uint32_t mode = kDrawModeSolid;
    std::uint32_t flags = 0;
    float padding1 = 0.0f;
    float padding2 = 0.0f;
    simd::float4 params0 = {0.0f, 0.0f, 0.0f, 0.0f};
    simd::float4 params1 = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct ImageEntry {
    __strong id<MTLTexture> texture = nil;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

auto encode_clear_color(std::array<float, 4> rgba) -> MTLClearColor {
    return MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
}

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

auto needs_srgb_encode(Builders::SurfaceDesc const& desc) -> bool {
    switch (desc.pixel_format) {
    case Builders::PixelFormat::RGBA8Unorm_sRGB:
    case Builders::PixelFormat::BGRA8Unorm_sRGB:
        return true;
    default:
        break;
    }
    return desc.color_space == Builders::ColorSpace::sRGB;
}

auto compile_pipeline(id<MTLDevice> device,
                      MTLPixelFormat color_format,
                      MaterialShaderKey const& shader_key) -> id<MTLRenderPipelineState> {
    static NSString* const kShaderSource = @R"(
        #include <metal_stdlib>
        using namespace metal;

        struct VertexIn {
            float2 position [[attribute(0)]];
            float4 color [[attribute(1)]];
            float2 uv [[attribute(2)]];
            float2 local [[attribute(3)]];
        };

        struct VertexOut {
            float4 position [[position]];
            float4 color;
            float2 uv;
            float2 local;
        };

        struct FrameUniforms {
            uint encode_mode;
            uint padding0;
            float padding1;
            float padding2;
        };

        struct DrawUniforms {
            uint mode;
            uint flags;
            float padding1;
            float padding2;
            float4 params0;
            float4 params1;
        };

        constant uint kFlagRequiresUnpremultiplied = 1u << 0;
        constant uint kFlagDebugOverdraw = 1u << 1;
        constant uint kFlagDebugWireframe = 1u << 2;
        constant uint kFlagEncodeSrgb = 1u << 3;

        float linear_to_srgb(float value) {
            value = clamp(value, 0.0f, 1.0f);
            if (value <= 0.0031308f) {
                return value * 12.92f;
            }
            return 1.055f * powr(value, 1.0f / 2.4f) - 0.055f;
        }

        vertex VertexOut ps_vertex_main(VertexIn in [[stage_in]]) {
            VertexOut out;
            out.position = float4(in.position, 0.0, 1.0);
            out.color = in.color;
            out.uv = in.uv;
            out.local = in.local;
            return out;
        }

        fragment float4 ps_fragment_main(VertexOut in [[stage_in]],
                                         constant FrameUniforms& frame [[buffer(1)]],
                                         constant DrawUniforms& draw [[buffer(2)]],
                                         texture2d<float> image_tex [[texture(0)]],
                                         sampler image_sampler [[sampler(0)]]) {
            float4 color = in.color;
            bool requires_unpremultiplied = (draw.flags & kFlagRequiresUnpremultiplied) != 0u;
            bool debug_overdraw = (draw.flags & kFlagDebugOverdraw) != 0u;
            bool encode_srgb = (frame.encode_mode != 0u) || ((draw.flags & kFlagEncodeSrgb) != 0u);

            if (draw.mode == 1u) {
                float min_x = draw.params0.x;
                float min_y = draw.params0.y;
                float max_x = draw.params0.z;
                float max_y = draw.params0.w;
                float radius_tl = draw.params1.x;
                float radius_tr = draw.params1.y;
                float radius_br = draw.params1.z;
                float radius_bl = draw.params1.w;
                float2 pos = in.local;

                bool inside = (pos.x >= min_x) && (pos.x <= max_x) && (pos.y >= min_y) && (pos.y <= max_y);
                if (inside && radius_tl > 0.0f && pos.x < (min_x + radius_tl) && pos.y < (min_y + radius_tl)) {
                    float2 delta = pos - float2(min_x + radius_tl, min_y + radius_tl);
                    inside = dot(delta, delta) <= radius_tl * radius_tl;
                }
                if (inside && radius_tr > 0.0f && pos.x > (max_x - radius_tr) && pos.y < (min_y + radius_tr)) {
                    float2 delta = pos - float2(max_x - radius_tr, min_y + radius_tr);
                    inside = dot(delta, delta) <= radius_tr * radius_tr;
                }
                if (inside && radius_br > 0.0f && pos.x > (max_x - radius_br) && pos.y > (max_y - radius_br)) {
                    float2 delta = pos - float2(max_x - radius_br, max_y - radius_br);
                    inside = dot(delta, delta) <= radius_br * radius_br;
                }
                if (inside && radius_bl > 0.0f && pos.x < (min_x + radius_bl) && pos.y > (max_y - radius_bl)) {
                    float2 delta = pos - float2(min_x + radius_bl, max_y - radius_bl);
                    inside = dot(delta, delta) <= radius_bl * radius_bl;
                }
                if (!inside) {
                    return float4(0.0f);
                }
            } else if (draw.mode == 2u) {
                float4 tint = draw.params0;
                float4 sampled = image_tex.sample(image_sampler, in.uv);
                float3 rgb = clamp(sampled.rgb * tint.rgb, 0.0f, 1.0f);
                float alpha = clamp(sampled.a * tint.a, 0.0f, 1.0f);
                if (requires_unpremultiplied) {
                    rgb *= alpha;
                } else {
                    float tint_alpha = clamp(tint.a, 0.0f, 1.0f);
                    rgb *= tint_alpha;
                }
                color = float4(clamp(rgb, 0.0f, 1.0f), alpha);
            } else {
                float alpha = clamp(color.a, 0.0f, 1.0f);
                if (requires_unpremultiplied) {
                    color = float4(clamp(color.rgb * alpha, 0.0f, 1.0f), alpha);
                } else {
                    color = float4(clamp(color.rgb, 0.0f, 1.0f), alpha);
                }
            }

            if (debug_overdraw) {
                float intensity = clamp(color.a, 0.0f, 1.0f);
                color = float4(intensity, 0.0f, 0.0f, max(intensity, 0.1f));
            }

            color = clamp(color, 0.0f, 1.0f);
            if (encode_srgb) {
                color = float4(linear_to_srgb(color.r),
                               linear_to_srgb(color.g),
                               linear_to_srgb(color.b),
                               clamp(color.a, 0.0f, 1.0f));
            }
            return color;
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

    MTLVertexDescriptor* vertex_desc = [[MTLVertexDescriptor alloc] init];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[0].offset = offsetof(Vertex, position);
    vertex_desc.attributes[0].bufferIndex = 0;

    vertex_desc.attributes[1].format = MTLVertexFormatFloat4;
    vertex_desc.attributes[1].offset = offsetof(Vertex, color);
    vertex_desc.attributes[1].bufferIndex = 0;

    vertex_desc.attributes[2].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[2].offset = offsetof(Vertex, uv);
    vertex_desc.attributes[2].bufferIndex = 0;

    vertex_desc.attributes[3].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[3].offset = offsetof(Vertex, local);
    vertex_desc.attributes[3].bufferIndex = 0;

    vertex_desc.layouts[0].stride = sizeof(Vertex);
    vertex_desc.layouts[0].stepRate = 1;
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = @"PathRenderer2DMetal";
    descriptor.vertexFunction = vertex_fn;
    descriptor.fragmentFunction = fragment_fn;
    descriptor.vertexDescriptor = vertex_desc;
    descriptor.colorAttachments[0].pixelFormat = color_format;
    if (shader_key.alpha_blend) {
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    } else {
        descriptor.colorAttachments[0].blendingEnabled = NO;
    }

    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) {
        return nil;
    }
    return pipeline;
}

auto to_simd(std::array<float, 4> const& color) -> simd::float4 {
    return {color[0], color[1], color[2], color[3]};
}

auto to_clip_x(float x, int width) -> float {
    if (width <= 0) {
        return -1.0f;
    }
    return (x / static_cast<float>(width)) * 2.0f - 1.0f;
}

auto to_clip_y(float y, int height) -> float {
    if (height <= 0) {
        return 1.0f;
    }
    return 1.0f - (y / static_cast<float>(height)) * 2.0f;
}

struct PipelineKey {
    MTLPixelFormat format = MTLPixelFormatInvalid;
    MaterialShaderKey shader_key{};
    auto operator==(PipelineKey const&) const -> bool = default;
};

struct PipelineKeyHash {
    auto operator()(PipelineKey const& key) const noexcept -> std::size_t {
        using FormatType = std::underlying_type_t<MTLPixelFormat>;
        std::size_t h = std::hash<FormatType>{}(static_cast<FormatType>(key.format));
        auto mix = [&](auto value) {
            std::size_t v = std::hash<std::decay_t<decltype(value)>>{}(value);
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        };
        mix(key.shader_key.pipeline_flags);
        mix(key.shader_key.alpha_blend);
        mix(key.shader_key.requires_unpremultiplied);
        mix(key.shader_key.srgb_framebuffer);
        mix(key.shader_key.uses_image);
        mix(key.shader_key.debug_overdraw);
        mix(key.shader_key.debug_wireframe);
        return h;
    }
};

auto compute_draw_flags(MaterialShaderKey const& key) -> std::uint32_t {
    std::uint32_t flags = 0;
    if (key.requires_unpremultiplied) {
        flags |= kDrawFlagRequiresUnpremultiplied;
    }
    if (key.debug_overdraw) {
        flags |= kDrawFlagDebugOverdraw;
    }
    if (key.debug_wireframe) {
        flags |= kDrawFlagDebugWireframe;
    }
    if (key.srgb_framebuffer) {
        flags |= kDrawFlagEncodeSrgb;
    }
    return flags;
}

} // namespace

struct PathRenderer2DMetal::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLCommandBuffer> command_buffer = nil;
    id<MTLRenderCommandEncoder> encoder = nil;
    id<MTLRenderPipelineState> default_pipeline = nil;
    id<MTLSamplerState> sampler = nil;
    id<MTLBuffer> frame_uniform_buffer = nil;
    id<MTLBuffer> vertex_buffer = nil;
    Builders::SurfaceDesc desc{};
    std::vector<Vertex> vertices{};
    std::vector<DrawCall> draw_calls{};
    std::unordered_map<std::uint64_t, ImageEntry> image_textures{};
    std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, PipelineKeyHash> pipeline_cache{};
    MaterialShaderKey active_key{};
    bool active_key_valid = false;
    std::uint32_t active_material_id = 0;
    __strong id<MTLRenderPipelineState> current_pipeline = nil;
    std::uint32_t current_draw_flags = 0;
    MTLTriangleFillMode current_fill_mode = MTLTriangleFillModeFill;
    MTLPixelFormat pixel_format = MTLPixelFormatInvalid;
    bool valid = false;
    bool encode_srgb = false;

    auto pipeline_for(MaterialShaderKey const& key) -> id<MTLRenderPipelineState> {
        PipelineKey cache_key{pixel_format, key};
        auto it = pipeline_cache.find(cache_key);
        if (it != pipeline_cache.end()) {
            return it->second;
        }
        if (!device) {
            return nil;
        }
        id<MTLRenderPipelineState> pipeline = compile_pipeline(device, pixel_format, key);
        if (!pipeline) {
            return nil;
        }
        pipeline_cache.emplace(cache_key, pipeline);
        return pipeline;
    }
};

PathRenderer2DMetal::PathRenderer2DMetal()
    : impl_(std::make_unique<Impl>()) {}

PathRenderer2DMetal::~PathRenderer2DMetal() = default;

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
    impl->draw_calls.clear();
    impl->valid = false;

    impl->desc = desc;
    impl->encode_srgb = needs_srgb_encode(desc);

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

    MaterialShaderKey default_key{};
    default_key.pipeline_flags = 0u;
    default_key.alpha_blend = true;
    default_key.srgb_framebuffer = impl->encode_srgb;
    impl->default_pipeline = impl->pipeline_for(default_key);
    if (!impl->default_pipeline) {
        return false;
    }
    impl->current_pipeline = nil;
    impl->current_draw_flags = 0;
    impl->current_fill_mode = MTLTriangleFillModeFill;
    impl->active_key_valid = false;
    impl->active_material_id = 0;

    if (!impl->sampler) {
        MTLSamplerDescriptor* sampler_desc = [[MTLSamplerDescriptor alloc] init];
        sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
        sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
        sampler_desc.mipFilter = MTLSamplerMipFilterLinear;
        sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler_desc.rAddressMode = MTLSamplerAddressModeClampToEdge;
        impl->sampler = [impl->device newSamplerStateWithDescriptor:sampler_desc];
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
    [impl->encoder setRenderPipelineState:impl->default_pipeline];

    MTLViewport viewport{};
    viewport.originX = 0.0;
    viewport.originY = 0.0;
    viewport.width = std::max(desc.size_px.width, 1);
    viewport.height = std::max(desc.size_px.height, 1);
    viewport.znear = 0.0;
    viewport.zfar = 1.0;
    [impl->encoder setViewport:viewport];

    FrameUniforms frame_uniforms{};
    frame_uniforms.encode_mode = impl->encode_srgb ? 1u : 0u;
    impl->frame_uniform_buffer = [impl->device newBufferWithBytes:&frame_uniforms
                                                           length:sizeof(FrameUniforms)
                                                          options:MTLResourceStorageModeManaged];
    if (!impl->frame_uniform_buffer) {
        return false;
    }
    [impl->encoder setFragmentBuffer:impl->frame_uniform_buffer offset:0 atIndex:1];
    [impl->encoder setFragmentSamplerState:impl->sampler atIndex:0];

    impl->valid = true;
    return true;
}

auto PathRenderer2DMetal::draw_rect(Rect const& rect,
                                    std::array<float, 4> color_rgba) -> bool {
    auto* impl = impl_.get();
    if (!impl || !impl->valid) {
        return false;
    }

    id<MTLRenderPipelineState> pipeline = impl->current_pipeline ? impl->current_pipeline
                                                                : impl->default_pipeline;
    if (!pipeline) {
        return false;
    }
    auto const draw_flags = impl->current_draw_flags;
    auto const fill_mode = impl->current_fill_mode;

    auto width = std::max(impl->desc.size_px.width, 1);
    auto height = std::max(impl->desc.size_px.height, 1);

    auto make_vertex = [&](float x, float y, float u, float v) -> Vertex {
        Vertex vertex{};
        vertex.position = {to_clip_x(x, width), to_clip_y(y, height)};
        vertex.color = to_simd(color_rgba);
        vertex.uv = {u, v};
        vertex.local = {x, y};
        return vertex;
    };

    auto min_x = rect.min_x;
    auto max_x = rect.max_x;
    auto min_y = rect.min_y;
    auto max_y = rect.max_y;

    std::uint32_t start = static_cast<std::uint32_t>(impl->vertices.size());
    impl->vertices.push_back(make_vertex(min_x, max_y, 0.0f, 1.0f));
    impl->vertices.push_back(make_vertex(max_x, max_y, 1.0f, 1.0f));
    impl->vertices.push_back(make_vertex(min_x, min_y, 0.0f, 0.0f));
    impl->vertices.push_back(make_vertex(min_x, min_y, 0.0f, 0.0f));
    impl->vertices.push_back(make_vertex(max_x, max_y, 1.0f, 1.0f));
    impl->vertices.push_back(make_vertex(max_x, min_y, 1.0f, 0.0f));

    DrawCall call{};
    call.start = start;
    call.count = 6;
    call.mode = kDrawModeSolid;
    call.flags = draw_flags;
    call.pipeline = pipeline;
    call.fill_mode = fill_mode;
    impl->draw_calls.push_back(call);
    return true;
}

auto PathRenderer2DMetal::draw_text_quad(Scene::TextGlyphsCommand const& command,
                                         std::array<float, 4> color_linear_premul) -> bool {
    Rect rect{
        .min_x = std::min(command.min_x, command.max_x),
        .min_y = std::min(command.min_y, command.max_y),
        .max_x = std::max(command.min_x, command.max_x),
        .max_y = std::max(command.min_y, command.max_y),
    };
    return draw_rect(rect, color_linear_premul);
}

auto PathRenderer2DMetal::draw_rounded_rect(Scene::RoundedRectCommand const& command,
                                            std::array<float, 4> color_linear_premul) -> bool {
    auto* impl = impl_.get();
    if (!impl || !impl->valid) {
        return false;
    }

    float min_x = std::min(command.min_x, command.max_x);
    float max_x = std::max(command.min_x, command.max_x);
    float min_y = std::min(command.min_y, command.max_y);
    float max_y = std::max(command.min_y, command.max_y);

    if (!draw_rect(Rect{min_x, min_y, max_x, max_y}, color_linear_premul)) {
        return false;
    }

    auto radius_positive = [](float value) -> float {
        return std::max(0.0f, value);
    };

    float radius_tl = radius_positive(command.radius_top_left);
    float radius_tr = radius_positive(command.radius_top_right);
    float radius_br = radius_positive(command.radius_bottom_right);
    float radius_bl = radius_positive(command.radius_bottom_left);

    auto adjust_pair = [](float& a, float& b, float limit) {
        if (limit <= 0.0f) {
            a = 0.0f;
            b = 0.0f;
            return;
        }
        float sum = a + b;
        if (sum > limit && sum > 0.0f) {
            float scale = limit / sum;
            a *= scale;
            b *= scale;
        }
    };

    adjust_pair(radius_tl, radius_tr, max_x - min_x);
    adjust_pair(radius_bl, radius_br, max_x - min_x);
    adjust_pair(radius_tl, radius_bl, max_y - min_y);
    adjust_pair(radius_tr, radius_br, max_y - min_y);

    if (impl->draw_calls.empty()) {
        return false;
    }
    auto& call = impl->draw_calls.back();
    call.mode = kDrawModeRoundedRect;
    call.params0 = {min_x, min_y, max_x, max_y};
    call.params1 = {radius_tl, radius_tr, radius_br, radius_bl};
    return true;
}

auto PathRenderer2DMetal::draw_image(Scene::ImageCommand const& command,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     float const* pixels,
                                     std::size_t pixel_count,
                                     std::array<float, 4> tint_linear_straight) -> bool {
    auto* impl = impl_.get();
    if (!impl || !impl->valid) {
        return false;
    }
    id<MTLRenderPipelineState> pipeline = impl->current_pipeline ? impl->current_pipeline
                                                                : impl->default_pipeline;
    if (!pipeline) {
        return false;
    }
    auto const draw_flags = impl->current_draw_flags;
    auto const fill_mode = impl->current_fill_mode;
    if (width == 0 || height == 0 || pixels == nullptr) {
        return false;
    }
    std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (pixel_count < expected) {
        return false;
    }

    auto& entry = impl->image_textures[command.image_fingerprint];
    if (!entry.texture || entry.width != width || entry.height != height) {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeManaged;
        entry.texture = [impl->device newTextureWithDescriptor:descriptor];
        entry.width = width;
        entry.height = height;
    }

    if (!entry.texture) {
        return false;
    }

    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    std::size_t row_bytes = static_cast<std::size_t>(width) * 4u * sizeof(float);
    [entry.texture replaceRegion:region mipmapLevel:0 withBytes:pixels bytesPerRow:row_bytes];

    Rect rect{
        .min_x = std::min(command.min_x, command.max_x),
        .min_y = std::min(command.min_y, command.max_y),
        .max_x = std::max(command.min_x, command.max_x),
        .max_y = std::max(command.min_y, command.max_y),
    };

    auto width_px = std::max(impl->desc.size_px.width, 1);
    auto height_px = std::max(impl->desc.size_px.height, 1);
    auto make_vertex = [&](float x, float y, float u, float v) -> Vertex {
        Vertex vertex{};
        vertex.position = {to_clip_x(x, width_px), to_clip_y(y, height_px)};
        vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
        vertex.uv = {u, v};
        vertex.local = {x, y};
        return vertex;
    };

    std::uint32_t start = static_cast<std::uint32_t>(impl->vertices.size());
    impl->vertices.push_back(make_vertex(rect.min_x, rect.max_y, command.uv_min_x, command.uv_max_y));
    impl->vertices.push_back(make_vertex(rect.max_x, rect.max_y, command.uv_max_x, command.uv_max_y));
    impl->vertices.push_back(make_vertex(rect.min_x, rect.min_y, command.uv_min_x, command.uv_min_y));
    impl->vertices.push_back(make_vertex(rect.min_x, rect.min_y, command.uv_min_x, command.uv_min_y));
    impl->vertices.push_back(make_vertex(rect.max_x, rect.max_y, command.uv_max_x, command.uv_max_y));
    impl->vertices.push_back(make_vertex(rect.max_x, rect.min_y, command.uv_max_x, command.uv_min_y));

    DrawCall call{};
    call.start = start;
    call.count = 6;
    call.mode = kDrawModeImage;
    call.texture = entry.texture;
    call.params0 = to_simd(tint_linear_straight);
    call.flags = draw_flags;
    call.pipeline = pipeline;
    call.fill_mode = fill_mode;
    impl->draw_calls.push_back(call);
    return true;
}

auto PathRenderer2DMetal::bind_material(MaterialDescriptor const& descriptor) -> bool {
    auto* impl = impl_.get();
    if (!impl || !impl->valid) {
        return false;
    }

    MaterialShaderKey key = make_shader_key(descriptor, impl->desc);
    bool reuse_current = impl->active_key_valid
                         && impl->active_material_id == descriptor.material_id
                         && impl->active_key == key
                         && impl->current_pipeline != nil;

    if (!reuse_current) {
        id<MTLRenderPipelineState> pipeline = impl->pipeline_for(key);
        if (!pipeline) {
            impl->current_pipeline = nil;
            impl->current_draw_flags = 0;
            impl->current_fill_mode = MTLTriangleFillModeFill;
            impl->active_key_valid = false;
            impl->active_material_id = 0;
            return false;
        }
        impl->current_pipeline = pipeline;
        impl->active_key = key;
        impl->active_key_valid = true;
        impl->active_material_id = descriptor.material_id;
    }

    impl->current_draw_flags = compute_draw_flags(key);
    impl->current_fill_mode = key.debug_wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
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

    if (!impl->vertices.empty() && !impl->draw_calls.empty()) {
        std::size_t data_size = impl->vertices.size() * sizeof(Vertex);
        impl->vertex_buffer = [impl->device newBufferWithBytes:impl->vertices.data()
                                                        length:data_size
                                                       options:MTLResourceStorageModeManaged];
        if (!impl->vertex_buffer) {
            impl->valid = false;
            return false;
        }
        [impl->encoder setVertexBuffer:impl->vertex_buffer offset:0 atIndex:0];

        for (auto const& call : impl->draw_calls) {
            id<MTLRenderPipelineState> pipeline = call.pipeline ? call.pipeline : impl->default_pipeline;
            if (!pipeline) {
                impl->valid = false;
                return false;
            }
            [impl->encoder setRenderPipelineState:pipeline];
            [impl->encoder setTriangleFillMode:call.fill_mode];

            DrawUniforms uniforms{};
            uniforms.mode = call.mode;
            uniforms.flags = call.flags;
            uniforms.params0 = call.params0;
            uniforms.params1 = call.params1;
            [impl->encoder setFragmentBytes:&uniforms length:sizeof(DrawUniforms) atIndex:2];
            if (call.mode == kDrawModeImage) {
                [impl->encoder setFragmentTexture:call.texture atIndex:0];
            } else {
                [impl->encoder setFragmentTexture:nil atIndex:0];
            }
            [impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                               vertexStart:call.start
                               vertexCount:call.count];
        }
    }

    impl->vertices.clear();
    impl->draw_calls.clear();

    [impl->encoder endEncoding];
    impl->encoder = nil;

    surface.present_completed(frame_index, revision);

    [impl->command_buffer commit];
    [impl->command_buffer waitUntilCompleted];
    impl->command_buffer = nil;
    impl->vertex_buffer = nil;
    impl->frame_uniform_buffer = nil;

    impl->current_pipeline = nil;
    impl->current_draw_flags = 0;
    impl->current_fill_mode = MTLTriangleFillModeFill;
    impl->active_key_valid = false;
    impl->active_material_id = 0;

    impl->valid = false;
    return true;
}

} // namespace SP::UI

#endif // defined(__APPLE__) && PATHSPACE_UI_METAL
