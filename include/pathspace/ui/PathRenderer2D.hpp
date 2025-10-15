#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>

#include <chrono>

namespace SP::UI {

class PathRenderer2D {
public:
    struct RenderParams {
        SP::ConcretePathStringView target_path;
        Builders::RenderSettings const& settings;
        PathSurfaceSoftware& surface;
    };

    struct RenderStats {
        std::uint64_t frame_index = 0;
        std::uint64_t revision = 0;
        double render_ms = 0.0;
        std::size_t drawable_count = 0;
    };

    explicit PathRenderer2D(PathSpace& space);

    auto render(RenderParams params) -> SP::Expected<RenderStats>;

private:
    PathSpace& space_;
};

} // namespace SP::UI

