#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include "PaintInputBridge.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace SP;
using namespace SP::UI;
namespace UIScene = SP::UI::Scene;

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
namespace SP {
void PSInitLocalEventWindow();
void PSInitLocalEventWindowWithSize(int width, int height, const char* title);
void PSPollLocalEventWindow();
void PSUpdateWindowFramebuffer(const std::uint8_t* data, int width, int height, int rowStrideBytes);
void PSPresentIOSurface(void* surface, int width, int height, int rowStrideBytes);
void PSGetLocalWindowContentSize(int* width, int* height);
} // namespace SP
#endif

namespace {

using DirtyRectHint = Builders::DirtyRectHint;

auto align_down_to_tile(float value, int tileSizePx) -> float {
    auto const tile = static_cast<float>(std::max(1, tileSizePx));
    return std::floor(value / tile) * tile;
}

auto align_up_to_tile(float value, int tileSizePx) -> float {
    auto const tile = static_cast<float>(std::max(1, tileSizePx));
    return std::ceil(value / tile) * tile;
}

auto clamp_and_align_hint(DirtyRectHint const& hint,
                          int canvasWidth,
                          int canvasHeight,
                          int tileSizePx) -> std::optional<DirtyRectHint> {
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        return std::nullopt;
    }

    auto const maxX = static_cast<float>(canvasWidth);
    auto const maxY = static_cast<float>(canvasHeight);
    auto const minX = std::clamp(hint.min_x, 0.0f, maxX);
    auto const minY = std::clamp(hint.min_y, 0.0f, maxY);
    auto const alignedMaxX = std::clamp(align_up_to_tile(std::clamp(hint.max_x, 0.0f, maxX), tileSizePx), 0.0f, maxX);
    auto const alignedMaxY = std::clamp(align_up_to_tile(std::clamp(hint.max_y, 0.0f, maxY), tileSizePx), 0.0f, maxY);
    auto const alignedMinX = std::clamp(align_down_to_tile(minX, tileSizePx), 0.0f, maxX);
    auto const alignedMinY = std::clamp(align_down_to_tile(minY, tileSizePx), 0.0f, maxY);

    if (alignedMaxX <= alignedMinX || alignedMaxY <= alignedMinY) {
        return std::nullopt;
    }

return DirtyRectHint{
        .min_x = alignedMinX,
        .min_y = alignedMinY,
        .max_x = alignedMaxX,
        .max_y = alignedMaxY,
    };
}

template <typename T>
auto replace_value(PathSpace& space, std::string const& path, T const& value) -> bool {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& err = taken.error();
        if (err.code == SP::Error::Code::NoObjectFound
            || err.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        std::cerr << "failed clearing '" << path << "': ";
        if (err.message.has_value()) {
            std::cerr << *err.message;
        } else {
            std::cerr << static_cast<int>(err.code);
        }
        std::cerr << std::endl;
        return false;
    }

    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        auto const& err = result.errors.front();
        std::cerr << "failed writing '" << path << "': ";
        if (err.message.has_value()) {
            std::cerr << *err.message;
        } else {
            std::cerr << static_cast<int>(err.code);
        }
        std::cerr << std::endl;
        return false;
    }
    return true;
}

void ensure_config_value(PathSpace& space,
                         std::string const& path,
                         int defaultValue) {
    auto value = space.read<int>(path);
    if (value) {
        return;
    }
    auto const& err = value.error();
    if (err.code == SP::Error::Code::NoObjectFound
        || err.code == SP::Error::Code::NoSuchPath) {
        replace_value(space, path, defaultValue);
    }
}

auto read_config_value(PathSpace& space,
                       std::string const& path,
                       int fallback) -> int {
    auto value = space.read<int>(path);
    if (value) {
        return std::max(1, *value);
    }
    return std::max(1, fallback);
}

void coalesce_dirty_hints(std::vector<DirtyRectHint>& hints,
                          int canvasWidth,
                          int canvasHeight,
                          int tileSizePx) {
    std::vector<DirtyRectHint> normalized;
    normalized.reserve(hints.size());
    for (auto const& hint : hints) {
        if (auto normalizedHint = clamp_and_align_hint(hint, canvasWidth, canvasHeight, tileSizePx)) {
            normalized.push_back(*normalizedHint);
        }
    }
    if (normalized.empty()) {
        hints.clear();
        return;
    }

    std::vector<DirtyRectHint> merged;
    merged.reserve(normalized.size());
    for (auto const& hint : normalized) {
        bool mergedExisting = false;
        for (auto& existing : merged) {
            auto const xOverlap = !(hint.max_x <= existing.min_x || hint.min_x >= existing.max_x);
            auto const yOverlap = !(hint.max_y <= existing.min_y || hint.min_y >= existing.max_y);
            if (xOverlap && yOverlap) {
                existing.min_x = std::min(existing.min_x, hint.min_x);
                existing.min_y = std::min(existing.min_y, hint.min_y);
                existing.max_x = std::max(existing.max_x, hint.max_x);
                existing.max_y = std::max(existing.max_y, hint.max_y);
                mergedExisting = true;
                break;
            }
        }
        if (!mergedExisting) {
            merged.push_back(hint);
        }
    }

    hints = std::move(merged);
}

struct RuntimeOptions {
    bool debug = false;
};

auto parse_runtime_options(int argc, char** argv) -> RuntimeOptions {
    RuntimeOptions opts{};
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--debug") {
            opts.debug = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: paint_example [--debug]\n";
            std::exit(0);
        }
    }
    return opts;
}

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
auto unwrap_or_exit(SP::Expected<T> value, std::string const& context) -> T {
    if (!value) {
        std::cerr << context;
        if (value.error().message.has_value()) {
            std::cerr << ": " << *value.error().message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
    return *std::move(value);
}

auto unwrap_or_exit(SP::Expected<void> value, std::string const& context) -> void {
    if (!value) {
        std::cerr << context;
        if (value.error().message.has_value()) {
            std::cerr << ": " << *value.error().message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
}

auto build_bucket(std::vector<Stroke> const& strokes) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    const std::size_t count = strokes.size();

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
    bucket.command_kinds.reserve(count);
    bucket.clip_head_indices.assign(count, -1);
    bucket.authoring_map.reserve(count);
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

        auto previousSize = bucket.command_payload.size();
        bucket.command_payload.resize(previousSize + sizeof(UIScene::RectCommand));
        std::memcpy(bucket.command_payload.data() + previousSize, &stroke.rect, sizeof(UIScene::RectCommand));

        bucket.authoring_map.push_back(UIScene::DrawableAuthoringMapEntry{
            stroke.drawable_id,
            stroke.authoring_id,
            0,
            0});
    }

    bucket.opaque_indices.resize(count);
    std::iota(bucket.opaque_indices.begin(), bucket.opaque_indices.end(), 0);
    bucket.alpha_indices.clear();

    return bucket;
}

auto publish_snapshot(PathSpace& space,
                      UIScene::SceneSnapshotBuilder& builder,
                      UIScene::ScenePath const& scenePath,
                      UIScene::DrawableBucketSnapshot const& bucket) -> void {
    UIScene::SnapshotPublishOptions opts{};
    opts.metadata.author = "paint_example";
    opts.metadata.tool_version = "paint_example";
    opts.metadata.created_at = std::chrono::system_clock::now();
    opts.metadata.drawable_count = bucket.drawable_ids.size();
    opts.metadata.command_count = bucket.command_kinds.size();
    unwrap_or_exit(builder.publish(opts, bucket), "failed to publish paint scene snapshot");
}

struct PresentOutcome {
    bool used_iosurface = false;
    std::size_t framebuffer_bytes = 0;
    std::size_t stride_bytes = 0;
};

auto present_frame(PathSpace& space,
                   Builders::WindowPath const& windowPath,
                   std::string_view viewName,
                   int width,
                   int height,
                   bool debug) -> std::optional<PresentOutcome> {
    auto presentResult = Builders::Window::Present(space, windowPath, viewName);
    if (!presentResult) {
        std::cerr << "present failed";
        if (presentResult.error().message.has_value()) {
            std::cerr << ": " << *presentResult.error().message;
        }
        std::cerr << std::endl;
        return std::nullopt;
    }
#if defined(__APPLE__)
    bool used_iosurface = false;
    std::size_t computed_stride = 0;
    if (presentResult->stats.iosurface && presentResult->stats.iosurface->valid()) {
        auto iosurface_ref = presentResult->stats.iosurface->retain_for_external_use();
        if (iosurface_ref) {
            SP::PSPresentIOSurface(static_cast<void*>(iosurface_ref),
                                   width,
                                   height,
                                   static_cast<int>(presentResult->stats.iosurface->row_bytes()));
            CFRelease(iosurface_ref);
            used_iosurface = true;
            computed_stride = presentResult->stats.iosurface->row_bytes();
        }
    }
    if (!used_iosurface && !presentResult->framebuffer.empty()) {
        int row_stride_bytes = 0;
        if (height > 0) {
            auto total_bytes = static_cast<int>(presentResult->framebuffer.size());
            row_stride_bytes = total_bytes / height;
        }
        if (row_stride_bytes <= 0) {
            row_stride_bytes = width * 4;
        }
        computed_stride = static_cast<std::size_t>(row_stride_bytes);
        SP::PSUpdateWindowFramebuffer(presentResult->framebuffer.data(),
                                      width,
                                      height,
                                      row_stride_bytes);
    }
#else
    (void)width;
    (void)height;
    std::size_t computed_stride = static_cast<std::size_t>(width) * 4;
#endif
    PresentOutcome outcome{};
    outcome.used_iosurface = presentResult->stats.used_iosurface;
    outcome.framebuffer_bytes = presentResult->framebuffer.size();
    if (computed_stride == 0) {
        computed_stride = static_cast<std::size_t>(width) * 4;
    }
    outcome.stride_bytes = computed_stride;

    if (debug) {
        auto const& stats = presentResult->stats;
        std::cout << "[present] frame=" << stats.frame.frame_index
                  << " render_ms=" << stats.frame.render_ms
                  << " present_ms=" << stats.present_ms
                  << " tiles=" << stats.progressive_tiles_copied
                  << " rects=" << stats.progressive_rects_coalesced
                  << " skipped=" << stats.skipped
                  << " buffered=" << stats.buffered_frame_consumed
                  << " dirty_bytes=" << outcome.framebuffer_bytes
                  << " stride=" << outcome.stride_bytes
                  << std::endl;
    }
    return outcome;
}

auto to_canvas_y(int viewY, int canvasHeight) -> int {
    return std::clamp(viewY, 0, std::max(canvasHeight - 1, 0));
}

auto add_stroke(std::vector<Stroke>& strokes,
                std::uint64_t& nextId,
                int canvasWidth,
                int canvasHeight,
                int x,
                int y,
                std::array<float, 4> const& color,
                int brushSizePx) -> std::optional<DirtyRectHint> {
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        return std::nullopt;
    }
    int canvasX = std::clamp(x, 0, canvasWidth - 1);
    int canvasY = to_canvas_y(y, canvasHeight);
    float half = static_cast<float>(brushSizePx) * 0.5f;
    float minX = std::clamp(static_cast<float>(canvasX) - half, 0.0f, static_cast<float>(canvasWidth));
    float minY = std::clamp(static_cast<float>(canvasY) - half, 0.0f, static_cast<float>(canvasHeight));
    float maxX = std::clamp(minX + static_cast<float>(brushSizePx), 0.0f, static_cast<float>(canvasWidth));
    float maxY = std::clamp(minY + static_cast<float>(brushSizePx), 0.0f, static_cast<float>(canvasHeight));
    if (maxX <= minX || maxY <= minY) {
        return std::nullopt;
    }

    UIScene::RectCommand rectCmd{};
    rectCmd.min_x = minX;
    rectCmd.min_y = minY;
    rectCmd.max_x = maxX;
    rectCmd.max_y = maxY;
    rectCmd.color = color;

    Stroke stroke{
        .drawable_id = nextId++,
        .rect = rectCmd,
        .authoring_id = "nodes/paint/stroke_" + std::to_string(strokes.size()),
    };
    strokes.push_back(std::move(stroke));
    return DirtyRectHint{
        .min_x = minX,
        .min_y = minY,
        .max_x = maxX,
        .max_y = maxY,
    };
}

auto lay_down_segment(std::vector<Stroke>& strokes,
                      std::uint64_t& nextId,
                      int canvasWidth,
                      int canvasHeight,
                      std::pair<int, int> const& from,
                      std::pair<int, int> const& to,
                      std::array<float, 4> const& color,
                      std::vector<DirtyRectHint>& dirtyHints,
                      int brushSizePx) -> bool {
    bool wrote = false;
    DirtyRectHint segmentBounds{};
    bool haveBounds = false;

    auto accumulate_hint = [&](DirtyRectHint const& hint) {
        if (!haveBounds) {
            segmentBounds = hint;
            haveBounds = true;
        } else {
            segmentBounds.min_x = std::min(segmentBounds.min_x, hint.min_x);
            segmentBounds.min_y = std::min(segmentBounds.min_y, hint.min_y);
            segmentBounds.max_x = std::max(segmentBounds.max_x, hint.max_x);
            segmentBounds.max_y = std::max(segmentBounds.max_y, hint.max_y);
        }
    };
    double x0 = static_cast<double>(from.first);
    double y0 = static_cast<double>(from.second);
    double x1 = static_cast<double>(to.first);
    double y1 = static_cast<double>(to.second);
    double dx = x1 - x0;
    double dy = y1 - y0;
    double dist = std::hypot(dx, dy);
    double spacing = std::max(1.0, static_cast<double>(brushSizePx) * 0.5);
    int steps = (dist > spacing) ? static_cast<int>(std::floor(dist / spacing)) : 0;
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(steps + 1);
        int xi = static_cast<int>(std::round(x0 + dx * t));
        int yi = static_cast<int>(std::round(y0 + dy * t));
        if (auto hint = add_stroke(strokes,
                                   nextId,
                                   canvasWidth,
                                   canvasHeight,
                                   xi,
                                   yi,
                                   color,
                                   brushSizePx)) {
            accumulate_hint(*hint);
            wrote = true;
        }
    }
    if (auto hint = add_stroke(strokes,
                               nextId,
                               canvasWidth,
                               canvasHeight,
                               to.first,
                               to.second,
                               color,
                               brushSizePx)) {
        accumulate_hint(*hint);
        wrote = true;
    }
    if (wrote && haveBounds) {
        dirtyHints.push_back(segmentBounds);
    }
    return wrote;
}

} // namespace

int main(int argc, char** argv) {
#if !defined(__APPLE__)
    std::cerr << "paint_example currently supports only macOS builds." << std::endl;
    return 1;
#else
    auto options = parse_runtime_options(argc, argv);
    PathSpace space;
    SP::App::AppRootPath appRoot{"/system/applications/paint"};
    auto rootView = SP::App::AppRootPathView{appRoot.getPath()};

    const std::string configBasePath = std::string(rootView.getPath()) + "/config";
    const std::string canvasWidthPath = configBasePath + "/canvasWidthPx";
    const std::string canvasHeightPath = configBasePath + "/canvasHeightPx";
    const std::string brushSizePath = configBasePath + "/brushSizePx";
    const std::string tileSizePath = configBasePath + "/progressiveTileSizePx";

    ensure_config_value(space, canvasWidthPath, 320);
    ensure_config_value(space, canvasHeightPath, 240);
    ensure_config_value(space, brushSizePath, 8);
    ensure_config_value(space, tileSizePath, 64);

    int canvasWidth = read_config_value(space, canvasWidthPath, 320);
    int canvasHeight = read_config_value(space, canvasHeightPath, 240);

    SP::PSInitLocalEventWindowWithSize(canvasWidth, canvasHeight, "PathSpace Paint");

    Builders::SceneParams sceneParams{
        .name = "canvas",
        .description = "paint example canvas",
    };
    auto scenePath = unwrap_or_exit(Builders::Scene::Create(space, rootView, sceneParams),
                                    "failed to create paint scene");

    Builders::RendererParams rendererParams{
        .name = "software2d",
        .description = "paint renderer",
    };
    auto rendererPath = unwrap_or_exit(Builders::Renderer::Create(space, rootView, rendererParams, Builders::RendererKind::Software2D),
                                       "failed to create renderer");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = canvasWidth;
    surfaceDesc.size_px.height = canvasHeight;
    surfaceDesc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = Builders::ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    Builders::SurfaceParams surfaceParams{};
    surfaceParams.name = "canvas_surface";
    surfaceParams.desc = surfaceDesc;
    surfaceParams.renderer = rendererPath.getPath();

    auto surfacePath = unwrap_or_exit(Builders::Surface::Create(space, rootView, surfaceParams),
                                      "failed to create surface");
    unwrap_or_exit(Builders::Surface::SetScene(space, surfacePath, scenePath),
                   "failed to bind scene to surface");

    auto targetRelative = unwrap_or_exit(space.read<std::string, std::string>(std::string(surfacePath.getPath()) + "/target"),
                                         "failed to read surface target binding");
    auto targetAbsolute = unwrap_or_exit(SP::App::resolve_app_relative(rootView, targetRelative),
                                         "failed to resolve surface target path");
    std::string surfaceDescPath = std::string(surfacePath.getPath()) + "/desc";
    std::string targetDescPath = targetAbsolute.getPath() + "/desc";

    Builders::WindowParams windowParams{};
    windowParams.name = "window";
    windowParams.title = "PathSpace Paint";
    windowParams.width = canvasWidth;
    windowParams.height = canvasHeight;
    auto windowPath = unwrap_or_exit(Builders::Window::Create(space, rootView, windowParams),
                                     "failed to create window");
    unwrap_or_exit(Builders::Window::AttachSurface(space, windowPath, "main", surfacePath),
                   "failed to attach surface to window");

    UIScene::SceneSnapshotBuilder builder{space, rootView, scenePath};

    std::vector<Stroke> strokes;
    std::uint64_t nextId = 1;

    Builders::RenderSettings rendererSettings{};
    rendererSettings.clear_color = {1.0f, 1.0f, 1.0f, 1.0f};
    rendererSettings.surface.size_px.width = canvasWidth;
    rendererSettings.surface.size_px.height = canvasHeight;
    unwrap_or_exit(Builders::Renderer::UpdateSettings(space,
                                                     SP::ConcretePathStringView{targetAbsolute.getPath()},
                                                     rendererSettings),
                  "failed to set renderer clear color");

    auto bucket = build_bucket(strokes);
    publish_snapshot(space, builder, scenePath, bucket);
    (void)present_frame(space,
                       windowPath,
                       "main",
                       canvasWidth,
                       canvasHeight,
                       options.debug);

    auto fps_last_report = std::chrono::steady_clock::now();
    std::uint64_t fps_frames = 0;
    std::uint64_t fps_iosurface_frames = 0;
    std::size_t fps_last_stride = 0;
    std::size_t fps_last_framebuffer_bytes = 0;

    bool drawing = false;
    std::optional<std::pair<int, int>> lastAbsolute;
    std::optional<std::pair<int, int>> lastPainted;
    std::array<float, 4> brushColor{0.9f, 0.1f, 0.3f, 1.0f};
    std::vector<DirtyRectHint> dirtyHints;

    while (true) {
        SP::PSPollLocalEventWindow();

        int requestedWidth = canvasWidth;
        int requestedHeight = canvasHeight;
        SP::PSGetLocalWindowContentSize(&requestedWidth, &requestedHeight);
        if (requestedWidth <= 0 || requestedHeight <= 0) {
            break;
        }

        bool updated = false;
        dirtyHints.clear();

        const int brushSizePx = read_config_value(space, brushSizePath, 8);
        const int progressiveTileSizePx = read_config_value(space, tileSizePath, 64);

        bool sizeChanged = (requestedWidth != canvasWidth) || (requestedHeight != canvasHeight);
        if (sizeChanged) {
            canvasWidth = requestedWidth;
            canvasHeight = requestedHeight;
            surfaceDesc.size_px.width = canvasWidth;
            surfaceDesc.size_px.height = canvasHeight;
            replace_value(space, surfaceDescPath, surfaceDesc);
            replace_value(space, targetDescPath, surfaceDesc);
            replace_value(space, canvasWidthPath, canvasWidth);
            replace_value(space, canvasHeightPath, canvasHeight);
            lastPainted.reset();
            lastAbsolute.reset();
            rendererSettings.surface.size_px.width = canvasWidth;
            rendererSettings.surface.size_px.height = canvasHeight;
            unwrap_or_exit(Builders::Renderer::UpdateSettings(space,
                                                             SP::ConcretePathStringView{targetAbsolute.getPath()},
                                                             rendererSettings),
                          "failed to refresh renderer size on resize");
            dirtyHints.push_back(DirtyRectHint{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = static_cast<float>(canvasWidth),
                .max_y = static_cast<float>(canvasHeight),
            });
            updated = true;
        }
        while (auto evt = PaintInput::try_pop_mouse()) {
            auto const& e = *evt;
            switch (e.type) {
            case PaintInput::MouseEventType::AbsoluteMove: {
                if (e.x < 0 || e.y < 0) {
                    break;
                }
                std::pair<int, int> current{e.x, e.y};
                lastAbsolute = current;
                if (drawing) {
                    if (!lastPainted) {
                        lastPainted = current;
                    }
                    updated |= lay_down_segment(strokes,
                                                nextId,
                                                canvasWidth,
                                                canvasHeight,
                                                *lastPainted,
                                                current,
                                                brushColor,
                                                dirtyHints,
                                                brushSizePx);
                    lastPainted = current;
                }
                break;
            }
            case PaintInput::MouseEventType::ButtonDown:
                if (e.button == PaintInput::MouseButton::Left) {
                    std::optional<std::pair<int, int>> point;
                    if (e.x >= 0 && e.y >= 0) {
                        point = std::pair<int, int>{e.x, e.y};
                    } else if (lastAbsolute) {
                        point = lastAbsolute;
                    }
                    if (point) {
                        lastAbsolute = *point;
                        drawing = true;
                        if (auto hint = add_stroke(strokes,
                                                   nextId,
                                                   canvasWidth,
                                                   canvasHeight,
                                                   point->first,
                                                   point->second,
                                                   brushColor,
                                                   brushSizePx)) {
                            dirtyHints.push_back(*hint);
                            updated = true;
                        }
                        lastPainted = *point;
                    }
                }
                break;
            case PaintInput::MouseEventType::ButtonUp:
                if (e.button == PaintInput::MouseButton::Left) {
                    drawing = false;
                    lastPainted.reset();
                }
                break;
            case PaintInput::MouseEventType::Move:
            case PaintInput::MouseEventType::Wheel:
                // Ignored for painting.
                break;
            }
        }

        if (updated) {
            bucket = build_bucket(strokes);
            publish_snapshot(space, builder, scenePath, bucket);
        }

        coalesce_dirty_hints(dirtyHints, canvasWidth, canvasHeight, progressiveTileSizePx);

        if (updated || sizeChanged) {
            if (!dirtyHints.empty()) {
                unwrap_or_exit(Builders::Renderer::SubmitDirtyRects(space,
                                                                    SP::ConcretePathStringView{targetAbsolute.getPath()},
                                                                    std::span<const DirtyRectHint>{dirtyHints}),
                               "failed to submit renderer dirty hints");
            }
            if (auto outcome = present_frame(space,
                                             windowPath,
                                             "main",
                                             canvasWidth,
                                             canvasHeight,
                                             options.debug)) {
                ++fps_frames;
                if (outcome->used_iosurface) {
                    ++fps_iosurface_frames;
                }
                fps_last_stride = outcome->stride_bytes;
                fps_last_framebuffer_bytes = outcome->framebuffer_bytes;
                auto report_now = std::chrono::steady_clock::now();
                auto elapsed = report_now - fps_last_report;
                if (elapsed >= std::chrono::seconds(1)) {
                    double seconds = std::chrono::duration<double>(elapsed).count();
                    if (seconds > 0.0 && fps_frames > 0) {
                        double fps = static_cast<double>(fps_frames) / seconds;
                        auto iosurface_frames = fps_iosurface_frames;
                        auto frames = fps_frames;
                        auto stride_bytes = fps_last_stride;
                        auto framebuffer_bytes = fps_last_framebuffer_bytes;
                        std::cout << "FPS: " << fps
                                  << " (iosurface " << iosurface_frames << '/' << frames
                                  << ", stride=" << stride_bytes
                                  << ", frameBytes=" << framebuffer_bytes
                                  << ')'
                                  << std::endl;
                    }
                    fps_frames = 0;
                    fps_iosurface_frames = 0;
                    fps_last_report = report_now;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    PaintInput::clear_mouse();
    return 0;
#endif
}
