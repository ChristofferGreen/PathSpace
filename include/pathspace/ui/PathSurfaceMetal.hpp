#pragma once

#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/SurfaceTypes.hpp>

#include <cstdint>
#include <span>
#include <memory>
#include <stdexcept>
#include <vector>

namespace SP::UI {

struct PathSurfaceMetalTextureInfo {
    void* texture = nullptr;
    std::uint64_t frame_index = 0;
    std::uint64_t revision = 0;
};

#if defined(__APPLE__)

class PathSurfaceMetal {
public:
    using TextureInfo = PathSurfaceMetalTextureInfo;

    explicit PathSurfaceMetal(Builders::SurfaceDesc desc);
    ~PathSurfaceMetal();

    PathSurfaceMetal(PathSurfaceMetal const&) = delete;
    PathSurfaceMetal& operator=(PathSurfaceMetal const&) = delete;
    PathSurfaceMetal(PathSurfaceMetal&&) noexcept;
    PathSurfaceMetal& operator=(PathSurfaceMetal&&) noexcept;

    void resize(Builders::SurfaceDesc const& desc);

    [[nodiscard]] auto desc() const -> Builders::SurfaceDesc const&;
    [[nodiscard]] auto acquire_texture() -> TextureInfo;
    void update_from_rgba8(std::span<std::uint8_t const> pixels,
                           std::size_t bytes_per_row,
                           std::uint64_t frame_index,
                           std::uint64_t revision);
    void present_completed(std::uint64_t frame_index, std::uint64_t revision);
    void update_material_descriptors(std::span<MaterialDescriptor const> descriptors);
    [[nodiscard]] auto material_descriptors() const -> std::span<MaterialDescriptor const>;
    [[nodiscard]] auto resident_gpu_bytes() const -> std::size_t;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#else

class PathSurfaceMetal {
public:
    using TextureInfo = PathSurfaceMetalTextureInfo;

    explicit PathSurfaceMetal(Builders::SurfaceDesc) {
        throw std::runtime_error("PathSurfaceMetal is only available on Apple platforms.");
    }
    void resize(Builders::SurfaceDesc const&) {}
    [[nodiscard]] auto desc() const -> Builders::SurfaceDesc const& {
        throw std::runtime_error("PathSurfaceMetal is only available on Apple platforms.");
    }
    [[nodiscard]] auto acquire_texture() -> TextureInfo {
        throw std::runtime_error("PathSurfaceMetal is only available on Apple platforms.");
    }
    void update_from_rgba8(std::span<std::uint8_t const>, std::size_t, std::uint64_t, std::uint64_t) {}
    void present_completed(std::uint64_t, std::uint64_t) {}
    void update_material_descriptors(std::span<MaterialDescriptor const>) {}
    [[nodiscard]] auto material_descriptors() const -> std::span<MaterialDescriptor const> {
        return {};
    }
    [[nodiscard]] auto resident_gpu_bytes() const -> std::size_t { return 0; }
};

#endif

} // namespace SP::UI
