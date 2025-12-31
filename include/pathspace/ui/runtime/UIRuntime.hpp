#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/task/Future.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/HtmlSerialization.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>
#include <pathspace/ui/FontAtlas.hpp>

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
#include <pathspace/ui/runtime/TextRuntime.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>

namespace SP::UI::Runtime {

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
    bool emit_color_atlas = true;
    FontAtlasFormat preferred_atlas_format = FontAtlasFormat::Alpha8;
};

auto Resolve(AppRootPathView appRoot,
             std::string_view family,
             std::string_view style) -> SP::Expected<FontResourcePaths>;

auto Register(PathSpace& space,
              AppRootPathView appRoot,
              RegisterFontParams const& params) -> SP::Expected<FontResourcePaths>;

auto EnsureBuiltInPack(PathSpace& space,
                       AppRootPathView appRoot) -> SP::Expected<void>;

} // namespace Resources::Fonts

namespace App {

struct BootstrapParams {
    RendererParams renderer{};
    SurfaceParams surface{};
    WindowParams window{};
    std::string view_name;
    PathWindowView::PresentPolicy present_policy{};
    bool configure_present_policy = false;
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

struct ErrorStats {
    std::uint64_t total = 0;
    std::uint64_t cleared = 0;
    std::uint64_t info = 0;
    std::uint64_t warning = 0;
    std::uint64_t recoverable = 0;
    std::uint64_t fatal = 0;
    std::uint64_t last_code = 0;
    PathSpaceError::Severity last_severity = PathSpaceError::Severity::Info;
    std::uint64_t last_timestamp_ns = 0;
    std::uint64_t last_revision = 0;
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
    uint64_t error_total = 0;
    uint64_t error_cleared = 0;
    uint64_t error_info = 0;
    uint64_t error_warning = 0;
    uint64_t error_recoverable = 0;
    uint64_t error_fatal = 0;
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
    double encode_worker_stall_ms_total = 0.0;
    double encode_worker_stall_ms_max = 0.0;
    uint64_t encode_worker_stall_workers = 0;
    uint64_t tiles_total = 0;
    uint64_t tiles_dirty = 0;
    uint64_t tiles_rendered = 0;
    uint64_t tile_jobs = 0;
    uint64_t tile_workers_used = 0;
    uint32_t tile_width_px = 0;
    uint32_t tile_height_px = 0;
    bool tiled_renderer_used = false;
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
    uint64_t font_active_count = 0;
    uint64_t font_atlas_cpu_bytes = 0;
    uint64_t font_atlas_gpu_bytes = 0;
    uint64_t font_atlas_resource_count = 0;
    std::vector<SP::UI::Scene::FontAssetReference> font_assets;
    uint64_t font_registered_fonts = 0;
    uint64_t font_cache_hits = 0;
    uint64_t font_cache_misses = 0;
    uint64_t font_cache_evictions = 0;
    uint64_t font_cache_size = 0;
    uint64_t font_cache_capacity = 0;
    uint64_t font_cache_hard_capacity = 0;
    uint64_t font_atlas_soft_bytes = 0;
    uint64_t font_atlas_hard_bytes = 0;
    uint64_t font_shaped_run_approx_bytes = 0;
    // HTML adapter metrics
    uint64_t html_dom_node_count = 0;
    uint64_t html_command_count = 0;
    uint64_t html_asset_count = 0;
    uint64_t html_max_dom_nodes = 0;
    bool html_used_canvas_fallback = false;
    bool html_prefer_dom = true;
    bool html_allow_canvas_fallback = true;
    std::string html_mode;
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

struct TargetDiagnosticsSummary {
    std::string path;
    std::string renderer;
    std::string target;
    TargetMetrics metrics;
    std::optional<PathSpaceError> live_error;
    ErrorStats error_stats;
};

auto ReadTargetMetrics(PathSpace const& space,
                        ConcretePathView targetPath) -> SP::Expected<TargetMetrics>;

auto ReadTargetErrorStats(PathSpace const& space,
                          ConcretePathView targetPath) -> SP::Expected<ErrorStats>;

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

auto CollectTargetDiagnostics(PathSpace& space,
                              std::string_view renderers_root = "/renderers")
    -> SP::Expected<std::vector<TargetDiagnosticsSummary>>;

} // namespace Diagnostics

} // namespace SP::UI::Runtime

#if !PATHSPACE_ENABLE_UI
// Stub for UI-disabled builds so tools linking unconditionally still succeed.
// Default argument is only specified on the primary declaration above to avoid
// duplicate default-arg errors when UI is disabled.
namespace SP::UI::Runtime::Diagnostics {
inline auto CollectTargetDiagnostics(PathSpace&, std::string_view renderers_root)
    -> SP::Expected<std::vector<TargetDiagnosticsSummary>> {
    return std::vector<TargetDiagnosticsSummary>{};
}
} // namespace SP::UI::Runtime::Diagnostics
#endif
