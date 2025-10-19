#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/ImageCache.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>

#include <chrono>
#include <array>
#include <unordered_map>
#include <vector>

namespace SP::UI {

class PathRenderer2D {
public:
    struct DrawableBounds {
        int min_x = 0;
        int min_y = 0;
        int max_x = 0;
        int max_y = 0;

        [[nodiscard]] auto empty() const -> bool {
            return min_x >= max_x || min_y >= max_y;
        }
    };

    struct RenderParams {
        SP::ConcretePathStringView target_path;
        Builders::RenderSettings const& settings;
        PathSurfaceSoftware& surface;
        Builders::RendererKind backend_kind = Builders::RendererKind::Software2D;
    };

    struct RenderStats {
        std::uint64_t frame_index = 0;
        std::uint64_t revision = 0;
        double render_ms = 0.0;
        std::size_t drawable_count = 0;
        double damage_ms = 0.0;
        double encode_ms = 0.0;
        double progressive_copy_ms = 0.0;
        double publish_ms = 0.0;
        Builders::RendererKind backend_kind = Builders::RendererKind::Software2D;
        std::uint64_t resource_cpu_bytes = 0;
        std::uint64_t resource_gpu_bytes = 0;
    };

    explicit PathRenderer2D(PathSpace& space);

    auto render(RenderParams params) -> SP::Expected<RenderStats>;

private:
    struct DrawableState {
        DrawableBounds bounds{};
        std::uint64_t fingerprint = 0;
    };

    struct TargetState {
        Builders::SurfaceDesc desc{};
        std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 0.0f};
        std::unordered_map<std::uint64_t, DrawableState> drawable_states;
        std::vector<float> linear_buffer;
        std::uint64_t last_revision = 0;
    };

    PathSpace& space_;
    ImageCache image_cache_;

    static auto target_cache() -> std::unordered_map<std::string, TargetState>&;
};

} // namespace SP::UI
