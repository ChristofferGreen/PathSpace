#pragma once

#include <array>
#include <memory>

#include <pathspace/ui/SurfaceTypes.hpp>

namespace SP::UI {

class PathSurfaceMetal;

#if defined(__APPLE__) && PATHSPACE_UI_METAL
class PathRenderer2DMetal {
public:
    struct Rect {
        float min_x = 0.0f;
        float min_y = 0.0f;
        float max_x = 0.0f;
        float max_y = 0.0f;
    };

    PathRenderer2DMetal();
    ~PathRenderer2DMetal();

    PathRenderer2DMetal(PathRenderer2DMetal const&) = delete;
    PathRenderer2DMetal& operator=(PathRenderer2DMetal const&) = delete;
    PathRenderer2DMetal(PathRenderer2DMetal&&) noexcept;
    PathRenderer2DMetal& operator=(PathRenderer2DMetal&&) noexcept;

    auto begin_frame(PathSurfaceMetal& surface,
                     Builders::SurfaceDesc const& desc,
                     std::array<float, 4> clear_rgba) -> bool;

    auto draw_rect(Rect const& rect,
                   std::array<float, 4> color_rgba) -> bool;

    auto finish(PathSurfaceMetal& surface,
                std::uint64_t frame_index,
                std::uint64_t revision) -> bool;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace SP::UI

