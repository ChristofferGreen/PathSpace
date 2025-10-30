#pragma once

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

namespace Text {
struct BuildResult;
} // namespace Text

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

[[nodiscard]] inline auto MakeDirtyRectHint(float min_x,
                                            float min_y,
                                            float max_x,
                                            float max_y) -> DirtyRectHint {
    return DirtyRectHint{min_x, min_y, max_x, max_y};
}

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

namespace Widgets {

namespace Reducers {
struct WidgetAction;
} // namespace Reducers

namespace Input {
struct WidgetBounds;
}

struct TypographyStyle {
    float font_size = 28.0f;
    float line_height = 28.0f;
    float letter_spacing = 1.0f;
    float baseline_shift = 0.0f;
    std::string font_family = "system-ui";
    std::string font_style = "normal";
    std::string font_weight = "400";
    std::string language = "en";
    std::string direction = "ltr";
    std::vector<std::string> fallback_families{};
    std::vector<std::string> font_features{};
    std::string font_resource_root;
    std::uint64_t font_active_revision = 0;
    std::uint64_t font_asset_fingerprint = 0;
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
    bool focused = false;
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
    bool focused = false;
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

struct ButtonPreviewOptions {
    std::string authoring_root;
    bool pulsing_highlight = true;
};

auto BuildButtonPreview(ButtonStyle const& style,
                        ButtonState const& state,
                        ButtonPreviewOptions const& options = {}) -> SP::UI::Scene::DrawableBucketSnapshot;

struct LabelBuildParams {
    std::string text;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    TypographyStyle typography{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint64_t drawable_id = 0;
    std::string authoring_id;
    float z_value = 0.0f;

    [[nodiscard]] static auto Make(std::string text_value, TypographyStyle typography_value) -> LabelBuildParams {
        LabelBuildParams params{};
        params.text = std::move(text_value);
        params.typography = std::move(typography_value);
        return params;
    }

    auto WithOrigin(float x, float y) & -> LabelBuildParams& {
        origin_x = x;
        origin_y = y;
        return *this;
    }

    auto WithOrigin(float x, float y) && -> LabelBuildParams {
        origin_x = x;
        origin_y = y;
        return std::move(*this);
    }

    auto WithColor(std::array<float, 4> value) & -> LabelBuildParams& {
        color = value;
        return *this;
    }

    auto WithColor(std::array<float, 4> value) && -> LabelBuildParams {
        color = value;
        return std::move(*this);
    }

    auto WithDrawable(std::uint64_t id, std::string authoring, float z = 0.0f) & -> LabelBuildParams& {
        drawable_id = id;
        authoring_id = std::move(authoring);
        z_value = z;
        return *this;
    }

    auto WithDrawable(std::uint64_t id, std::string authoring, float z = 0.0f) && -> LabelBuildParams {
        drawable_id = id;
        authoring_id = std::move(authoring);
        z_value = z;
        return std::move(*this);
    }
};

auto BuildLabel(LabelBuildParams const& params) -> std::optional<Text::BuildResult>;

auto LabelBounds(Text::BuildResult const& result) -> std::optional<Input::WidgetBounds>;

inline auto MakeMouseEvent(SP::MouseEventType type,
                           int x = 0,
                           int y = 0,
                           SP::MouseButton button = SP::MouseButton::Left,
                           int dx = 0,
                           int dy = 0,
                           int wheel = 0) -> SP::PathIOMouse::Event {
    SP::PathIOMouse::Event event{};
    event.type = type;
    event.x = x;
    event.y = y;
    event.dx = dx;
    event.dy = dy;
    event.button = button;
    event.wheel = wheel;
    return event;
}

inline auto MakeLocalKeyEvent(SP::UI::LocalKeyEventType type,
                              unsigned int keycode,
                              unsigned int modifiers,
                              char32_t character,
                              bool repeat) -> SP::UI::LocalKeyEvent {
    SP::UI::LocalKeyEvent event{};
    event.type = type;
    event.keycode = keycode;
    event.modifiers = modifiers;
    event.character = character;
    event.repeat = repeat;
    return event;
}

struct TogglePreviewOptions {
    std::string authoring_root;
    bool pulsing_highlight = true;
};

auto BuildTogglePreview(ToggleStyle const& style,
                        ToggleState const& state,
                        TogglePreviewOptions const& options = {}) -> SP::UI::Scene::DrawableBucketSnapshot;

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
    bool focused = false;
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

struct SliderPreviewOptions {
    std::string authoring_root;
    bool pulsing_highlight = true;
};

auto BuildSliderPreview(SliderStyle const& style,
                        SliderRange const& range,
                        SliderState const& state,
                        SliderPreviewOptions const& options = {}) -> SP::UI::Scene::DrawableBucketSnapshot;

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
    bool focused = false;
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

struct ListPreviewRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    [[nodiscard]] auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }
};

struct ListPreviewRowLayout {
    std::string id;
    bool enabled = true;
    bool hovered = false;
    bool selected = false;
    ListPreviewRect row_bounds{};
    ListPreviewRect label_bounds{};
    float label_baseline = 0.0f;
};

struct ListPreviewLayout {
    ListPreviewRect bounds{};
    float content_top = 0.0f;
    float item_height = 0.0f;
    float border_thickness = 0.0f;
    float label_inset = 0.0f;
    ListStyle style{};
    ListState state{};
    std::vector<ListPreviewRowLayout> rows;
};

struct ListPreviewOptions {
    std::string authoring_root;
    float label_inset = 16.0f;
    bool pulsing_highlight = true;
};

struct ListPreviewResult {
    SP::UI::Scene::DrawableBucketSnapshot bucket;
    ListPreviewLayout layout;
};

auto BuildListPreview(ListStyle const& style,
                      std::span<ListItem const> items,
                      ListState const& state,
                      ListPreviewOptions const& options = {}) -> ListPreviewResult;

struct TreeStyle {
    float width = 280.0f;
    float row_height = 32.0f;
    float corner_radius = 8.0f;
    float border_thickness = 1.0f;
    float indent_per_level = 18.0f;
    float toggle_icon_size = 12.0f;
    std::array<float, 4> background_color{0.121f, 0.129f, 0.145f, 1.0f};
    std::array<float, 4> border_color{0.239f, 0.247f, 0.266f, 1.0f};
    std::array<float, 4> row_color{0.176f, 0.184f, 0.204f, 1.0f};
    std::array<float, 4> row_hover_color{0.247f, 0.278f, 0.349f, 1.0f};
    std::array<float, 4> row_selected_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> row_disabled_color{0.145f, 0.149f, 0.162f, 1.0f};
    std::array<float, 4> connector_color{0.224f, 0.231f, 0.247f, 1.0f};
    std::array<float, 4> toggle_color{0.90f, 0.92f, 0.96f, 1.0f};
    std::array<float, 4> text_color{0.94f, 0.96f, 0.99f, 1.0f};
    TypographyStyle label_typography{
        .font_size = 20.0f,
        .line_height = 24.0f,
        .letter_spacing = 0.8f,
        .baseline_shift = 0.0f,
    };
};

struct TreeNode {
    std::string id;
    std::string parent_id;
    std::string label;
    bool enabled = true;
    bool expandable = false;
    bool loaded = true;
};

struct TreeState {
    bool enabled = true;
    bool focused = false;
    std::string hovered_id;
    std::string selected_id;
    std::vector<std::string> expanded_ids;
    std::vector<std::string> loading_ids;
    float scroll_offset = 0.0f;
};

struct TreeParams {
    std::string name;
    std::vector<TreeNode> nodes;
    TreeStyle style{};
};

struct TreePaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
    ConcretePath nodes;
};

auto CreateTree(PathSpace& space,
                AppRootPathView appRoot,
                TreeParams const& params) -> SP::Expected<TreePaths>;

auto UpdateTreeState(PathSpace& space,
                     TreePaths const& paths,
                     TreeState const& new_state) -> SP::Expected<bool>;

struct TreePreviewRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    [[nodiscard]] auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }
};

struct TreePreviewRowLayout {
    std::string id;
    std::string label;
    int depth = 0;
    bool expandable = false;
    bool expanded = false;
    bool loading = false;
    bool enabled = true;
    TreePreviewRect row_bounds{};
    TreePreviewRect toggle_bounds{};
};

struct TreePreviewLayout {
    TreePreviewRect bounds{};
    float content_top = 0.0f;
    float row_height = 0.0f;
    TreeStyle style{};
    TreeState state{};
    std::vector<TreePreviewRowLayout> rows;
};

struct TreePreviewOptions {
    std::string authoring_root;
    bool pulsing_highlight = true;
};

struct TreePreviewResult {
    SP::UI::Scene::DrawableBucketSnapshot bucket;
    TreePreviewLayout layout;
};

auto BuildTreePreview(TreeStyle const& style,
                      std::span<TreeNode const> nodes,
                      TreeState const& state,
                      TreePreviewOptions const& options = {}) -> TreePreviewResult;

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

struct StackPreviewRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    [[nodiscard]] auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }
};

struct StackPreviewLayout {
    StackPreviewRect bounds{};
    StackLayoutStyle style{};
    StackLayoutState state{};
    std::vector<StackPreviewRect> child_bounds;
};

struct StackPreviewOptions {
    std::string authoring_root = "widgets/stack_preview";
    std::array<float, 4> background_color{0.10f, 0.12f, 0.16f, 1.0f};
    std::array<float, 4> child_start_color{0.85f, 0.88f, 0.95f, 1.0f};
    std::array<float, 4> child_end_color{0.93f, 0.95f, 0.98f, 1.0f};
    float child_opacity = 0.85f;
    float mix_scale = 1.0f;
};

struct StackPreviewResult {
    SP::UI::Scene::DrawableBucketSnapshot bucket;
    StackPreviewLayout layout;
};

auto BuildStackPreview(StackLayoutStyle const& style,
                       StackLayoutState const& state,
                       StackPreviewOptions const& options = {}) -> StackPreviewResult;

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
    Stack,
    Tree,
};

struct HitTarget {
    WidgetPath widget;
    std::string component;
};

auto ResolveHitTarget(Scene::HitTestResult const& hit) -> std::optional<HitTarget>;

namespace Bindings {

using WidgetActionCallback = std::function<void(Reducers::WidgetAction const&)>;

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
    TreeHover,
    TreeSelect,
    TreeToggle,
    TreeExpand,
    TreeCollapse,
    TreeRequestLoad,
    TreeScroll,
};

struct PointerInfo {
    float scene_x = 0.0f;
    float scene_y = 0.0f;
    bool inside = false;
    bool primary = true;

    [[nodiscard]] static auto Make(float x, float y) -> PointerInfo {
        PointerInfo info{};
        info.scene_x = x;
        info.scene_y = y;
        return info;
    }

    auto WithInside(bool value) & -> PointerInfo& {
        inside = value;
        return *this;
    }

    auto WithInside(bool value) && -> PointerInfo {
        inside = value;
        return std::move(*this);
    }

    auto WithPrimary(bool value) & -> PointerInfo& {
        primary = value;
        return *this;
    }

    auto WithPrimary(bool value) && -> PointerInfo {
        primary = value;
        return std::move(*this);
    }
};

struct WidgetOp {
    WidgetOpKind kind = WidgetOpKind::HoverEnter;
    std::string widget_path;
    std::string target_id;
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
    ConcretePath focus_state;
    bool focus_enabled = false;
    std::vector<std::shared_ptr<WidgetActionCallback>> action_callbacks;
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

struct TreeBinding {
    TreePaths widget;
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
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<ButtonBinding>;

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<ToggleBinding>;

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
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
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override = std::nullopt,
                       bool auto_render = true) -> SP::Expected<ListBinding>;

auto CreateTreeBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       TreePaths const& paths,
                       ConcretePathView targetPath,
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override = std::nullopt,
                       bool auto_render = true) -> SP::Expected<TreeBinding>;

auto CreateStackBinding(PathSpace& space,
                        AppRootPathView appRoot,
                        StackPaths const& paths,
                        ConcretePathView targetPath,
                        DirtyRectHint footprint,
                        std::optional<DirtyRectHint> dirty_override = std::nullopt,
                        bool auto_render = true) -> SP::Expected<StackBinding>;

namespace ActionCallbacks {

inline auto add_action_callback(BindingOptions& options,
                                WidgetActionCallback callback) -> void {
    if (!callback) {
        return;
    }
    options.action_callbacks.emplace_back(
        std::make_shared<WidgetActionCallback>(std::move(callback)));
}

inline auto clear_action_callbacks(BindingOptions& options) -> void {
    options.action_callbacks.clear();
}

} // namespace ActionCallbacks

inline auto AddActionCallback(ButtonBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(ToggleBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(SliderBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(ListBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(TreeBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto ClearActionCallbacks(ButtonBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(ToggleBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(SliderBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(ListBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(TreeBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

auto DispatchList(PathSpace& space,
                  ListBinding const& binding,
                  ListState const& new_state,
                  WidgetOpKind op_kind,
                  PointerInfo const& pointer = {},
                  std::int32_t item_index = -1,
                  float scroll_delta = 0.0f) -> SP::Expected<bool>;

auto DispatchTree(PathSpace& space,
                  TreeBinding const& binding,
                  TreeState const& new_state,
                  WidgetOpKind op_kind,
                  std::string_view node_id = {},
                  PointerInfo const& pointer = {},
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
    std::optional<bool> pulsing_highlight;
};

struct UpdateResult {
    WidgetPath widget;
    bool changed = false;
};

auto FocusStatePath(AppRootPathView appRoot) -> ConcretePath;

auto MakeConfig(AppRootPathView appRoot,
                std::optional<ConcretePath> auto_render_target = std::nullopt,
                std::optional<bool> pulsing_highlight = std::nullopt) -> Config;

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

auto SetPulsingHighlight(PathSpace& space,
                         AppRootPathView appRoot,
                         bool enabled) -> SP::Expected<void>;

auto PulsingHighlightEnabled(PathSpace& space,
                             AppRootPathView appRoot) -> SP::Expected<bool>;

} // namespace Focus

namespace Input {

struct WidgetBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    constexpr void normalize() {
        if (max_x < min_x) {
            std::swap(max_x, min_x);
        }
        if (max_y < min_y) {
            std::swap(max_y, min_y);
        }
    }

    [[nodiscard]] constexpr auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] constexpr auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }

    [[nodiscard]] constexpr auto contains(float x, float y) const -> bool {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
    }

    constexpr auto include(WidgetBounds const& other) -> void {
        WidgetBounds normalized_other = other;
        normalized_other.normalize();
        if (!std::isfinite(min_x) || !std::isfinite(min_y)
            || !std::isfinite(max_x) || !std::isfinite(max_y)) {
            *this = normalized_other;
            return;
        }
        min_x = std::min(min_x, normalized_other.min_x);
        min_y = std::min(min_y, normalized_other.min_y);
        max_x = std::max(max_x, normalized_other.max_x);
        max_y = std::max(max_y, normalized_other.max_y);
    }

    [[nodiscard]] constexpr auto is_valid() const -> bool {
        return std::isfinite(min_x) && std::isfinite(min_y)
            && std::isfinite(max_x) && std::isfinite(max_y)
            && max_x >= min_x && max_y >= min_y;
    }
};

struct SliderLayout {
    WidgetBounds bounds{};
    WidgetBounds track{};
};

struct ListLayout {
    WidgetBounds bounds{};
    std::vector<WidgetBounds> item_bounds;
    float content_top = 0.0f;
    float item_height = 0.0f;
};

struct TreeRowLayout {
    WidgetBounds bounds{};
    WidgetBounds toggle{};
    std::string node_id;
    std::string label;
    int depth = 0;
    bool expandable = false;
    bool expanded = false;
    bool loading = false;
    bool enabled = true;
};

struct TreeLayout {
    WidgetBounds bounds{};
    float content_top = 0.0f;
    float row_height = 0.0f;
    std::vector<TreeRowLayout> rows;
};

struct LayoutSnapshot {
    WidgetBounds button{};
    WidgetBounds button_footprint{};
    WidgetBounds toggle{};
    WidgetBounds toggle_footprint{};
    std::optional<SliderLayout> slider{};
    WidgetBounds slider_footprint{};
    std::optional<ListLayout> list{};
    WidgetBounds list_footprint{};
    std::optional<TreeLayout> tree{};
    WidgetBounds tree_footprint{};
};

enum class FocusTarget {
    Button,
    Toggle,
    Slider,
    List,
    Tree,
};

struct FocusBindings {
    Focus::Config* config = nullptr;
    FocusTarget* current = nullptr;
    std::span<FocusTarget const> order{};
    std::optional<WidgetPath> button{};
    std::optional<WidgetPath> toggle{};
    std::optional<WidgetPath> slider{};
    std::optional<WidgetPath> list{};
    std::optional<WidgetPath> tree{};
    int* focus_list_index = nullptr;
    int* focus_tree_index = nullptr;
};

struct WidgetInputContext {
    PathSpace* space = nullptr;
    LayoutSnapshot layout{};
    FocusBindings focus{};
    Bindings::ButtonBinding* button_binding = nullptr;
    Widgets::ButtonPaths const* button_paths = nullptr;
    Widgets::ButtonState* button_state = nullptr;
    Bindings::ToggleBinding* toggle_binding = nullptr;
    Widgets::TogglePaths const* toggle_paths = nullptr;
    Widgets::ToggleState* toggle_state = nullptr;
    Bindings::SliderBinding* slider_binding = nullptr;
    Widgets::SliderPaths const* slider_paths = nullptr;
    Widgets::SliderState* slider_state = nullptr;
    Widgets::SliderStyle const* slider_style = nullptr;
    Widgets::SliderRange const* slider_range = nullptr;
    Bindings::ListBinding* list_binding = nullptr;
    Widgets::ListPaths const* list_paths = nullptr;
    Widgets::ListState* list_state = nullptr;
    Widgets::ListStyle const* list_style = nullptr;
    std::vector<Widgets::ListItem>* list_items = nullptr;
    Bindings::TreeBinding* tree_binding = nullptr;
    Widgets::TreePaths const* tree_paths = nullptr;
    Widgets::TreeState* tree_state = nullptr;
    Widgets::TreeStyle const* tree_style = nullptr;
    std::vector<Widgets::TreeNode>* tree_nodes = nullptr;
    float* pointer_x = nullptr;
    float* pointer_y = nullptr;
    bool* pointer_down = nullptr;
    bool* slider_dragging = nullptr;
    std::string* tree_pointer_down_id = nullptr;
    bool* tree_pointer_toggle = nullptr;
};

struct InputUpdate {
    bool state_changed = false;
    bool focus_changed = false;
};

struct SliderStepOptions {
    float percent_of_range = 0.05f;
    float minimum_step = 0.0f;
    bool respect_range_step = true;
};

struct SliderAnalogOptions {
    SliderStepOptions step_options{};
    float deadzone = 0.1f;
    float scale = 1.0f;
};

auto HandlePointerMove(WidgetInputContext& ctx, float x, float y) -> InputUpdate;
auto HandlePointerDown(WidgetInputContext& ctx) -> InputUpdate;
auto HandlePointerUp(WidgetInputContext& ctx) -> InputUpdate;
auto HandlePointerWheel(WidgetInputContext& ctx, int wheel_delta) -> InputUpdate;

auto RefreshFocusTargetFromSpace(WidgetInputContext& ctx) -> bool;
auto SetFocusTarget(WidgetInputContext& ctx,
                    FocusTarget target,
                    bool update_visuals = true) -> InputUpdate;
auto CycleFocus(WidgetInputContext& ctx, bool forward) -> InputUpdate;
auto ActivateFocusedWidget(WidgetInputContext& ctx) -> InputUpdate;
auto MoveListFocus(WidgetInputContext& ctx, int direction) -> InputUpdate;
auto MoveTreeFocus(WidgetInputContext& ctx, int direction) -> InputUpdate;
auto TreeApplyOp(WidgetInputContext& ctx, Bindings::WidgetOpKind op) -> InputUpdate;
auto AdjustSliderValue(WidgetInputContext& ctx, float delta) -> InputUpdate;
auto SliderStep(WidgetInputContext const& ctx,
                SliderStepOptions const& options = {}) -> float;
auto AdjustSliderByStep(WidgetInputContext& ctx,
                        int steps,
                        SliderStepOptions const& options = {}) -> InputUpdate;
auto AdjustSliderAnalog(WidgetInputContext& ctx,
                        float axis_value,
                        SliderAnalogOptions const& options = {}) -> InputUpdate;

// Build pointer metadata for keyboard/gamepad driven widget interactions.
auto ProgrammaticPointer(float scene_x, float scene_y, bool inside = true) -> Bindings::PointerInfo;

auto SliderPointerForValue(WidgetInputContext const& ctx, float value) -> std::pair<float, float>;
auto SliderThumbPosition(WidgetInputContext const& ctx, float value) -> std::pair<float, float>;
auto ListItemCenter(WidgetInputContext const& ctx, int index) -> std::pair<float, float>;
auto TreeRowCenter(WidgetInputContext const& ctx, int index) -> std::pair<float, float>;
auto TreeParentIndex(WidgetInputContext const& ctx, int index) -> int;

auto BoundsFromRect(Widgets::ListPreviewRect const& rect) -> WidgetBounds;
auto BoundsFromRect(Widgets::TreePreviewRect const& rect) -> WidgetBounds;
auto BoundsFromRect(Widgets::TreePreviewRect const& rect,
                    float dx,
                    float dy) -> WidgetBounds;

auto MakeListLayout(Widgets::ListPreviewLayout const& layout) -> std::optional<ListLayout>;
auto MakeTreeLayout(Widgets::TreePreviewLayout const& layout) -> std::optional<TreeLayout>;

auto ExpandForFocusHighlight(WidgetBounds& bounds) -> void;
auto FocusHighlightPadding() -> float;
auto MakeDirtyHint(WidgetBounds const& bounds) -> Builders::DirtyRectHint;
auto TranslateTreeLayout(TreeLayout& layout, float dx, float dy) -> void;

} // namespace Input

struct WidgetTheme {
    ButtonStyle button{};
    ToggleStyle toggle{};
    SliderStyle slider{};
    ListStyle list{};
    TreeStyle tree{};
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

struct ThemeSelection {
    WidgetTheme theme{};
    std::string canonical_name;
    bool recognized = true;
};

auto MakeDefaultWidgetTheme() -> WidgetTheme;
auto MakeSunsetWidgetTheme() -> WidgetTheme;
auto SetTheme(PathSpace& space,
              AppRootPathView appRoot,
              std::optional<std::string> const& requested_name) -> ThemeSelection;
auto ApplyTheme(WidgetTheme const& theme, ButtonParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, ToggleParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, SliderParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, ListParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, TreeParams& params) -> void;

struct ButtonParamsBuilder {
    ButtonParams value{};

    static auto Make(std::string name, std::string label = {}) -> ButtonParamsBuilder {
        ButtonParamsBuilder builder;
        builder.value.name = std::move(name);
        builder.value.label = std::move(label);
        return builder;
    }

    auto WithName(std::string name) -> ButtonParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithLabel(std::string label) -> ButtonParamsBuilder& {
        value.label = std::move(label);
        return *this;
    }

    auto WithStyle(ButtonStyle style) -> ButtonParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> ButtonParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> ButtonParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> ButtonParams { return value; }
    auto Build() && -> ButtonParams { return std::move(value); }
};

struct ToggleParamsBuilder {
    ToggleParams value{};

    static auto Make(std::string name) -> ToggleParamsBuilder {
        ToggleParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> ToggleParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithStyle(ToggleStyle style) -> ToggleParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> ToggleParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> ToggleParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> ToggleParams { return value; }
    auto Build() && -> ToggleParams { return std::move(value); }
};

struct SliderParamsBuilder {
    SliderParams value{};

    static auto Make(std::string name) -> SliderParamsBuilder {
        SliderParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> SliderParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithMinimum(float minimum) -> SliderParamsBuilder& {
        value.minimum = minimum;
        return *this;
    }

    auto WithMaximum(float maximum) -> SliderParamsBuilder& {
        value.maximum = maximum;
        return *this;
    }

    auto WithValue(float current) -> SliderParamsBuilder& {
        value.value = current;
        return *this;
    }

    auto WithStep(float step) -> SliderParamsBuilder& {
        value.step = step;
        return *this;
    }

    auto WithRange(float minimum, float maximum) -> SliderParamsBuilder& {
        value.minimum = minimum;
        value.maximum = maximum;
        return *this;
    }

    auto WithStyle(SliderStyle style) -> SliderParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> SliderParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> SliderParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> SliderParams { return value; }
    auto Build() && -> SliderParams { return std::move(value); }
};

struct ListParamsBuilder {
    ListParams value{};

    static auto Make(std::string name) -> ListParamsBuilder {
        ListParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> ListParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithItems(std::vector<ListItem> items) -> ListParamsBuilder& {
        value.items = std::move(items);
        return *this;
    }

    auto AddItem(ListItem item) -> ListParamsBuilder& {
        value.items.push_back(std::move(item));
        return *this;
    }

    auto WithStyle(ListStyle style) -> ListParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> ListParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> ListParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> ListParams { return value; }
    auto Build() && -> ListParams { return std::move(value); }
};

struct TreeParamsBuilder {
    TreeParams value{};

    static auto Make(std::string name) -> TreeParamsBuilder {
        TreeParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> TreeParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithNodes(std::vector<TreeNode> nodes) -> TreeParamsBuilder& {
        value.nodes = std::move(nodes);
        return *this;
    }

    auto WithStyle(TreeStyle style) -> TreeParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> TreeParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> TreeParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> TreeParams { return value; }
    auto Build() && -> TreeParams { return std::move(value); }
};

struct StackLayoutParamsBuilder {
    StackLayoutParams value{};

    static auto Make(std::string name) -> StackLayoutParamsBuilder {
        StackLayoutParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> StackLayoutParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithStyle(StackLayoutStyle style) -> StackLayoutParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> StackLayoutParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithChildren(std::vector<StackChildSpec> children) -> StackLayoutParamsBuilder& {
        value.children = std::move(children);
        return *this;
    }

    auto AddChild(StackChildSpec child) -> StackLayoutParamsBuilder& {
        value.children.push_back(std::move(child));
        return *this;
    }

    auto Build() const -> StackLayoutParams { return value; }
    auto Build() && -> StackLayoutParams { return std::move(value); }
};

struct ButtonStateBuilder {
    ButtonState value{};

    static auto Make() -> ButtonStateBuilder { return ButtonStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> ButtonStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithPressed(bool pressed = true) -> ButtonStateBuilder& {
        value.pressed = pressed;
        return *this;
    }

    auto WithHovered(bool hovered = true) -> ButtonStateBuilder& {
        value.hovered = hovered;
        return *this;
    }

    auto WithFocused(bool focused = true) -> ButtonStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto Build() const -> ButtonState { return value; }
    auto Build() && -> ButtonState { return std::move(value); }
};

struct ToggleStateBuilder {
    ToggleState value{};

    static auto Make() -> ToggleStateBuilder { return ToggleStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> ToggleStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithHovered(bool hovered = true) -> ToggleStateBuilder& {
        value.hovered = hovered;
        return *this;
    }

    auto WithChecked(bool checked = true) -> ToggleStateBuilder& {
        value.checked = checked;
        return *this;
    }

    auto WithFocused(bool focused = true) -> ToggleStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto Build() const -> ToggleState { return value; }
    auto Build() && -> ToggleState { return std::move(value); }
};

struct SliderStateBuilder {
    SliderState value{};

    static auto Make() -> SliderStateBuilder { return SliderStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> SliderStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithHovered(bool hovered = true) -> SliderStateBuilder& {
        value.hovered = hovered;
        return *this;
    }

    auto WithDragging(bool dragging = true) -> SliderStateBuilder& {
        value.dragging = dragging;
        return *this;
    }

    auto WithFocused(bool focused = true) -> SliderStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto WithValue(float current) -> SliderStateBuilder& {
        value.value = current;
        return *this;
    }

    auto Build() const -> SliderState { return value; }
    auto Build() && -> SliderState { return std::move(value); }
};

struct ListStateBuilder {
    ListState value{};

    static auto Make() -> ListStateBuilder { return ListStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> ListStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithFocused(bool focused = true) -> ListStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto WithHoveredIndex(std::int32_t index) -> ListStateBuilder& {
        value.hovered_index = index;
        return *this;
    }

    auto WithSelectedIndex(std::int32_t index) -> ListStateBuilder& {
        value.selected_index = index;
        return *this;
    }

    auto WithScrollOffset(float offset) -> ListStateBuilder& {
        value.scroll_offset = offset;
        return *this;
    }

    auto Build() const -> ListState { return value; }
    auto Build() && -> ListState { return std::move(value); }
};

struct TreeStateBuilder {
    TreeState value{};

    static auto Make() -> TreeStateBuilder { return TreeStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> TreeStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithFocused(bool focused = true) -> TreeStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto WithHoveredId(std::string id) -> TreeStateBuilder& {
        value.hovered_id = std::move(id);
        return *this;
    }

    auto WithSelectedId(std::string id) -> TreeStateBuilder& {
        value.selected_id = std::move(id);
        return *this;
    }

    auto WithExpandedIds(std::vector<std::string> ids) -> TreeStateBuilder& {
        value.expanded_ids = std::move(ids);
        return *this;
    }

    auto WithLoadingIds(std::vector<std::string> ids) -> TreeStateBuilder& {
        value.loading_ids = std::move(ids);
        return *this;
    }

    auto WithScrollOffset(float offset) -> TreeStateBuilder& {
        value.scroll_offset = offset;
        return *this;
    }

    auto Build() const -> TreeState { return value; }
    auto Build() && -> TreeState { return std::move(value); }
};

[[nodiscard]] inline auto MakeButtonParams(std::string name, std::string label = {}) -> ButtonParamsBuilder {
    return ButtonParamsBuilder::Make(std::move(name), std::move(label));
}

[[nodiscard]] inline auto MakeToggleParams(std::string name) -> ToggleParamsBuilder {
    return ToggleParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeSliderParams(std::string name) -> SliderParamsBuilder {
    return SliderParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeListParams(std::string name) -> ListParamsBuilder {
    return ListParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeTreeParams(std::string name) -> TreeParamsBuilder {
    return TreeParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeStackLayoutParams(std::string name) -> StackLayoutParamsBuilder {
    return StackLayoutParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeButtonState() -> ButtonStateBuilder {
    return ButtonStateBuilder::Make();
}

[[nodiscard]] inline auto MakeToggleState() -> ToggleStateBuilder {
    return ToggleStateBuilder::Make();
}

[[nodiscard]] inline auto MakeSliderState() -> SliderStateBuilder {
    return SliderStateBuilder::Make();
}

[[nodiscard]] inline auto MakeListState() -> ListStateBuilder {
    return ListStateBuilder::Make();
}

[[nodiscard]] inline auto MakeTreeState() -> TreeStateBuilder {
    return TreeStateBuilder::Make();
}

namespace Reducers {

struct WidgetAction {
    Bindings::WidgetOpKind kind = Bindings::WidgetOpKind::HoverEnter;
    std::string widget_path;
    std::string target_id;
    Bindings::PointerInfo pointer{};
    float analog_value = 0.0f;
    std::int32_t discrete_index = -1;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
};

auto MakeWidgetAction(Bindings::WidgetOp const& op) -> WidgetAction;

struct ProcessActionsResult {
    ConcretePath ops_queue;
    ConcretePath actions_queue;
    std::vector<WidgetAction> actions;
};

auto WidgetOpsQueue(WidgetPath const& widget_root) -> ConcretePath;

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath;

auto ReducePending(PathSpace& space,
                   ConcretePathView ops_queue,
                   std::size_t max_actions = std::numeric_limits<std::size_t>::max()) -> SP::Expected<std::vector<WidgetAction>>;

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void>;

auto ProcessPendingActions(PathSpace& space,
                           WidgetPath const& widget_root,
                           std::size_t max_actions = std::numeric_limits<std::size_t>::max()) -> SP::Expected<ProcessActionsResult>;

} // namespace Reducers

} // namespace Widgets

namespace Config::Theme {

struct ThemePaths {
    ConcretePath root;
    ConcretePath value;
};

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
