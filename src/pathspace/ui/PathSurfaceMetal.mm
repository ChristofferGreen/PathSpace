#import <Metal/Metal.h>

#include <algorithm>

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

} // namespace

struct PathSurfaceMetal::Impl {
    Builders::SurfaceDesc desc{};
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    id<MTLTexture> texture = nil;
    std::uint64_t frame_index = 0;
    std::uint64_t revision = 0;

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
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        descriptor.storageMode = MTLStorageModePrivate;

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

#endif // defined(__APPLE__)

} // namespace SP::UI
