#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/task/Future.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/HtmlSerialization.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SurfaceTypes.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
using HtmlTargetPath = ConcretePath;
using WidgetPath = ConcretePath;

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

struct DirtyRectHint {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

enum class ParamUpdateMode {
    Queue,
    ReplaceActive,
};

struct AutoRenderRequestEvent {
    std::uint64_t sequence = 0;
    std::string reason;
    std::uint64_t frame_index = 0;
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

struct HitTestResult {
    bool hit = false;
    HitDrawable target{};
    HitPosition position{};
    std::vector<std::string> focus_chain;
    std::vector<FocusEntry> focus_path;
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

namespace Window::TestHooks {

using BeforePresentHook = std::function<void(PathSurfaceSoftware&,
                                             PathWindowView::PresentPolicy&,
                                             std::vector<std::size_t>&)>;

void SetBeforePresentHook(BeforePresentHook hook);
void ResetBeforePresentHook();

} // namespace Window::TestHooks

namespace Widgets {

struct ButtonStyle {
    float width = 200.0f;
    float height = 48.0f;
    std::array<float, 4> background_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> text_color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct ButtonState {
    bool enabled = true;
    bool pressed = false;
    bool hovered = false;
};

struct ButtonParams {
    std::string name;
    std::string label;
    ButtonStyle style{};
};

struct ButtonPaths {
    ScenePath scene;
    WidgetPath root;
    ConcretePath state;
    ConcretePath label;
};

auto CreateButton(PathSpace& space,
                  AppRootPathView appRoot,
                  ButtonParams const& params) -> SP::Expected<ButtonPaths>;

struct ToggleStyle {
    float width = 56.0f;
    float height = 32.0f;
    std::array<float, 4> track_off_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> track_on_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct ToggleState {
    bool enabled = true;
    bool hovered = false;
    bool checked = false;
};

struct ToggleParams {
    std::string name;
    ToggleStyle style{};
};

struct TogglePaths {
    ScenePath scene;
    WidgetPath root;
    ConcretePath state;
};

auto CreateToggle(PathSpace& space,
                  AppRootPathView appRoot,
                  ToggleParams const& params) -> SP::Expected<TogglePaths>;

auto UpdateButtonState(PathSpace& space,
                       ButtonPaths const& paths,
                       ButtonState const& new_state) -> SP::Expected<bool>;

auto UpdateToggleState(PathSpace& space,
                       TogglePaths const& paths,
                       ToggleState const& new_state) -> SP::Expected<bool>;

} // namespace Widgets

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
    bool last_present_skipped = false;
    bool used_metal_texture = false;
    std::string backend_kind;
    std::string last_error;
    int last_error_code = 0;
    uint64_t last_error_revision = 0;
    PathSpaceError::Severity last_error_severity = PathSpaceError::Severity::Info;
    uint64_t last_error_timestamp_ns = 0;
    std::string last_error_detail;
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
