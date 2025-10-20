#import <Metal/Metal.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <string>
#include <IOSurface/IOSurface.h>

#include <pathspace/ui/PathSurfaceMetal.hpp>

namespace SP::UI {

#if defined(__APPLE__)

namespace {

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

auto clamp_dimension(int value) -> NSUInteger {
    if (value <= 0) {
        return 1;
    }
    return static_cast<NSUInteger>(value);
}

auto to_mtl_storage_mode(Builders::MetalStorageMode mode) -> MTLStorageMode {
    switch (mode) {
    case Builders::MetalStorageMode::Private:
        return MTLStorageModePrivate;
    case Builders::MetalStorageMode::Shared:
        return MTLStorageModeShared;
    case Builders::MetalStorageMode::Managed:
#if TARGET_OS_OSX
        return MTLStorageModeManaged;
#else
        return MTLStorageModeShared;
#endif
    case Builders::MetalStorageMode::Memoryless:
#if TARGET_OS_IPHONE
        return MTLStorageModeMemoryless;
#else
        return MTLStorageModePrivate;
#endif
    }
    return MTLStorageModePrivate;
}

auto to_mtl_usage(std::uint8_t usageFlags) -> MTLTextureUsage {
    MTLTextureUsage usage = 0;
    if (Builders::metal_usage_contains(usageFlags, Builders::MetalTextureUsage::ShaderRead)) {
        usage |= MTLTextureUsageShaderRead;
    }
    if (Builders::metal_usage_contains(usageFlags, Builders::MetalTextureUsage::ShaderWrite)) {
        usage |= MTLTextureUsageShaderWrite;
    }
    if (Builders::metal_usage_contains(usageFlags, Builders::MetalTextureUsage::RenderTarget)) {
        usage |= MTLTextureUsageRenderTarget;
    }
#ifdef MTLTextureUsageBlit
    if (Builders::metal_usage_contains(usageFlags, Builders::MetalTextureUsage::Blit)) {
        usage |= MTLTextureUsageBlit;
    }
#endif
    if (usage == 0) {
        usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    }
    return usage;
}

} // namespace

struct PathSurfaceMetal::Impl {
    Builders::SurfaceDesc desc{};
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLTexture> texture = nil;
    std::uint64_t frame_index = 0;
    std::uint64_t revision = 0;
    std::vector<MaterialDescriptor> materials{};
    std::vector<MaterialShaderKey> shader_keys{};
    std::vector<MaterialResourceResidency> material_resources{};

    explicit Impl(Builders::SurfaceDesc d)
        : desc(d)
        , device(MTLCreateSystemDefaultDevice())
        , command_queue([device newCommandQueue]) {
        ensure_texture();
    }

    ~Impl() {
        texture = nil;
        command_queue = nil;
        device = nil;
    }

    void ensure_texture() {
        if (!device) {
            texture = nil;
            return;
        }

        auto mtl_format = to_pixel_format(desc.pixel_format);
        if (mtl_format == MTLPixelFormatInvalid) {
            mtl_format = MTLPixelFormatBGRA8Unorm;
        }

        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
                                                               width:clamp_dimension(desc.size_px.width)
                                                              height:clamp_dimension(desc.size_px.height)
                                                           mipmapped:NO];
        descriptor.usage = to_mtl_usage(desc.metal.texture_usage);
        descriptor.storageMode = to_mtl_storage_mode(desc.metal.storage_mode);

        texture = [device newTextureWithDescriptor:descriptor];
    }
};

PathSurfaceMetal::PathSurfaceMetal(Builders::SurfaceDesc desc)
    : impl_(std::make_unique<Impl>(desc)) {
}

PathSurfaceMetal::~PathSurfaceMetal() = default;

PathSurfaceMetal::PathSurfaceMetal(PathSurfaceMetal&& other) noexcept = default;
auto PathSurfaceMetal::operator=(PathSurfaceMetal&& other) noexcept -> PathSurfaceMetal& = default;

void PathSurfaceMetal::resize(Builders::SurfaceDesc const& desc) {
    if (!impl_) {
        return;
    }
    impl_->desc = desc;
    impl_->ensure_texture();
}

auto PathSurfaceMetal::desc() const -> Builders::SurfaceDesc const& {
    return impl_->desc;
}

auto PathSurfaceMetal::acquire_texture() -> TextureInfo {
    if (!impl_) {
        return {};
    }
    if (!impl_->texture) {
        impl_->ensure_texture();
    }
    TextureInfo info{};
    info.texture = (__bridge void*)impl_->texture;
    info.frame_index = impl_->frame_index;
    info.revision = impl_->revision;
    return info;
}

void PathSurfaceMetal::update_from_rgba8(std::span<std::uint8_t const> pixels,
                                         std::size_t bytes_per_row,
                                         std::uint64_t frame_index,
                                         std::uint64_t revision) {
    if (!impl_) {
        return;
    }
    if (!impl_->texture) {
        impl_->ensure_texture();
    }
    if (!impl_->texture) {
        return;
    }

    auto const width = std::max(impl_->desc.size_px.width, 1);
    auto const height = std::max(impl_->desc.size_px.height, 1);
    auto const min_row_bytes = static_cast<std::size_t>(width) * 4u;
    if (bytes_per_row == 0) {
        bytes_per_row = min_row_bytes;
    }
    if (bytes_per_row < min_row_bytes) {
        bytes_per_row = min_row_bytes;
    }
    if (pixels.size() < bytes_per_row * static_cast<std::size_t>(height)) {
        return;
    }

    MTLRegion region = MTLRegionMake2D(0, 0, clamp_dimension(width), clamp_dimension(height));
    [impl_->texture replaceRegion:region
                      mipmapLevel:0
                      withBytes:pixels.data()
                    bytesPerRow:bytes_per_row];
    impl_->frame_index = frame_index;
    impl_->revision = revision;
}

void PathSurfaceMetal::present_completed(std::uint64_t frame_index, std::uint64_t revision) {
    if (!impl_) {
        return;
    }
    impl_->frame_index = frame_index;
    impl_->revision = revision;
}

auto PathSurfaceMetal::resident_gpu_bytes() const -> std::size_t {
    if (!impl_ || !impl_->texture) {
        return 0;
    }
    std::size_t allocated_bytes = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
    if ([impl_->texture respondsToSelector:@selector(allocatedSize)]) {
        auto const size = impl_->texture.allocatedSize;
        if (size > 0) {
            allocated_bytes = static_cast<std::size_t>(size);
        }
    }
#pragma clang diagnostic pop
    if (allocated_bytes != 0) {
        for (auto const& resource : impl_->material_resources) {
            allocated_bytes += resource.gpu_bytes;
        }
        return allocated_bytes;
    }
    auto const width = std::max(impl_->desc.size_px.width, 0);
    auto const height = std::max(impl_->desc.size_px.height, 0);
    auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    for (auto const& resource : impl_->material_resources) {
        bytes += resource.gpu_bytes;
    }
    return bytes;
}

void PathSurfaceMetal::update_material_descriptors(std::span<MaterialDescriptor const> descriptors) {
    if (!impl_) {
        return;
    }
    impl_->materials.assign(descriptors.begin(), descriptors.end());
    impl_->shader_keys.clear();
    impl_->shader_keys.reserve(impl_->materials.size());
    for (auto const& material : impl_->materials) {
        impl_->shader_keys.push_back(make_shader_key(material, impl_->desc));
    }
}

auto PathSurfaceMetal::material_descriptors() const -> std::span<MaterialDescriptor const> {
    if (!impl_) {
        return {};
    }
    return std::span<MaterialDescriptor const>{impl_->materials.data(), impl_->materials.size()};
}

void PathSurfaceMetal::update_resource_residency(std::span<MaterialResourceResidency const> resources) {
    if (!impl_) {
        return;
    }
    impl_->material_resources.assign(resources.begin(), resources.end());
}

auto PathSurfaceMetal::resource_residency() const -> std::span<MaterialResourceResidency const> {
    if (!impl_) {
        return {};
    }
    return std::span<MaterialResourceResidency const>{impl_->material_resources.data(),
                                                      impl_->material_resources.size()};
}

auto PathSurfaceMetal::shader_keys() const -> std::span<MaterialShaderKey const> {
    if (!impl_) {
        return {};
    }
    return std::span<MaterialShaderKey const>{impl_->shader_keys.data(), impl_->shader_keys.size()};
}

auto PathSurfaceMetal::blit_from_iosurface(IOSurfaceRef surface,
                                           std::uint64_t frame_index,
                                           std::uint64_t revision,
                                           std::string* error_message) -> bool {
    if (!impl_ || !surface) {
        if (error_message) {
            *error_message = "IOSurface unavailable for Metal upload";
        }
        return false;
    }
    if (!impl_->device || !impl_->command_queue) {
        if (error_message) {
            *error_message = "Metal device or command queue unavailable";
        }
        return false;
    }

    impl_->ensure_texture();
    if (!impl_->texture) {
        if (error_message) {
            *error_message = "Failed to allocate Metal render texture";
        }
        return false;
    }

    auto width = clamp_dimension(IOSurfaceGetWidth(surface));
    auto height = clamp_dimension(IOSurfaceGetHeight(surface));
    if (width == 0 || height == 0) {
        if (error_message) {
            *error_message = "IOSurface has invalid dimensions";
        }
        return false;
    }

    auto mtl_format = to_pixel_format(impl_->desc.pixel_format);
    if (mtl_format == MTLPixelFormatInvalid) {
        mtl_format = impl_->texture.pixelFormat;
    }

    @autoreleasepool {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
#ifdef MTLTextureUsageBlit
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageBlit;
#else
        descriptor.usage = MTLTextureUsageShaderRead;
#endif

        id<MTLTexture> source =
            [impl_->device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        if (!source) {
            if (error_message) {
                *error_message = "Failed to create Metal texture view for IOSurface";
            }
            return false;
        }

        id<MTLCommandBuffer> command_buffer = [impl_->command_queue commandBuffer];
        if (!command_buffer) {
            if (error_message) {
                *error_message = "Failed to create Metal command buffer";
            }
            return false;
        }

        id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
        if (!blit_encoder) {
            if (error_message) {
                *error_message = "Failed to create Metal blit encoder";
            }
            return false;
        }

        MTLSize copy_size = MTLSizeMake(width, height, 1);
        [blit_encoder copyFromTexture:source
                           sourceSlice:0
                            sourceLevel:0
                           sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:copy_size
                             toTexture:impl_->texture
                      destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit_encoder endEncoding];

        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            if (error_message) {
                NSError* command_error = command_buffer.error;
                if (command_error) {
                    auto const* text = [[command_error localizedDescription] UTF8String];
                    if (text) {
                        *error_message = std::string{"Metal blit failed: "} + text;
                    } else {
                        *error_message = "Metal blit failed: unknown error";
                    }
                } else {
                    *error_message = "Metal blit failed with status "
                                     + std::to_string(static_cast<int>(command_buffer.status));
                }
            }
            return false;
        }
    }

    impl_->frame_index = frame_index;
    impl_->revision = revision;
    return true;
}

#endif // defined(__APPLE__)

} // namespace SP::UI
