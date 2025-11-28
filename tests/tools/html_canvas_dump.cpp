#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/HtmlRunner.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace SP;
using namespace SP::UI;
namespace Runtime = SP::UI::Runtime;
namespace SceneData = SP::UI::Scene;
namespace WidgetBuilders = SP::UI::Builders::Widgets;

namespace {

enum class Scenario {
    Basic,
    WidgetsDefault,
    WidgetsSunset,
};

[[nodiscard]] auto scenario_from_string(std::string_view value) -> std::optional<Scenario> {
    if (value == "basic") {
        return Scenario::Basic;
    }
    if (value == "widgets-default") {
        return Scenario::WidgetsDefault;
    }
    if (value == "widgets-sunset") {
        return Scenario::WidgetsSunset;
    }
    return std::nullopt;
}

[[nodiscard]] auto scenario_label(Scenario scenario) -> std::string_view {
    switch (scenario) {
    case Scenario::Basic:
        return "basic";
    case Scenario::WidgetsDefault:
        return "widgets-default";
    case Scenario::WidgetsSunset:
        return "widgets-sunset";
    }
    return "basic";
}

void print_usage() {
    std::cout << "Usage: html_canvas_dump [--prefer-dom] [--scenario basic|widgets-default|widgets-sunset]\n";
}

[[nodiscard]] auto identity_transform() -> SceneData::Transform {
    SceneData::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

[[nodiscard]] auto make_sphere(float min_x, float min_y, float max_x, float max_y) -> SceneData::BoundingSphere {
    SceneData::BoundingSphere sphere{};
    float center_x = (min_x + max_x) * 0.5f;
    float center_y = (min_y + max_y) * 0.5f;
    float radius_x = std::max(0.0f, max_x - center_x);
    float radius_y = std::max(0.0f, max_y - center_y);
    sphere.center = {center_x, center_y, 0.0f};
    sphere.radius = std::hypot(radius_x, radius_y);
    return sphere;
}

[[nodiscard]] auto make_box(float min_x, float min_y, float max_x, float max_y) -> SceneData::BoundingBox {
    SceneData::BoundingBox box{};
    box.min = {min_x, min_y, 0.0f};
    box.max = {max_x, max_y, 0.0f};
    return box;
}

struct BucketBuilder {
    SceneData::DrawableBucketSnapshot bucket{};
    std::uint32_t command_index = 0;

    template <typename Command>
    void add_drawable(std::uint64_t id,
                      Command const& command,
                      SceneData::DrawCommandKind kind,
                      float z_value,
                      bool opaque,
                      std::string_view authoring_id) {
        bucket.drawable_ids.push_back(id);
        bucket.world_transforms.push_back(identity_transform());
        bucket.bounds_spheres.push_back(make_sphere(command.min_x, command.min_y, command.max_x, command.max_y));
        bucket.bounds_boxes.push_back(make_box(command.min_x, command.min_y, command.max_x, command.max_y));
        bucket.bounds_box_valid.push_back(1);
        bucket.layers.push_back(0);
        bucket.z_values.push_back(z_value);
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.command_offsets.push_back(command_index);
        bucket.command_counts.push_back(1);
        bucket.layer_indices.emplace_back();
        bucket.clip_head_indices.push_back(-1);
        bucket.drawable_fingerprints.push_back(id ^ static_cast<std::uint64_t>(kind));
        bucket.authoring_map.push_back({id, std::string(authoring_id), 0, 0});

        auto start = bucket.command_payload.size();
        bucket.command_payload.resize(start + sizeof(Command));
        std::memcpy(bucket.command_payload.data() + start, &command, sizeof(Command));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(kind));

        auto drawable_index = static_cast<std::uint32_t>(bucket.drawable_ids.size() - 1);
        if (opaque) {
            bucket.opaque_indices.push_back(drawable_index);
        } else {
            bucket.alpha_indices.push_back(drawable_index);
        }
        ++command_index;
    }

    void add_rect(std::uint64_t id,
                  SceneData::RectCommand const& rect,
                  float z_value,
                  bool opaque,
                  std::string_view authoring_id) {
        add_drawable(id, rect, SceneData::DrawCommandKind::Rect, z_value, opaque, authoring_id);
    }

    void add_rounded_rect(std::uint64_t id,
                          SceneData::RoundedRectCommand const& rounded,
                          float z_value,
                          bool opaque,
                          std::string_view authoring_id) {
        add_drawable(id, rounded, SceneData::DrawCommandKind::RoundedRect, z_value, opaque, authoring_id);
    }

    [[nodiscard]] auto finish() && -> SceneData::DrawableBucketSnapshot {
        return std::move(bucket);
    }
};

[[nodiscard]] auto make_basic_bucket() -> SceneData::DrawableBucketSnapshot {
    BucketBuilder builder;

    SceneData::RectCommand rect{};
    rect.min_x = 12.0f;
    rect.min_y = 8.0f;
    rect.max_x = 40.0f;
    rect.max_y = 24.0f;
    rect.color = {0.2f, 0.4f, 0.7f, 1.0f};
    builder.add_rect(0xAAu, rect, 0.0f, true, "basic/rect");

    SceneData::RoundedRectCommand rounded{};
    rounded.min_x = 44.0f;
    rounded.min_y = 18.0f;
    rounded.max_x = 70.0f;
    rounded.max_y = 40.0f;
    rounded.radius_top_left = 3.0f;
    rounded.radius_top_right = 2.0f;
    rounded.radius_bottom_right = 4.0f;
    rounded.radius_bottom_left = 1.5f;
    rounded.color = {0.9f, 0.3f, 0.2f, 0.6f};
    builder.add_rounded_rect(0xBBu, rounded, 0.1f, false, "basic/rounded");

    return std::move(builder).finish();
}

[[nodiscard]] auto clamp_radius(float value, float width, float height) -> float {
    float limit = std::min(width, height) * 0.5f;
    return std::clamp(value, 0.0f, limit);
}

[[nodiscard]] auto make_widget_bucket(WidgetBuilders::WidgetTheme const& theme) -> SceneData::DrawableBucketSnapshot {
    BucketBuilder builder;

    constexpr float kButtonX = 20.0f;
    constexpr float kButtonY = 20.0f;

    SceneData::RoundedRectCommand button{};
    button.min_x = kButtonX;
    button.min_y = kButtonY;
    button.max_x = button.min_x + std::max(theme.button.width, 1.0f);
    button.max_y = button.min_y + std::max(theme.button.height, 1.0f);
    float button_radius = clamp_radius(theme.button.corner_radius,
                                       button.max_x - button.min_x,
                                       button.max_y - button.min_y);
    button.radius_top_left = button.radius_top_right = button.radius_bottom_right = button.radius_bottom_left = button_radius;
    button.color = theme.button.background_color;
    builder.add_rounded_rect(0xB1000001ull, button, 0.0f, true, "widgets/button/background");

    float toggle_width = std::max(theme.toggle.width, 16.0f);
    float toggle_height = std::max(theme.toggle.height, 16.0f);
    float toggle_x = button.max_x + 32.0f;
    float toggle_y = button.min_y + ((button.max_y - button.min_y) - toggle_height) * 0.5f;

    SceneData::RoundedRectCommand toggle_track{};
    toggle_track.min_x = toggle_x;
    toggle_track.min_y = toggle_y;
    toggle_track.max_x = toggle_x + toggle_width;
    toggle_track.max_y = toggle_y + toggle_height;
    float toggle_radius = toggle_height * 0.5f;
    toggle_track.radius_top_left = toggle_radius;
    toggle_track.radius_top_right = toggle_radius;
    toggle_track.radius_bottom_right = toggle_radius;
    toggle_track.radius_bottom_left = toggle_radius;
    toggle_track.color = theme.toggle.track_on_color;
    builder.add_rounded_rect(0xB2000001ull, toggle_track, 0.05f, true, "widgets/toggle/track");

    SceneData::RoundedRectCommand toggle_thumb{};
    float thumb_padding = 2.0f;
    float thumb_radius = std::max(1.0f, toggle_radius - thumb_padding);
    float thumb_center_x = toggle_track.max_x - (thumb_radius + thumb_padding);
    toggle_thumb.min_x = thumb_center_x - thumb_radius;
    toggle_thumb.min_y = toggle_track.min_y + thumb_padding;
    toggle_thumb.max_x = thumb_center_x + thumb_radius;
    toggle_thumb.max_y = toggle_track.max_y - thumb_padding;
    toggle_thumb.radius_top_left = thumb_radius;
    toggle_thumb.radius_top_right = thumb_radius;
    toggle_thumb.radius_bottom_right = thumb_radius;
    toggle_thumb.radius_bottom_left = thumb_radius;
    toggle_thumb.color = theme.toggle.thumb_color;
    builder.add_rounded_rect(0xB2000002ull, toggle_thumb, 0.06f, true, "widgets/toggle/thumb");

    float slider_width = std::max(theme.slider.width, 32.0f);
    float slider_height = std::max(theme.slider.height, 16.0f);
    float slider_track_height = std::clamp(theme.slider.track_height, 1.0f, slider_height);
    float slider_x = kButtonX;
    float slider_y = button.max_y + 30.0f;
    float slider_track_top = slider_y + (slider_height - slider_track_height) * 0.5f;
    float slider_progress = 0.62f;

    SceneData::RectCommand slider_track{};
    slider_track.min_x = slider_x;
    slider_track.min_y = slider_track_top;
    slider_track.max_x = slider_x + slider_width;
    slider_track.max_y = slider_track_top + slider_track_height;
    slider_track.color = theme.slider.track_color;
    builder.add_rect(0xB3000001ull, slider_track, 0.0f, true, "widgets/slider/track");

    SceneData::RectCommand slider_fill = slider_track;
    slider_fill.max_x = slider_fill.min_x + slider_width * slider_progress;
    slider_fill.color = theme.slider.fill_color;
    builder.add_rect(0xB3000002ull, slider_fill, 0.01f, true, "widgets/slider/fill");

    SceneData::RoundedRectCommand slider_thumb{};
    float slider_thumb_radius = std::clamp(theme.slider.thumb_radius, slider_track_height * 0.5f, slider_height * 0.5f);
    float slider_thumb_center = slider_fill.max_x;
    slider_thumb.min_x = slider_thumb_center - slider_thumb_radius;
    slider_thumb.max_x = slider_thumb_center + slider_thumb_radius;
    slider_thumb.min_y = slider_y + (slider_height - slider_thumb_radius * 2.0f) * 0.5f;
    slider_thumb.max_y = slider_thumb.min_y + slider_thumb_radius * 2.0f;
    slider_thumb.radius_top_left = slider_thumb_radius;
    slider_thumb.radius_top_right = slider_thumb_radius;
    slider_thumb.radius_bottom_right = slider_thumb_radius;
    slider_thumb.radius_bottom_left = slider_thumb_radius;
    slider_thumb.color = theme.slider.thumb_color;
    builder.add_rounded_rect(0xB3000003ull, slider_thumb, 0.02f, true, "widgets/slider/thumb");

    float list_width = std::max(theme.list.width, 120.0f);
    float list_item_height = std::max(theme.list.item_height, 24.0f);
    float list_x = slider_x;
    float list_y = slider_y + slider_height + 24.0f;
    int list_item_count = 3;
    float list_padding = 8.0f;
    float list_height = list_item_count * list_item_height + list_padding * 2.0f;

    SceneData::RoundedRectCommand list_background{};
    list_background.min_x = list_x;
    list_background.min_y = list_y;
    list_background.max_x = list_x + list_width;
    list_background.max_y = list_y + list_height;
    float list_radius = clamp_radius(theme.list.corner_radius,
                                     list_background.max_x - list_background.min_x,
                                     list_background.max_y - list_background.min_y);
    list_background.radius_top_left = list_background.radius_top_right =
        list_background.radius_bottom_right = list_background.radius_bottom_left = list_radius;
    list_background.color = theme.list.background_color;
    builder.add_rounded_rect(0xB4000001ull, list_background, 0.0f, true, "widgets/list/background");

    std::array<std::array<float, 4>, 3> item_colors{
        theme.list.item_hover_color,
        theme.list.item_selected_color,
        theme.list.item_color,
    };

    for (int i = 0; i < list_item_count; ++i) {
        SceneData::RectCommand item{};
        float item_top = list_y + list_padding + static_cast<float>(i) * list_item_height;
        item.min_x = list_x + list_padding;
        item.max_x = list_x + list_width - list_padding;
        item.min_y = item_top;
        item.max_y = item_top + list_item_height - 4.0f;
        item.color = item_colors[static_cast<std::size_t>(i % item_colors.size())];
        builder.add_rect(0xB4000002ull + static_cast<std::uint64_t>(i),
                         item,
                         0.01f + static_cast<float>(i) * 0.001f,
                         true,
                         "widgets/list/item");

        if (i < list_item_count - 1) {
            SceneData::RectCommand separator{};
            separator.min_x = item.min_x;
            separator.max_x = item.max_x;
            separator.min_y = item.max_y;
            separator.max_y = separator.min_y + 1.0f;
            separator.color = theme.list.separator_color;
            builder.add_rect(0xB4000100ull + static_cast<std::uint64_t>(i),
                             separator,
                             0.015f,
                             true,
                             "widgets/list/separator");
        }
    }

    return std::move(builder).finish();
}

struct RenderHarness {
    PathSpace space{};
    SP::App::AppRootPath app_root{"/system/applications/html_canvas_verify"};
    Builders::ScenePath scene;
    Builders::RendererPath renderer;
    Builders::SurfacePath surface;
    SP::ConcretePathString target;
    Runtime::SurfaceDesc surface_desc{};
    Builders::RenderSettings settings{};
    PathRenderer2D renderer2d;
    std::uint64_t frame_index = 0;
    bool ready = false;

    RenderHarness()
        : renderer2d(space) {
        if (!initialise()) {
            return;
        }
        ready = true;
    }

    [[nodiscard]] auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    [[nodiscard]] auto initialise() -> bool {
        Builders::SceneParams scene_params{
            .name = "html_canvas_verify_scene",
            .description = "HtmlCanvasVerify bucket",
        };
        auto scene_result = Builders::Scene::Create(space, root_view(), scene_params);
        if (!scene_result) {
            std::cerr << "Failed to create scene: " << scene_result.error().message.value_or("<unspecified>") << "\n";
            return false;
        }
        scene = *scene_result;

        Builders::RendererParams renderer_params{
            .name = "html_canvas_verify_renderer",
            .kind = Builders::RendererKind::Software2D,
            .description = "HtmlCanvasVerify renderer",
        };
        auto renderer_result = Builders::Renderer::Create(space, root_view(), renderer_params);
        if (!renderer_result) {
            std::cerr << "Failed to create renderer: " << renderer_result.error().message.value_or("<unspecified>") << "\n";
            return false;
        }
        renderer = *renderer_result;

        surface_desc.size_px.width = 512;
        surface_desc.size_px.height = 360;
        surface_desc.pixel_format = Runtime::PixelFormat::RGBA8Unorm_sRGB;
        surface_desc.color_space = Runtime::ColorSpace::sRGB;
        surface_desc.premultiplied_alpha = true;
        surface_desc.progressive_tile_size_px = 32;

        Builders::SurfaceParams surface_params{};
        surface_params.name = "html_canvas_verify_surface";
        surface_params.desc = surface_desc;
        surface_params.renderer = std::string("renderers/") + renderer_params.name;

        auto surface_result = Builders::Surface::Create(space, root_view(), surface_params);
        if (!surface_result) {
            std::cerr << "Failed to create surface: " << surface_result.error().message.value_or("<unspecified>") << "\n";
            return false;
        }
        surface = *surface_result;

        auto set_scene = Builders::Surface::SetScene(space, surface, scene);
        if (!set_scene) {
            std::cerr << "Failed to attach scene to surface: " << set_scene.error().message.value_or("<unspecified>") << "\n";
            return false;
        }

        auto target_rel = space.read<std::string, std::string>(std::string(surface.getPath()) + "/target");
        if (!target_rel) {
            std::cerr << "Failed to read surface target: " << target_rel.error().message.value_or("<unspecified>") << "\n";
            return false;
        }
        auto target_abs = SP::App::resolve_app_relative(root_view(), *target_rel);
        if (!target_abs) {
            std::cerr << "Failed to resolve surface target path: " << target_abs.error().message.value_or("<unspecified>") << "\n";
            return false;
        }
        target = SP::ConcretePathString{target_abs->getPath()};

        settings.surface.size_px.width = surface_desc.size_px.width;
        settings.surface.size_px.height = surface_desc.size_px.height;
        settings.surface.dpi_scale = 1.0f;
        settings.surface.visibility = true;
        settings.renderer.backend_kind = Builders::RendererKind::Software2D;
        settings.renderer.metal_uploads_enabled = false;
        settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
        return true;
    }

    [[nodiscard]] auto render_bucket(SceneData::DrawableBucketSnapshot const& bucket) -> std::optional<std::vector<std::uint8_t>> {
        if (!ready) {
            return std::nullopt;
        }

        SceneData::SceneSnapshotBuilder builder{space, root_view(), scene};
        SceneData::SnapshotPublishOptions publish_opts{};
        publish_opts.metadata.author = "html_canvas_dump";
        publish_opts.metadata.tool_version = "verify";
        publish_opts.metadata.drawable_count = bucket.drawable_ids.size();
        publish_opts.metadata.command_count = bucket.command_kinds.size();
        auto revision = builder.publish(publish_opts, bucket);
        if (!revision) {
            std::cerr << "Failed to publish snapshot: " << revision.error().message.value_or("<unspecified>") << "\n";
            return std::nullopt;
        }

        PathSurfaceSoftware surface_instance{
            surface_desc,
            PathSurfaceSoftware::Options{
                .enable_progressive = false,
                .enable_buffered = true,
                .progressive_tile_size_px = 32,
            },
        };

        settings.time.frame_index = frame_index++;
        auto render_result = renderer2d.render({
            .target_path = SP::ConcretePathStringView{target.getPath()},
            .settings = settings,
            .surface = surface_instance,
            .backend_kind = Builders::RendererKind::Software2D,
        });
        if (!render_result) {
            std::cerr << "Renderer failed: " << render_result.error().message.value_or("<unspecified>") << "\n";
            return std::nullopt;
        }

        std::vector<std::uint8_t> buffer(surface_instance.frame_bytes(), 0);
        if (!surface_instance.copy_buffered_frame(buffer)) {
            std::cerr << "Failed to copy framebuffer" << "\n";
            return std::nullopt;
        }
        return buffer;
    }

    [[nodiscard]] auto render_and_compare(SceneData::DrawableBucketSnapshot const& bucket,
                                          Html::EmitResult const& emitted) -> std::optional<std::pair<std::string, std::string>> {
        auto baseline = render_bucket(bucket);
        if (!baseline) {
            return std::nullopt;
        }

        Html::CanvasReplayOptions replay_opts{};
        replay_opts.stroke_points = emitted.stroke_points;
        auto replay_bucket = Html::commands_to_bucket(emitted.canvas_replay_commands, replay_opts);
        if (!replay_bucket) {
            std::cerr << "Failed to replay canvas commands: " << replay_bucket.error().message.value_or("<unspecified>") << "\n";
            return std::nullopt;
        }

        auto replay = render_bucket(*replay_bucket);
        if (!replay) {
            return std::nullopt;
        }

        auto hash_buffer = [](std::vector<std::uint8_t> const& data) -> std::string {
            constexpr std::uint64_t kOffset = 1469598103934665603ull;
            constexpr std::uint64_t kPrime = 1099511628211ull;
            std::uint64_t hash = kOffset;
            for (auto byte : data) {
                hash ^= static_cast<std::uint64_t>(byte);
                hash *= kPrime;
            }
            std::ostringstream oss;
            oss << std::hex << std::setfill('0') << std::setw(16) << hash;
            return oss.str();
        };

        return std::pair{hash_buffer(*baseline), hash_buffer(*replay)};
    }
};

[[nodiscard]] auto escape_json(std::string const& input) -> std::string {
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
        case '\"':
            escaped.append("\\\"");
            break;
        case '\\':
            escaped.append("\\\\");
            break;
        case '\b':
            escaped.append("\\b");
            break;
        case '\f':
            escaped.append("\\f");
            break;
        case '\n':
            escaped.append("\\n");
            break;
        case '\r':
            escaped.append("\\r");
            break;
        case '\t':
            escaped.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                escaped.append(buffer);
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }
    return escaped;
}

} // namespace

int main(int argc, char** argv) {
    bool prefer_dom = false;
    Scenario scenario = Scenario::Basic;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--prefer-dom") {
            prefer_dom = true;
        } else if (arg == "--scenario") {
            if (i + 1 >= argc) {
                std::cerr << "--scenario requires a value" << "\n";
                print_usage();
                return EXIT_FAILURE;
            }
            auto parsed = scenario_from_string(argv[i + 1]);
            if (!parsed) {
                std::cerr << "Unknown scenario: " << argv[i + 1] << "\n";
                print_usage();
                return EXIT_FAILURE;
            }
            scenario = *parsed;
            ++i;
        } else if (arg == "--help") {
            print_usage();
            return EXIT_SUCCESS;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage();
            return EXIT_FAILURE;
        }
    }

    SceneData::DrawableBucketSnapshot bucket;
    switch (scenario) {
    case Scenario::Basic:
        bucket = make_basic_bucket();
        break;
    case Scenario::WidgetsDefault:
        bucket = make_widget_bucket(WidgetBuilders::MakeDefaultWidgetTheme());
        break;
    case Scenario::WidgetsSunset:
        bucket = make_widget_bucket(WidgetBuilders::MakeSunsetWidgetTheme());
        break;
    }

    Html::Adapter adapter;
    Html::EmitOptions options{};
    options.prefer_dom = prefer_dom;
    auto emitted = adapter.emit(bucket, options);
    if (!emitted) {
        std::cerr << "Html adapter emit failed: " << emitted.error().message.value_or("<unspecified>") << "\n";
        return EXIT_FAILURE;
    }

    std::optional<std::pair<std::string, std::string>> digests;
    static std::optional<RenderHarness> harness;
    if (emitted->used_canvas_fallback) {
        if (!harness) {
            harness.emplace();
            if (!harness->ready) {
                std::cerr << "Render harness initialisation failed" << "\n";
                return EXIT_FAILURE;
            }
        }
        digests = harness->render_and_compare(bucket, *emitted);
        if (!digests) {
            return EXIT_FAILURE;
        }
    }

    std::string canvas_json = emitted->canvas_commands.empty() ? "[]" : emitted->canvas_commands;
    std::cout << "{";
    std::cout << "\"scenario\":\"" << scenario_label(scenario) << "\",";
    std::cout << "\"preferDom\":" << (prefer_dom ? "true" : "false") << ",";
    std::cout << "\"usedCanvasFallback\":" << (emitted->used_canvas_fallback ? "true" : "false") << ",";
    std::cout << "\"canvas\":" << canvas_json << ",";
    std::cout << "\"dom\":\"" << escape_json(emitted->dom) << "\",";
    if (digests) {
        std::cout << "\"baselineDigest\":\"" << digests->first << "\",";
        std::cout << "\"replayDigest\":\"" << digests->second << "\"";
    } else {
        std::cout << "\"baselineDigest\":null,\"replayDigest\":null";
    }
    std::cout << "}" << std::endl;

    return EXIT_SUCCESS;
}
