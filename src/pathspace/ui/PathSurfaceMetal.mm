#import <Metal/Metal.h>

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

void PathSurfaceMetal::present_completed(std::uint64_t frame_index, std::uint64_t revision) {
    if (!impl_) {
        return;
    }
    impl_->frame_index = frame_index;
    impl_->revision = revision;
}

#endif // defined(__APPLE__)

} // namespace SP::UI
