#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/task/Future.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SP::UI {
struct PathWindowPresentStats;
}

namespace SP::UI::Builders {

using AppRootPath = SP::App::AppRootPath;
using AppRootPathView = SP::App::AppRootPathView;
using ConcretePath = SP::ConcretePathString;
using ConcretePathView = SP::ConcretePathStringView;
using UnvalidatedPathView = SP::UnvalidatedPathView;
using ScenePath = ConcretePath;
using RendererPath = ConcretePath;
using SurfacePath = ConcretePath;
using WindowPath = ConcretePath;

struct SceneParams {
    std::string name;
    std::string description;
};

struct SceneRevisionDesc {
    uint64_t revision = 0;
    std::chrono::system_clock::time_point published_at{};
    std::string author;
};

struct RendererParams {
    std::string name;
    std::string description;
};

enum class RendererKind {
    Software2D,
    Metal2D,
    Vulkan2D,
};

enum class PixelFormat {
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA8Unorm_sRGB,
    BGRA8Unorm_sRGB,
    RGBA16F,
    RGBA32F,
};

enum class ColorSpace {
    sRGB,
    DisplayP3,
    Linear,
};

struct SurfaceDesc {
    struct SizePx {
        int width = 0;
        int height = 0;
    } size_px;
    PixelFormat pixel_format = PixelFormat::RGBA8Unorm;
    ColorSpace color_space = ColorSpace::sRGB;
    bool premultiplied_alpha = true;
};

struct SurfaceParams {
    std::string name;
    SurfaceDesc desc;
    std::string renderer; // name, app-relative, or absolute path
};

struct WindowParams {
    std::string name;
    std::string title;
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    std::string background;
};

struct RenderSettings {
    struct Time {
        double   time_ms    = 0.0;
        double   delta_ms   = 0.0;
        uint64_t frame_index = 0;
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
        uint32_t flags   = 0;
        bool     enabled = false;
    } debug;

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

enum class ParamUpdateMode {
    Queue,
    ReplaceActive,
};

auto resolve_app_relative(AppRootPathView root,
                          UnvalidatedPathView maybeRelative) -> SP::Expected<ConcretePath>;

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> SP::Expected<ConcretePath>;

namespace Scene {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             SceneParams const& params) -> SP::Expected<ScenePath>;

auto EnsureAuthoringRoot(PathSpace& space,
                          ScenePath const& scenePath) -> SP::Expected<void>;

auto PublishRevision(PathSpace& space,
                      ScenePath const& scenePath,
                      SceneRevisionDesc const& revision,
                      std::span<std::byte const> drawableBucket,
                      std::span<std::byte const> metadata) -> SP::Expected<void>;

auto ReadCurrentRevision(PathSpace const& space,
                          ScenePath const& scenePath) -> SP::Expected<SceneRevisionDesc>;

auto WaitUntilReady(PathSpace& space,
                     ScenePath const& scenePath,
                     std::chrono::milliseconds timeout) -> SP::Expected<void>;

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             RendererParams const& params,
             RendererKind kind) -> SP::Expected<RendererPath>;

auto ResolveTargetBase(PathSpace const& space,
                        AppRootPathView appRoot,
                        RendererPath const& rendererPath,
                        std::string_view targetSpec) -> SP::Expected<ConcretePath>;

auto UpdateSettings(PathSpace& space,
                     ConcretePathView targetPath,
                     RenderSettings const& settings) -> SP::Expected<void>;

auto ReadSettings(PathSpace const& space,
                   ConcretePathView targetPath) -> SP::Expected<RenderSettings>;

auto TriggerRender(PathSpace& space,
                    ConcretePathView targetPath,
                    RenderSettings const& settings) -> SP::Expected<SP::FutureAny>;

} // namespace Renderer

namespace Surface {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             SurfaceParams const& params) -> SP::Expected<SurfacePath>;

auto SetScene(PathSpace& space,
               SurfacePath const& surfacePath,
               ScenePath const& scenePath) -> SP::Expected<void>;

auto RenderOnce(PathSpace& space,
                 SurfacePath const& surfacePath,
                 std::optional<RenderSettings> settingsOverride = std::nullopt) -> SP::Expected<SP::FutureAny>;

} // namespace Surface

namespace Window {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             WindowParams const& params) -> SP::Expected<WindowPath>;

auto AttachSurface(PathSpace& space,
                    WindowPath const& windowPath,
                    std::string_view viewName,
                    SurfacePath const& surfacePath) -> SP::Expected<void>;

auto Present(PathSpace& space,
              WindowPath const& windowPath,
              std::string_view viewName) -> SP::Expected<void>;

} // namespace Window

namespace Diagnostics {

struct TargetMetrics {
    uint64_t frame_index = 0;
    uint64_t revision = 0;
    double render_ms = 0.0;
    double present_ms = 0.0;
    bool last_present_skipped = false;
    std::string last_error;
};

auto ReadTargetMetrics(PathSpace const& space,
                        ConcretePathView targetPath) -> SP::Expected<TargetMetrics>;

auto ClearTargetError(PathSpace& space,
                      ConcretePathView targetPath) -> SP::Expected<void>;

auto WritePresentMetrics(PathSpace& space,
                          ConcretePathView targetPath,
                          PathWindowPresentStats const& stats) -> SP::Expected<void>;

} // namespace Diagnostics

} // namespace SP::UI::Builders
