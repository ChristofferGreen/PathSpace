#pragma once

#if defined(PATHSPACE_DISABLE_LEGACY_BUILDERS)
#error "Legacy widget builders have been disabled (set PATHSPACE_DISABLE_LEGACY_BUILDERS=OFF or migrate to SP::UI::Declarative)."
#endif

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/task/Future.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/HtmlSerialization.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SurfaceTypes.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>

namespace SP::UI::Builders {

struct SceneParams {
    std::string name;
    std::string description;
};

struct SceneRevisionDesc {
    uint64_t revision = 0;
    std::chrono::system_clock::time_point published_at{};
    std::string author;
};

enum class RendererKind {
    Software2D,
    Metal2D,
    Vulkan2D,
};

struct RendererParams {
    std::string name;
    RendererKind kind = RendererKind::Software2D;
    std::string description;
};
struct SurfaceParams {
    std::string name;
    SurfaceDesc desc;
    std::string renderer; // name, app-relative, or absolute path
};

struct HtmlTargetParams {
    std::string name;
    HtmlTargetDesc desc;
    std::string scene; // app-relative scene path (e.g., "scenes/main")
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
        uint32_t flags   = 0;
        bool     enabled = false;

        static constexpr std::uint32_t kForceShapedText = 1u << 6;
        static constexpr std::uint32_t kDisableTextFallback = 1u << 7;
    } debug;

    struct RendererState {
        RendererKind backend_kind = RendererKind::Software2D;
        bool metal_uploads_enabled = false;
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

auto resolve_app_relative(AppRootPathView root,
                          UnvalidatedPathView maybeRelative) -> SP::Expected<ConcretePath>;

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> SP::Expected<ConcretePath>;

namespace Scene {

struct HitTestRequest {
    float x = 0.0f;
    float y = 0.0f;
    std::size_t max_results = 8;
    bool schedule_render = false;
    std::optional<ConcretePath> auto_render_target;
};

struct HitDrawable {
    std::uint64_t drawable_id = 0;
    std::string authoring_node_id;
    std::uint32_t drawable_index_within_node = 0;
    std::uint32_t generation = 0;
};

enum class DirtyKind : std::uint32_t {
    None       = 0,
    Structure  = 1u << 0,
    Layout     = 1u << 1,
    Transform  = 1u << 2,
    Visual     = 1u << 3,
    Text       = 1u << 4,
    Batch      = 1u << 5,
    All        = (1u << 6) - 1,
};

[[nodiscard]] inline constexpr DirtyKind operator|(DirtyKind lhs, DirtyKind rhs) {
    return static_cast<DirtyKind>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] inline constexpr DirtyKind operator&(DirtyKind lhs, DirtyKind rhs) {
    return static_cast<DirtyKind>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

inline constexpr DirtyKind& operator|=(DirtyKind& lhs, DirtyKind rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline constexpr DirtyKind& operator&=(DirtyKind& lhs, DirtyKind rhs) {
    lhs = lhs & rhs;
    return lhs;
}

struct DirtyState {
    std::uint64_t sequence = 0;
    DirtyKind pending = DirtyKind::None;
    std::int64_t timestamp_ms = 0;
};

struct DirtyEvent {
    std::uint64_t sequence = 0;
    DirtyKind kinds = DirtyKind::None;
    std::int64_t timestamp_ms = 0;
};

struct HitPosition {
    float scene_x = 0.0f;
   float scene_y = 0.0f;
    float local_x = 0.0f;
    float local_y = 0.0f;
    bool  has_local = false;
};

struct FocusEntry {
    std::string path;
    bool focusable = false;
};

struct HitCandidate {
    HitDrawable target{};
    HitPosition position{};
    std::vector<std::string> focus_chain;
    std::vector<FocusEntry> focus_path;
};

struct HitTestResult {
    bool hit = false;
    HitDrawable target{};
    HitPosition position{};
    std::vector<std::string> focus_chain;
    std::vector<FocusEntry> focus_path;
    std::vector<HitCandidate> hits;
};

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

auto HitTest(PathSpace& space,
             ScenePath const& scenePath,
             HitTestRequest const& request) -> SP::Expected<HitTestResult>;

auto MarkDirty(PathSpace& space,
               ScenePath const& scenePath,
               DirtyKind kinds,
               std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now())
    -> SP::Expected<std::uint64_t>;

auto ClearDirty(PathSpace& space,
                ScenePath const& scenePath,
                DirtyKind kinds) -> SP::Expected<void>;

auto ReadDirtyState(PathSpace const& space,
                    ScenePath const& scenePath) -> SP::Expected<DirtyState>;

auto TakeDirtyEvent(PathSpace& space,
                    ScenePath const& scenePath,
                    std::chrono::milliseconds timeout) -> SP::Expected<DirtyEvent>;

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& space,
            AppRootPathView appRoot,
            RendererParams const& params) -> SP::Expected<RendererPath>;

auto ResolveTargetBase(PathSpace const& space,
                        AppRootPathView appRoot,
                        RendererPath const& rendererPath,
                        std::string_view targetSpec) -> SP::Expected<ConcretePath>;

auto UpdateSettings(PathSpace& space,
                     ConcretePathView targetPath,
                     RenderSettings const& settings) -> SP::Expected<void>;

auto ReadSettings(PathSpace const& space,
                   ConcretePathView targetPath) -> SP::Expected<RenderSettings>;

auto SubmitDirtyRects(PathSpace& space,
                      ConcretePathView targetPath,
                      std::span<DirtyRectHint const> rects) -> SP::Expected<void>;

auto TriggerRender(PathSpace& space,
                   ConcretePathView targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny>;

auto CreateHtmlTarget(PathSpace& space,
                      AppRootPathView appRoot,
                      RendererPath const& rendererPath,
                      HtmlTargetParams const& params) -> SP::Expected<HtmlTargetPath>;

auto RenderHtml(PathSpace& space,
                ConcretePathView targetPath) -> SP::Expected<void>;

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

struct WindowPresentResult {
    PathWindowPresentStats stats;
    std::vector<std::uint8_t> framebuffer;
    struct HtmlPayload {
        uint64_t revision = 0;
        std::string dom;
        std::string css;
        std::string commands;
        std::string mode;
        bool used_canvas_fallback = false;
        std::vector<Html::Asset> assets;
    };
    std::optional<HtmlPayload> html;
};

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             WindowParams const& params) -> SP::Expected<WindowPath>;

auto AttachSurface(PathSpace& space,
                    WindowPath const& windowPath,
                    std::string_view viewName,
                    SurfacePath const& surfacePath) -> SP::Expected<void>;

auto AttachHtmlTarget(PathSpace& space,
                       WindowPath const& windowPath,
                       std::string_view viewName,
                       HtmlTargetPath const& targetPath) -> SP::Expected<void>;

auto Present(PathSpace& space,
              WindowPath const& windowPath,
              std::string_view viewName) -> SP::Expected<WindowPresentResult>;

} // namespace Window

auto maybe_schedule_auto_render(PathSpace& space,
                                std::string const& targetPath,
                                PathWindowView::PresentStats const& stats,
                                PathWindowView::PresentPolicy const& policy) -> SP::Expected<bool>;

namespace Window::TestHooks {

using BeforePresentHook = std::function<void(PathSurfaceSoftware&,
                                             PathWindowView::PresentPolicy&,
                                             std::vector<std::size_t>&)>;

void SetBeforePresentHook(BeforePresentHook hook);
void ResetBeforePresentHook();

} // namespace Window::TestHooks

namespace Resources::Fonts {

struct FontResourcePaths {
    ConcretePath root;
    ConcretePath meta;
    ConcretePath active_revision;
    ConcretePath builds;
    ConcretePath inbox;
};

struct RegisterFontParams {
    std::string family;
    std::string style;
    std::string weight = "400";
    std::vector<std::string> fallback_families{};
    std::uint64_t initial_revision = 0;
    std::uint64_t atlas_soft_bytes = 4 * 1024 * 1024;
    std::uint64_t atlas_hard_bytes = 8 * 1024 * 1024;
    std::uint64_t shaped_run_approx_bytes = 512;
};

auto Resolve(AppRootPathView appRoot,
             std::string_view family,
             std::string_view style) -> SP::Expected<FontResourcePaths>;

auto Register(PathSpace& space,
              AppRootPathView appRoot,
              RegisterFontParams const& params) -> SP::Expected<FontResourcePaths>;

} // namespace Resources::Fonts

namespace App {

struct BootstrapParams {
    RendererParams renderer{};
    SurfaceParams surface{};
    WindowParams window{};
    std::string view_name;
    PathWindowView::PresentPolicy present_policy{};
    bool configure_present_policy = true;
    bool configure_renderer_settings = true;
    std::optional<RenderSettings> renderer_settings_override;
    bool submit_initial_dirty_rect = true;
    std::optional<DirtyRectHint> initial_dirty_rect_override;

    BootstrapParams();
};

struct BootstrapResult {
    RendererPath renderer;
    SurfacePath surface;
    ConcretePath target;
    WindowPath window;
    std::string view_name;
    SurfaceDesc surface_desc;
    RenderSettings applied_settings;
    PathWindowView::PresentPolicy present_policy;
};

struct ResizeSurfaceOptions {
    bool update_surface_desc = true;
    bool update_target_desc = true;
    bool update_renderer_settings = true;
    bool submit_dirty_rect = true;
    std::optional<RenderSettings> renderer_settings_override{};
};

auto UpdateSurfaceSize(PathSpace& space,
                       BootstrapResult& bootstrap,
                       int width,
                       int height,
                       ResizeSurfaceOptions const& options = {}) -> SP::Expected<void>;

struct PresentToLocalWindowOptions {
    bool allow_iosurface = true;
    bool allow_framebuffer = true;
    bool warn_when_metal_texture_unshared = true;
};

struct PresentToLocalWindowResult {
    bool presented = false;
    bool skipped = false;
    bool used_iosurface = false;
    bool used_framebuffer = false;
    std::size_t framebuffer_bytes = 0;
    std::size_t row_stride_bytes = 0;
};

auto PresentToLocalWindow(Window::WindowPresentResult const& present,
                          int width,
                          int height,
                          PresentToLocalWindowOptions const& options = {}) -> PresentToLocalWindowResult;

auto Bootstrap(PathSpace& space,
               AppRootPathView appRoot,
               ScenePath const& scene,
               BootstrapParams const& params) -> SP::Expected<BootstrapResult>;

inline App::BootstrapParams::BootstrapParams() {
    renderer.name = "main_renderer";
    renderer.kind = RendererKind::Software2D;
    renderer.description = "bootstrap renderer";

    surface.name = "main_surface";
    surface.desc.size_px.width = 1280;
    surface.desc.size_px.height = 720;
    surface.desc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surface.desc.color_space = ColorSpace::sRGB;
    surface.desc.premultiplied_alpha = true;
    surface.renderer.clear();

    window.name = "main_window";
    window.title = "PathSpace Window";
    window.width = 1280;
    window.height = 720;
    window.scale = 1.0f;
    window.background = "#101218";

    view_name = "main";

    present_policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    present_policy.staleness_budget = std::chrono::milliseconds{0};
    present_policy.staleness_budget_ms_value = 0.0;
    present_policy.max_age_frames = 0;
    present_policy.frame_timeout = std::chrono::milliseconds{0};
    present_policy.frame_timeout_ms_value = 0.0;
    present_policy.vsync_align = false;
    present_policy.auto_render_on_present = true;
    present_policy.capture_framebuffer = false;
}


} // namespace App

namespace Config::Theme {

using ThemePaths = SP::UI::Declarative::ThemeConfig::ThemePaths;

auto SanitizeName(std::string_view theme_name) -> std::string;

auto Resolve(AppRootPathView appRoot,
             std::string_view theme_name) -> SP::Expected<ThemePaths>;

auto Ensure(PathSpace& space,
            AppRootPathView appRoot,
            std::string_view theme_name,
            Widgets::WidgetTheme const& defaults) -> SP::Expected<ThemePaths>;

auto Load(PathSpace& space,
          ThemePaths const& paths) -> SP::Expected<Widgets::WidgetTheme>;

auto SetActive(PathSpace& space,
               AppRootPathView appRoot,
               std::string_view theme_name) -> SP::Expected<void>;

auto LoadActive(PathSpace& space,
                AppRootPathView appRoot) -> SP::Expected<std::string>;

} // namespace Config::Theme

inline auto MakeWidgetBounds(float min_x,
                             float min_y,
                             float max_x,
                             float max_y) -> Widgets::Input::WidgetBounds {
    Widgets::Input::WidgetBounds bounds{min_x, min_y, max_x, max_y};
    bounds.normalize();
    return bounds;
}

namespace Diagnostics {

struct PathSpaceError {
    enum class Severity : std::uint32_t { Info = 0, Warning, Recoverable, Fatal };

    int code = 0;
    Severity severity = Severity::Info;
    std::string message;
    std::string path;
    std::uint64_t revision = 0;
    std::uint64_t timestamp_ns = 0;
    std::string detail;
};

struct TargetMetrics {
    uint64_t frame_index = 0;
    uint64_t revision = 0;
    double render_ms = 0.0;
    double present_ms = 0.0;
    double gpu_encode_ms = 0.0;
    double gpu_present_ms = 0.0;
    double progressive_copy_ms = 0.0;
    bool last_present_skipped = false;
    bool used_metal_texture = false;
    bool presented = false;
    bool buffered_frame_consumed = false;
    bool used_progressive = false;
    bool stale = false;
    std::string backend_kind;
    std::string present_mode;
    double wait_budget_ms = 0.0;
    double staleness_budget_ms = 0.0;
    double frame_timeout_ms = 0.0;
    std::uint64_t max_age_frames = 0;
    bool auto_render_on_present = false;
    bool vsync_align = false;
    std::string last_error;
    int last_error_code = 0;
    uint64_t last_error_revision = 0;
    PathSpaceError::Severity last_error_severity = PathSpaceError::Severity::Info;
    uint64_t last_error_timestamp_ns = 0;
    std::string last_error_detail;
    double frame_age_ms = 0.0;
    uint64_t frame_age_frames = 0;
    uint64_t drawable_count = 0;
    uint64_t progressive_tiles_updated = 0;
    uint64_t progressive_bytes_copied = 0;
    uint64_t progressive_tile_size = 0;
    uint64_t progressive_workers_used = 0;
    uint64_t progressive_jobs = 0;
    uint64_t encode_workers_used = 0;
    uint64_t encode_jobs = 0;
    bool progressive_tile_diagnostics_enabled = false;
    uint64_t progressive_tiles_copied = 0;
    uint64_t progressive_tiles_dirty = 0;
    uint64_t progressive_tiles_total = 0;
    uint64_t progressive_tiles_skipped = 0;
    uint64_t progressive_rects_coalesced = 0;
    uint64_t progressive_skip_seq_odd = 0;
    uint64_t progressive_recopy_after_seq_change = 0;
    uint64_t material_count = 0;
    std::vector<MaterialDescriptor> materials;
    uint64_t material_resource_count = 0;
    std::vector<MaterialResourceResidency> material_resources;
    // Residency metrics are optional; zero indicates unavailable.
    std::uint64_t cpu_bytes = 0;
    std::uint64_t cpu_soft_bytes = 0;
    std::uint64_t cpu_hard_bytes = 0;
    std::uint64_t gpu_bytes = 0;
    std::uint64_t gpu_soft_bytes = 0;
    std::uint64_t gpu_hard_bytes = 0;
    double cpu_soft_budget_ratio = 0.0;
    double cpu_hard_budget_ratio = 0.0;
    double gpu_soft_budget_ratio = 0.0;
    double gpu_hard_budget_ratio = 0.0;
    bool cpu_soft_exceeded = false;
    bool cpu_hard_exceeded = false;
    bool gpu_soft_exceeded = false;
    bool gpu_hard_exceeded = false;
    std::string cpu_residency_status;
    std::string gpu_residency_status;
    std::string residency_overall_status;
};

auto ReadTargetMetrics(PathSpace const& space,
                        ConcretePathView targetPath) -> SP::Expected<TargetMetrics>;

auto ClearTargetError(PathSpace& space,
                      ConcretePathView targetPath) -> SP::Expected<void>;

auto WriteTargetError(PathSpace& space,
                      ConcretePathView targetPath,
                      PathSpaceError const& error) -> SP::Expected<void>;

auto ReadTargetError(PathSpace const& space,
                     ConcretePathView targetPath) -> SP::Expected<std::optional<PathSpaceError>>;

auto ReadSoftwareFramebuffer(PathSpace const& space,
                              ConcretePathView targetPath) -> SP::Expected<SoftwareFramebuffer>;

auto WritePresentMetrics(PathSpace& space,
                          ConcretePathView targetPath,
                          PathWindowPresentStats const& stats,
                          PathWindowPresentPolicy const& policy) -> SP::Expected<void>;

auto WriteWindowPresentMetrics(PathSpace& space,
                               ConcretePathView windowPath,
                               std::string_view viewName,
                               PathWindowPresentStats const& stats,
                               PathWindowPresentPolicy const& policy) -> SP::Expected<void>;

auto WriteResidencyMetrics(PathSpace& space,
                           ConcretePathView targetPath,
                           std::uint64_t cpu_bytes,
                           std::uint64_t gpu_bytes,
                           std::uint64_t cpu_soft_bytes = 0,
                           std::uint64_t cpu_hard_bytes = 0,
                           std::uint64_t gpu_soft_bytes = 0,
                           std::uint64_t gpu_hard_bytes = 0) -> SP::Expected<void>;

} // namespace Diagnostics

} // namespace SP::UI::Builders
