#pragma once

#include <pathspace/ui/SurfaceTypes.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace SP::UI {

#if defined(__APPLE__)

class PathSurfaceMetal {
public:
    struct TextureInfo {
        void* texture = nullptr;          // stored as opaque pointer to id<MTLTexture>
        std::uint64_t frame_index = 0;
        std::uint64_t revision = 0;
    };

    explicit PathSurfaceMetal(Builders::SurfaceDesc desc);
    ~PathSurfaceMetal();

    PathSurfaceMetal(PathSurfaceMetal const&) = delete;
    PathSurfaceMetal& operator=(PathSurfaceMetal const&) = delete;
    PathSurfaceMetal(PathSurfaceMetal&&) noexcept;
    PathSurfaceMetal& operator=(PathSurfaceMetal&&) noexcept;

    void resize(Builders::SurfaceDesc const& desc);

    [[nodiscard]] auto desc() const -> Builders::SurfaceDesc const&;
    [[nodiscard]] auto acquire_texture() -> TextureInfo;
    void present_completed(std::uint64_t frame_index, std::uint64_t revision);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#else

class PathSurfaceMetal {
public:
    explicit PathSurfaceMetal(Builders::SurfaceDesc) {
        throw std::runtime_error("PathSurfaceMetal is only available on Apple platforms.");
    }
};

#endif

} // namespace SP::UI
