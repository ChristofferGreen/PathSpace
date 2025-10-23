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
#include <limits>
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

namespace Widgets {

struct TypographyStyle {
    float font_size = 28.0f;
    float line_height = 28.0f;
    float letter_spacing = 1.0f;
    float baseline_shift = 0.0f;
};

struct ButtonStyle {
    float width = 200.0f;
    float height = 48.0f;
    float corner_radius = 6.0f;
    std::array<float, 4> background_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> text_color{1.0f, 1.0f, 1.0f, 1.0f};
    TypographyStyle typography{};
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

struct WidgetStateScenes {
    ScenePath idle;
    ScenePath hover;
    ScenePath pressed;
    ScenePath disabled;
};

struct ButtonPaths {
    ScenePath scene;
    WidgetStateScenes states;
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
    WidgetStateScenes states;
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

struct SliderStyle {
    float width = 240.0f;
    float height = 32.0f;
    float track_height = 6.0f;
    float thumb_radius = 10.0f;
    std::array<float, 4> track_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> fill_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> label_color{0.90f, 0.92f, 0.96f, 1.0f};
    TypographyStyle label_typography{
        .font_size = 24.0f,
        .line_height = 28.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
};

struct SliderState {
    bool enabled = true;
    bool hovered = false;
    bool dragging = false;
    float value = 0.0f;
};

struct SliderParams {
    std::string name;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float value = 0.5f;
    float step = 0.0f; // 0 => continuous
    SliderStyle style{};
};

struct SliderRange {
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.0f;
};

struct SliderPaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
    ConcretePath range;
};

auto CreateSlider(PathSpace& space,
                  AppRootPathView appRoot,
                  SliderParams const& params) -> SP::Expected<SliderPaths>;

auto UpdateSliderState(PathSpace& space,
                       SliderPaths const& paths,
                       SliderState const& new_state) -> SP::Expected<bool>;

struct ListStyle {
    float width = 240.0f;
    float item_height = 36.0f;
    float corner_radius = 8.0f;
    float border_thickness = 1.0f;
    std::array<float, 4> background_color{0.121f, 0.129f, 0.145f, 1.0f};
    std::array<float, 4> border_color{0.239f, 0.247f, 0.266f, 1.0f};
    std::array<float, 4> item_color{0.176f, 0.184f, 0.204f, 1.0f};
    std::array<float, 4> item_hover_color{0.247f, 0.278f, 0.349f, 1.0f};
    std::array<float, 4> item_selected_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> separator_color{0.224f, 0.231f, 0.247f, 1.0f};
    std::array<float, 4> item_text_color{0.94f, 0.96f, 0.99f, 1.0f};
    TypographyStyle item_typography{
        .font_size = 21.0f,
        .line_height = 24.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
};

struct ListItem {
    std::string id;
    std::string label;
    bool enabled = true;
};

struct ListState {
    bool enabled = true;
    std::int32_t hovered_index = -1;
    std::int32_t selected_index = -1;
    float scroll_offset = 0.0f;
};

struct ListParams {
    std::string name;
    std::vector<ListItem> items;
    ListStyle style{};
};

struct ListPaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
    ConcretePath items;
};

auto CreateList(PathSpace& space,
                AppRootPathView appRoot,
                ListParams const& params) -> SP::Expected<ListPaths>;

auto UpdateListState(PathSpace& space,
                     ListPaths const& paths,
                     ListState const& new_state) -> SP::Expected<bool>;

enum class StackAxis : std::uint8_t {
    Horizontal = 0,
    Vertical = 1,
};

enum class StackAlignMain : std::uint8_t {
    Start = 0,
    Center = 1,
    End = 2,
};

enum class StackAlignCross : std::uint8_t {
    Start = 0,
    Center = 1,
    End = 2,
    Stretch = 3,
};

struct StackChildConstraints {
    float weight = 0.0f;
    float min_main = 0.0f;
    float max_main = 0.0f;
    float min_cross = 0.0f;
    float max_cross = 0.0f;
    float margin_main_start = 0.0f;
    float margin_main_end = 0.0f;
    float margin_cross_start = 0.0f;
    float margin_cross_end = 0.0f;
    bool has_min_main = false;
    bool has_max_main = false;
    bool has_min_cross = false;
    bool has_max_cross = false;
};

struct StackChildSpec {
    std::string id;
    std::string widget_path;
    std::string scene_path;
    StackChildConstraints constraints{};
};

struct StackLayoutStyle {
    StackAxis axis = StackAxis::Vertical;
    float spacing = 16.0f;
    StackAlignMain align_main = StackAlignMain::Start;
    StackAlignCross align_cross = StackAlignCross::Stretch;
    float padding_main_start = 0.0f;
    float padding_main_end = 0.0f;
    float padding_cross_start = 0.0f;
    float padding_cross_end = 0.0f;
    float width = 0.0f;  // 0 => derive from children
    float height = 0.0f; // 0 => derive from children
    bool clip_contents = false;
};

struct StackLayoutComputedChild {
    std::string id;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct StackLayoutState {
    float width = 0.0f;
    float height = 0.0f;
    std::vector<StackLayoutComputedChild> children;
};

struct StackLayoutParams {
    std::string name;
    StackLayoutStyle style{};
    std::vector<StackChildSpec> children;
};

struct StackPaths {
    ScenePath scene;
    WidgetPath root;
    ConcretePath style;
    ConcretePath children;
    ConcretePath computed;
};

auto CreateStack(PathSpace& space,
                 AppRootPathView appRoot,
                 StackLayoutParams const& params) -> SP::Expected<StackPaths>;

auto UpdateStackLayout(PathSpace& space,
                       StackPaths const& paths,
                       StackLayoutParams const& params) -> SP::Expected<bool>;

auto DescribeStack(PathSpace const& space,
                   StackPaths const& paths) -> SP::Expected<StackLayoutParams>;

auto ReadStackLayout(PathSpace const& space,
                     StackPaths const& paths) -> SP::Expected<StackLayoutState>;

enum class WidgetKind {
    Button,
    Toggle,
    Slider,
    List,
};

struct HitTarget {
    WidgetPath widget;
    std::string component;
};

auto ResolveHitTarget(Scene::HitTestResult const& hit) -> std::optional<HitTarget>;

namespace Bindings {

enum class WidgetOpKind : std::uint32_t {
    HoverEnter = 0,
    HoverExit,
    Press,
    Release,
    Activate,
    Toggle,
    SliderBegin,
    SliderUpdate,
    SliderCommit,
    ListHover,
    ListSelect,
    ListActivate,
    ListScroll,
};

struct PointerInfo {
    float scene_x = 0.0f;
    float scene_y = 0.0f;
    bool inside = false;
    bool primary = true;
};

struct WidgetOp {
    WidgetOpKind kind = WidgetOpKind::HoverEnter;
    std::string widget_path;
    PointerInfo pointer;
    float value = 0.0f;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
};

struct BindingOptions {
    ConcretePath target;
    ConcretePath ops_queue;
    DirtyRectHint dirty_rect;
    bool auto_render = true;
};

struct ButtonBinding {
    ButtonPaths widget;
    BindingOptions options;
};

struct ToggleBinding {
    TogglePaths widget;
    BindingOptions options;
};

struct SliderBinding {
    SliderPaths widget;
    BindingOptions options;
};

struct ListBinding {
    ListPaths widget;
    BindingOptions options;
};

struct StackBinding {
    StackPaths layout;
    BindingOptions options;
};

auto CreateButtonBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         ButtonPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<ButtonBinding>;

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<ToggleBinding>;

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<SliderBinding>;

auto DispatchButton(PathSpace& space,
                    ButtonBinding const& binding,
                    ButtonState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer = {}) -> SP::Expected<bool>;

auto DispatchToggle(PathSpace& space,
                    ToggleBinding const& binding,
                    ToggleState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer = {}) -> SP::Expected<bool>;

auto DispatchSlider(PathSpace& space,
                    SliderBinding const& binding,
                    SliderState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer = {}) -> SP::Expected<bool>;

auto CreateListBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       ListPaths const& paths,
                       ConcretePathView targetPath,
                       std::optional<DirtyRectHint> dirty_override = std::nullopt,
                       bool auto_render = true) -> SP::Expected<ListBinding>;

auto CreateStackBinding(PathSpace& space,
                        AppRootPathView appRoot,
                        StackPaths const& paths,
                        ConcretePathView targetPath,
                        std::optional<DirtyRectHint> dirty_override = std::nullopt,
                        bool auto_render = true) -> SP::Expected<StackBinding>;

auto DispatchList(PathSpace& space,
                  ListBinding const& binding,
                  ListState const& new_state,
                  WidgetOpKind op_kind,
                  PointerInfo const& pointer = {},
                  std::int32_t item_index = -1,
                  float scroll_delta = 0.0f) -> SP::Expected<bool>;

auto UpdateStack(PathSpace& space,
                 StackBinding const& binding,
                 StackLayoutParams const& params) -> SP::Expected<bool>;

auto PointerFromHit(Scene::HitTestResult const& hit) -> PointerInfo;

} // namespace Bindings

namespace Focus {

enum class Direction { Forward, Backward };

struct Config {
    ConcretePath focus_state;
    std::optional<ConcretePath> auto_render_target;
};

struct UpdateResult {
    WidgetPath widget;
    bool changed = false;
};

auto FocusStatePath(AppRootPathView appRoot) -> ConcretePath;

auto MakeConfig(AppRootPathView appRoot,
                std::optional<ConcretePath> auto_render_target = std::nullopt) -> Config;

auto Current(PathSpace const& space,
             ConcretePathView focus_state) -> SP::Expected<std::optional<std::string>>;

auto Set(PathSpace& space,
         Config const& config,
         WidgetPath const& widget) -> SP::Expected<UpdateResult>;

auto Clear(PathSpace& space,
           Config const& config) -> SP::Expected<bool>;

auto Move(PathSpace& space,
          Config const& config,
          std::span<WidgetPath const> order,
          Direction direction) -> SP::Expected<std::optional<UpdateResult>>;

auto ApplyHit(PathSpace& space,
              Config const& config,
              Scene::HitTestResult const& hit) -> SP::Expected<std::optional<UpdateResult>>;

} // namespace Focus

struct WidgetTheme {
    ButtonStyle button{};
    ToggleStyle toggle{};
    SliderStyle slider{};
    ListStyle list{};
    TypographyStyle heading{
        .font_size = 32.0f,
        .line_height = 36.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
    TypographyStyle caption{
        .font_size = 24.0f,
        .line_height = 28.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
    std::array<float, 4> heading_color{0.93f, 0.95f, 0.98f, 1.0f};
    std::array<float, 4> caption_color{0.90f, 0.92f, 0.96f, 1.0f};
    std::array<float, 4> accent_text_color{0.85f, 0.88f, 0.95f, 1.0f};
    std::array<float, 4> muted_text_color{0.70f, 0.72f, 0.78f, 1.0f};
};

auto MakeDefaultWidgetTheme() -> WidgetTheme;
auto MakeSunsetWidgetTheme() -> WidgetTheme;
auto ApplyTheme(WidgetTheme const& theme, ButtonParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, ToggleParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, SliderParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, ListParams& params) -> void;

namespace Reducers {

struct WidgetAction {
    Bindings::WidgetOpKind kind = Bindings::WidgetOpKind::HoverEnter;
    std::string widget_path;
    Bindings::PointerInfo pointer{};
    float analog_value = 0.0f;
    std::int32_t discrete_index = -1;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
};

auto WidgetOpsQueue(WidgetPath const& widget_root) -> ConcretePath;

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath;

auto ReducePending(PathSpace& space,
                   ConcretePathView ops_queue,
                   std::size_t max_actions = std::numeric_limits<std::size_t>::max()) -> SP::Expected<std::vector<WidgetAction>>;

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void>;

} // namespace Reducers

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
