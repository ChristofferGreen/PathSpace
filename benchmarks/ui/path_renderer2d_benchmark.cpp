#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <path/UnvalidatedPath.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <vector>

using namespace SP;
using namespace SP::UI;
namespace UIScene = SP::UI::Scene;

namespace {

struct Stroke {
    std::uint64_t            drawable_id = 0;
    UIScene::RectCommand     rect{};
    std::string              authoring_id;
};

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (int i = 0; i < 16; ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

template <typename T>
auto replace_value(PathSpace& space, std::string const& path, T const& value) -> void {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& err = taken.error();
        if (err.code == Error::Code::NoObjectFound
            || err.code == Error::Code::NoSuchPath) {
            break;
        }
        auto message = err.message.value_or("unknown error");
        throw std::runtime_error("failed clearing '" + path + "': " + message);
    }
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        auto const& err = inserted.errors.front();
        auto message = err.message.value_or("unknown error");
        throw std::runtime_error("failed writing '" + path + "': " + message);
    }
}

auto build_bucket(std::vector<Stroke> const& strokes) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    auto const count = strokes.size();

    bucket.drawable_ids.reserve(count);
    bucket.world_transforms.reserve(count);
    bucket.bounds_spheres.reserve(count);
    bucket.bounds_boxes.reserve(count);
    bucket.bounds_box_valid.reserve(count);
    bucket.layers.reserve(count);
    bucket.z_values.reserve(count);
    bucket.material_ids.reserve(count);
    bucket.pipeline_flags.reserve(count);
    bucket.visibility.reserve(count);
    bucket.command_offsets.reserve(count);
    bucket.command_counts.reserve(count);
    bucket.authoring_map.reserve(count);
    bucket.clip_head_indices.assign(count, -1);
    bucket.drawable_fingerprints.reserve(count);

    std::hash<std::string> hash_author;

    for (std::size_t i = 0; i < count; ++i) {
        auto const& stroke = strokes[i];
        bucket.drawable_ids.push_back(stroke.drawable_id);
        bucket.world_transforms.push_back(identity_transform());

        UIScene::BoundingBox box{};
        box.min = {stroke.rect.min_x, stroke.rect.min_y, 0.0f};
        box.max = {stroke.rect.max_x, stroke.rect.max_y, 0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        auto width = std::max(0.0f, stroke.rect.max_x - stroke.rect.min_x);
        auto height = std::max(0.0f, stroke.rect.max_y - stroke.rect.min_y);
        float radius = std::sqrt(width * width + height * height) * 0.5f;
        UIScene::BoundingSphere sphere{};
        sphere.center = {(stroke.rect.min_x + stroke.rect.max_x) * 0.5f,
                         (stroke.rect.min_y + stroke.rect.max_y) * 0.5f,
                         0.0f};
        sphere.radius = radius;
        bucket.bounds_spheres.push_back(sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(static_cast<float>(i));
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);

        auto command_index = static_cast<std::uint32_t>(bucket.command_kinds.size());
        bucket.command_offsets.push_back(command_index);
        bucket.command_counts.push_back(1);
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

        auto payload_offset = bucket.command_payload.size();
        bucket.command_payload.resize(payload_offset + sizeof(UIScene::RectCommand));
        std::memcpy(bucket.command_payload.data() + payload_offset,
                    &stroke.rect,
                    sizeof(UIScene::RectCommand));

        bucket.authoring_map.push_back(UIScene::DrawableAuthoringMapEntry{
            stroke.drawable_id,
            stroke.authoring_id,
            0,
            0});

        auto fingerprint = static_cast<std::uint64_t>(hash_author(stroke.authoring_id));
        fingerprint ^= static_cast<std::uint64_t>(stroke.drawable_id) << 32;
        bucket.drawable_fingerprints.push_back(fingerprint);
    }

    bucket.opaque_indices.resize(count);
    std::iota(bucket.opaque_indices.begin(), bucket.opaque_indices.end(), 0u);
    bucket.alpha_indices.clear();

    return bucket;
}

struct FrameMetrics {
    double render_ms = 0.0;
    double damage_ms = 0.0;
    double encode_ms = 0.0;
    double progressive_copy_ms = 0.0;
    double publish_ms = 0.0;
    double present_ms = 0.0;
    std::uint64_t tiles = 0;
    std::uint64_t bytes = 0;
};

auto read_metric(PathSpace const& space, std::string const& base, std::string const& leaf) -> std::uint64_t {
    auto value = space.read<std::uint64_t>(base + "/" + leaf);
    if (value) {
        return *value;
    }
    return 0;
}

auto render_frame(PathRenderer2D& renderer,
                  PathSurfaceSoftware& surface,
                  PathSpace& space,
                  Builders::ConcretePathView targetPath,
                  Builders::RenderSettings& settings,
                  std::uint64_t frame_index) -> FrameMetrics {
    using namespace std::chrono_literals;

    settings.time.frame_index = frame_index;
    auto stats = renderer.render({
        .target_path = targetPath,
        .settings = settings,
        .surface = surface,
    });
    if (!stats) {
        auto message = stats.error().message.value_or("render failed");
        throw std::runtime_error(message);
    }

    auto metrics_base = std::string(targetPath.getPath()) + "/output/v1/common";
    FrameMetrics metrics{};
    metrics.render_ms = stats->render_ms;
    metrics.damage_ms = stats->damage_ms;
    metrics.encode_ms = stats->encode_ms;
    metrics.progressive_copy_ms = stats->progressive_copy_ms;
    metrics.publish_ms = stats->publish_ms;
    metrics.tiles = read_metric(space, metrics_base, "progressiveTilesUpdated");
    metrics.bytes = read_metric(space, metrics_base, "progressiveBytesCopied");

    static std::vector<std::uint8_t> present_buffer;
    auto const frame_bytes = surface.frame_bytes();
    std::span<std::uint8_t> framebuffer_span{};
    if (frame_bytes > 0) {
        if (present_buffer.size() < frame_bytes) {
            present_buffer.resize(frame_bytes);
        }
        framebuffer_span = std::span<std::uint8_t>{present_buffer.data(), frame_bytes};
    }

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    PathWindowView window_view;
    PathWindowView::PresentPolicy present_policy{};
    present_policy.auto_render_on_present = false;
    auto const now = std::chrono::steady_clock::now();
    auto const vsync_deadline = now + 16ms;

    PathWindowView::PresentRequest present_request{};
    present_request.now = now;
    present_request.vsync_deadline = vsync_deadline;
    present_request.framebuffer = framebuffer_span;
    present_request.dirty_tiles = std::span<std::size_t const>{dirty_tiles.data(), dirty_tiles.size()};
#if defined(__APPLE__)
    present_request.allow_iosurface_sharing = true;
#endif

    auto present_stats = window_view.present(surface, present_policy, present_request);
    metrics.present_ms = present_stats.present_ms;

    return metrics;
}

auto format_result(std::vector<FrameMetrics> const& frames) -> std::string {
    if (frames.empty()) {
        return "no frames recorded";
    }
    auto count = static_cast<double>(frames.size());
    double sum_ms = 0.0;
    double sum_damage = 0.0;
    double sum_encode = 0.0;
    double sum_progressive = 0.0;
    double sum_publish = 0.0;
    double sum_present = 0.0;
    double sum_tiles = 0.0;
    double sum_bytes = 0.0;
    double worst_ms = 0.0;
    for (auto const& frame : frames) {
        sum_ms += frame.render_ms;
        sum_damage += frame.damage_ms;
        sum_encode += frame.encode_ms;
        sum_progressive += frame.progressive_copy_ms;
        sum_publish += frame.publish_ms;
        sum_present += frame.present_ms;
        sum_tiles += static_cast<double>(frame.tiles);
        sum_bytes += static_cast<double>(frame.bytes);
        worst_ms = std::max(worst_ms, frame.render_ms);
    }
    double avg_ms = sum_ms / count;
    double avg_damage = sum_damage / count;
    double avg_encode = sum_encode / count;
    double avg_copy = sum_progressive / count;
    double avg_publish = sum_publish / count;
    double avg_present = sum_present / count;
    double avg_tiles = sum_tiles / count;
    double avg_bytes = sum_bytes / count;
    double fps = avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "frames=" << frames.size()
        << " avg_ms=" << avg_ms
        << " fps=" << fps
        << " worst_ms=" << worst_ms
        << " avg_damage_ms=" << avg_damage
        << " avg_encode_ms=" << avg_encode
        << " avg_copy_ms=" << avg_copy
        << " avg_publish_ms=" << avg_publish
        << " avg_present_ms=" << avg_present
        << " avg_tiles=" << avg_tiles
        << " avg_bytes=" << avg_bytes / 1'000'000.0 << "MB";
    return oss.str();
}

} // namespace

int main() try {
    constexpr int canvas_width = 3840;
    constexpr int canvas_height = 2160;
    constexpr int brush_size = 64;
    constexpr int incremental_frames = 48;

    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/bench_app"};
    auto root_view = SP::App::AppRootPathView{app_root.getPath()};

    // Scene setup
    Builders::SceneParams scene_params{
        .name = "benchmark_scene",
        .description = "Renderer benchmark scene",
    };
    auto scene_path = Builders::Scene::Create(space, root_view, scene_params);
    if (!scene_path) {
        throw std::runtime_error(scene_path.error().message.value_or("failed to create scene"));
    }

    UIScene::SceneSnapshotBuilder snapshot_builder{space, root_view, *scene_path};

    // Renderer + surface setup
    Builders::RendererParams renderer_params{
        .name = "renderer_bench",
        .description = "Benchmark renderer",
    };
    auto renderer_path = Builders::Renderer::Create(space, root_view, renderer_params, Builders::RendererKind::Software2D);
    if (!renderer_path) {
        throw std::runtime_error(renderer_path.error().message.value_or("failed to create renderer"));
    }

    Builders::SurfaceDesc surface_desc{};
    surface_desc.size_px.width = canvas_width;
    surface_desc.size_px.height = canvas_height;
    surface_desc.pixel_format = Builders::PixelFormat::BGRA8Unorm;
    surface_desc.color_space = Builders::ColorSpace::sRGB;
    surface_desc.premultiplied_alpha = true;

    Builders::SurfaceParams surface_params{};
    surface_params.name = "surface_bench";
    surface_params.desc = surface_desc;
    surface_params.renderer = renderer_params.name;
    auto surface_path = Builders::Surface::Create(space, root_view, surface_params);
    if (!surface_path) {
        throw std::runtime_error(surface_path.error().message.value_or("failed to create surface"));
    }

    auto set_scene = Builders::Surface::SetScene(space, *surface_path, *scene_path);
    if (!set_scene) {
        throw std::runtime_error(set_scene.error().message.value_or("failed to bind scene to surface"));
    }

    // Resolve target path
    auto target_rel = space.read<std::string, std::string>(std::string(surface_path->getPath()) + "/target");
    if (!target_rel) {
        throw std::runtime_error(target_rel.error().message.value_or("failed to read surface target"));
    }
    auto target_abs = SP::App::resolve_app_relative(root_view, SP::UnvalidatedPathView{target_rel->c_str()});
    if (!target_abs) {
        throw std::runtime_error(target_abs.error().message.value_or("failed to resolve surface target"));
    }
    auto target_path = *target_abs;
    auto target_path_view = Builders::ConcretePathView{target_path.getPath()};
    auto hints_path = std::string(target_path.getPath()) + "/hints/dirtyRects";

    PathRenderer2D renderer{space};
    PathSurfaceSoftware::Options surface_options{
        .enable_progressive = true,
        .enable_buffered = true,
        .progressive_tile_size_px = 64,
    };
    PathSurfaceSoftware surface{surface_desc, surface_options};

    Builders::RenderSettings render_settings{};
    render_settings.surface.size_px.width = canvas_width;
    render_settings.surface.size_px.height = canvas_height;
    render_settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

    std::vector<Stroke> strokes;
    strokes.reserve(512);
    std::uint64_t next_id = 1;

    auto add_background = [&]() {
        Stroke bg{};
        bg.drawable_id = next_id++;
        bg.rect.min_x = 0.0f;
        bg.rect.min_y = 0.0f;
        bg.rect.max_x = static_cast<float>(canvas_width);
        bg.rect.max_y = static_cast<float>(canvas_height);
        bg.rect.color = {0.1f, 0.1f, 0.12f, 1.0f};
        bg.authoring_id = "background";
        strokes.push_back(bg);
    };

    add_background();

    auto publish_scene = [&]() {
        auto bucket = build_bucket(strokes);
        UIScene::SnapshotPublishOptions opts{};
        opts.metadata.author = "path_renderer2d_benchmark";
        opts.metadata.tool_version = "bench";
        opts.metadata.created_at = std::chrono::system_clock::now();
        opts.metadata.drawable_count = bucket.drawable_ids.size();
        opts.metadata.command_count = bucket.command_kinds.size();
        auto published = snapshot_builder.publish(opts, bucket);
        if (!published) {
            throw std::runtime_error(published.error().message.value_or("failed to publish snapshot"));
        }
    };

    publish_scene();
    replace_value<std::vector<Builders::DirtyRectHint>>(space, hints_path, {});

    std::vector<FrameMetrics> full_frames;
    full_frames.reserve(4);
    std::vector<FrameMetrics> incremental_frames_metrics;
    incremental_frames_metrics.reserve(incremental_frames);

    std::uint64_t frame_index = 1;
    // Initial full repaint (background publish).
    full_frames.push_back(render_frame(renderer, surface, space, target_path_view, render_settings, frame_index++));

    // Simulate incremental brush strokes.
    std::mt19937 rng{0xC0FFEE};
    std::uniform_real_distribution<float> dist_x(0.0f, static_cast<float>(canvas_width - brush_size));
    std::uniform_real_distribution<float> dist_y(0.0f, static_cast<float>(canvas_height - brush_size));
    std::uniform_real_distribution<float> dist_color(0.2f, 1.0f);

    for (int i = 0; i < incremental_frames; ++i) {
        float min_x = dist_x(rng);
        float min_y = dist_y(rng);
        Stroke stroke{};
        stroke.drawable_id = next_id++;
        stroke.rect.min_x = min_x;
        stroke.rect.min_y = min_y;
        stroke.rect.max_x = min_x + brush_size;
        stroke.rect.max_y = min_y + brush_size;
        stroke.rect.color = {dist_color(rng), dist_color(rng), dist_color(rng), 1.0f};
        stroke.authoring_id = "stroke/" + std::to_string(stroke.drawable_id);
        strokes.push_back(stroke);

        publish_scene();

        Builders::DirtyRectHint hint{};
        hint.min_x = stroke.rect.min_x - 1.0f;
        hint.min_y = stroke.rect.min_y - 1.0f;
        hint.max_x = stroke.rect.max_x + 1.0f;
        hint.max_y = stroke.rect.max_y + 1.0f;
        std::vector<Builders::DirtyRectHint> hints{hint};
        replace_value(space, hints_path, hints);

        incremental_frames_metrics.push_back(
            render_frame(renderer, surface, space, target_path_view, render_settings, frame_index++));
    }

    // Force a full repaint by clearing hints and changing clear color.
    render_settings.clear_color = {0.02f, 0.02f, 0.02f, 1.0f};
    replace_value<std::vector<Builders::DirtyRectHint>>(space, hints_path, {});
    publish_scene();
    full_frames.push_back(render_frame(renderer, surface, space, target_path_view, render_settings, frame_index++));

    std::cout << "=== PathRenderer2D Benchmark ===" << std::endl;
    std::cout << "Canvas: " << canvas_width << "x" << canvas_height
              << " progressive tiles=" << surface.progressive_tile_count()
              << " initial tile size=" << surface.progressive_tile_size() << "px" << std::endl;
    std::cout << "Full repaint stats: " << format_result(full_frames) << std::endl;
    std::cout << "Incremental stroke stats: " << format_result(incremental_frames_metrics) << std::endl;

    // Small-surface diagnostic matching regression tests
    {
        PathSpace small_space;
        SP::App::AppRootPath small_root{"/system/applications/bench_small"};
        auto small_root_view = SP::App::AppRootPathView{small_root.getPath()};

        Builders::SceneParams sp_params{.name = "small_scene", .description = "Small surface diagnostics"};
        auto sp_scene = Builders::Scene::Create(small_space, small_root_view, sp_params);
        if (!sp_scene) {
            throw std::runtime_error(sp_scene.error().message.value_or("failed to create small scene"));
        }
        UIScene::SceneSnapshotBuilder sp_builder{small_space, small_root_view, *sp_scene};

        auto make_small_bucket = [](float origin_x, float origin_y) {
            UIScene::DrawableBucketSnapshot bucket{};
            bucket.drawable_ids = {0xABCDEFu};
            bucket.world_transforms = {identity_transform()};
            bucket.bounds_boxes = {
                UIScene::BoundingBox{{origin_x, origin_y, 0.0f},
                                     {origin_x + 2.0f, origin_y + 2.0f, 0.0f}},
            };
            bucket.bounds_box_valid = {1};
            bucket.bounds_spheres = {
                UIScene::BoundingSphere{{origin_x + 1.0f, origin_y + 1.0f, 0.0f}, 1.5f},
            };
            bucket.layers = {0};
            bucket.z_values = {0.0f};
            bucket.material_ids = {0};
            bucket.pipeline_flags = {0};
            bucket.visibility = {1};
            bucket.command_offsets = {0};
            bucket.command_counts = {1};
            bucket.opaque_indices = {0};
            bucket.alpha_indices.clear();
            bucket.clip_head_indices = {-1};
            bucket.authoring_map = {
                UIScene::DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0},
            };

            UIScene::RectCommand rect{
                .min_x = origin_x,
                .min_y = origin_y,
                .max_x = origin_x + 2.0f,
                .max_y = origin_y + 2.0f,
                .color = {0.4f, 0.2f, 0.9f, 1.0f},
            };
            auto offset = bucket.command_payload.size();
            bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
            std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(UIScene::RectCommand));
            bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));
            return bucket;
        };

        auto small_bucket = make_small_bucket(0.0f, 0.0f);
        UIScene::SnapshotPublishOptions sp_opts{};
        sp_opts.metadata.author = "path_renderer2d_benchmark";
        sp_opts.metadata.tool_version = "bench";
        sp_opts.metadata.drawable_count = 1;
        sp_opts.metadata.command_count = 1;
        if (!sp_builder.publish(sp_opts, small_bucket)) {
            throw std::runtime_error("failed to publish small snapshot");
        }

        Builders::RendererParams sp_renderer_params{.name = "small_renderer", .description = ""};
        auto sp_renderer = Builders::Renderer::Create(small_space, small_root_view, sp_renderer_params, Builders::RendererKind::Software2D);
        if (!sp_renderer) {
            throw std::runtime_error("failed to create small renderer");
        }

        Builders::SurfaceDesc sp_desc{};
        sp_desc.size_px.width = 8;
        sp_desc.size_px.height = 8;
        sp_desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
        sp_desc.color_space = Builders::ColorSpace::sRGB;
        sp_desc.premultiplied_alpha = true;

        Builders::SurfaceParams sp_surface_params{};
        sp_surface_params.name = "small_surface";
        sp_surface_params.desc = sp_desc;
        sp_surface_params.renderer = sp_renderer_params.name;
        auto sp_surface_path = Builders::Surface::Create(small_space, small_root_view, sp_surface_params);
        if (!sp_surface_path) {
            throw std::runtime_error("failed to create small surface");
        }

        if (auto set_scene_result = Builders::Surface::SetScene(small_space, *sp_surface_path, *sp_scene); !set_scene_result) {
            throw std::runtime_error("failed to bind scene for small surface");
        }

        auto sp_target_rel = small_space.read<std::string, std::string>(std::string(sp_surface_path->getPath()) + "/target");
        auto sp_target_abs = SP::App::resolve_app_relative(small_root_view, SP::UnvalidatedPathView{sp_target_rel->c_str()});
        auto sp_target_view = Builders::ConcretePathView{sp_target_abs->getPath()};

        PathRenderer2D sp_renderer_inst{small_space};
        PathSurfaceSoftware::Options sp_opts_surface{.enable_progressive = true, .enable_buffered = false, .progressive_tile_size_px = 2};
        PathSurfaceSoftware sp_surface{sp_desc, sp_opts_surface};

        Builders::RenderSettings sp_settings{};
        sp_settings.surface.size_px.width = sp_desc.size_px.width;
        sp_settings.surface.size_px.height = sp_desc.size_px.height;
        sp_settings.clear_color = {0.1f, 0.1f, 0.1f, 1.0f};

        auto render_small = [&](std::uint64_t frame_index) {
            sp_settings.time.frame_index = frame_index;
            auto stats = sp_renderer_inst.render({
                .target_path = sp_target_view,
                .settings = sp_settings,
                .surface = sp_surface,
            });
            if (!stats) {
                throw std::runtime_error("small surface render failed");
            }
        };

        render_small(1);
        (void)sp_surface.consume_progressive_dirty_tiles();

        auto moved_bucket = make_small_bucket(6.0f, 6.0f);
        if (!sp_builder.publish(sp_opts, moved_bucket)) {
            throw std::runtime_error("failed to publish moved small snapshot");
        }

        render_small(2);
        auto small_tiles = sp_surface.consume_progressive_dirty_tiles();
        std::sort(small_tiles.begin(), small_tiles.end());
        std::cout << "Small-surface tiles: ";
        for (auto tile : small_tiles) {
            std::cout << tile << ' ';
        }
        std::cout << "(count=" << small_tiles.size() << ")" << std::endl;
    }

    return 0;
} catch (std::exception const& ex) {
    std::cerr << "Benchmark failed: " << ex.what() << std::endl;
    return 1;
}
