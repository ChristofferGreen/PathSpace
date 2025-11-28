#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <pathspace/ui/runtime/SurfaceTypes.hpp>

namespace SP::UI::Runtime {

enum class RendererKind {
    Software2D,
    Metal2D,
    Vulkan2D,
};

struct DirtyRectHint {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

[[nodiscard]] inline auto MakeDirtyRectHint(float min_x,
                                            float min_y,
                                            float max_x,
                                            float max_y) -> DirtyRectHint {
    return DirtyRectHint{min_x, min_y, max_x, max_y};
}

struct RenderSettings {
    struct Time {
        double   time_ms    = 0.0;
        double   delta_ms   = 0.0;
        std::uint64_t frame_index = 0;
    } time;

    struct Pacing {
        bool   has_user_cap_fps = false;
        double user_cap_fps     = 0.0;
    } pacing;

    struct Surface {
        struct SizePx {
            int width  = 0;
            int height = 0;
        } size_px;
        float dpi_scale = 1.0f;
        bool  visibility = true;
        MetalSurfaceOptions metal;
    } surface;

    std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 1.0f};

    struct Camera {
        enum class Projection {
            Orthographic,
            Perspective,
        } projection = Projection::Orthographic;
        float z_near = 0.1f;
        float z_far  = 1000.0f;
        bool  enabled = false;
    } camera;

    struct Debug {
        std::uint32_t flags   = 0;
        bool          enabled = false;

        static constexpr std::uint32_t kForceShapedText   = 1u << 6;
        static constexpr std::uint32_t kDisableTextFallback = 1u << 7;
    } debug;

    struct RendererState {
        RendererKind backend_kind = RendererKind::Software2D;
        bool         metal_uploads_enabled = false;
    } renderer;

    struct Cache {
        std::uint64_t cpu_soft_bytes = 0;
        std::uint64_t cpu_hard_bytes = 0;
        std::uint64_t gpu_soft_bytes = 0;
        std::uint64_t gpu_hard_bytes = 0;
    } cache;

    struct MicrotriRT {
        enum class HardwareMode {
            Auto,
            ForceOn,
            ForceOff,
        };

        struct Environment {
            std::string hdr_path;
            float       intensity = 1.0f;
            float       rotation  = 0.0f;
        };

        struct Budget {
            float         microtri_edge_px = 1.0f;
            std::uint32_t max_microtris_per_frame = 200'000;
            std::uint32_t rays_per_vertex = 1;
        };

        struct Path {
            std::uint32_t max_bounces = 1;
            std::uint32_t rr_start_bounce = 1;
            bool          allow_caustics = false;
        };

        struct Clamp {
            float direct = 0.0f;
            float indirect = 0.0f;
            bool  has_direct = false;
            bool  has_indirect = false;
        };

        bool          enabled = false;
        Budget        budget{};
        Path          path{};
        HardwareMode  use_hardware_rt = HardwareMode::Auto;
        Environment   environment{};
        Clamp         clamp{};
        bool          progressive_accumulation = true;
        float         vertex_accum_half_life = 0.25f;
        std::uint64_t seed = 0;
    } microtri_rt;
};

} // namespace SP::UI::Runtime
