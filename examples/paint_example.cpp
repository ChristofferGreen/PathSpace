#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>

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
namespace SP {
void PSInitLocalEventWindow(PathIOMouse*, PathIOKeyboard*);
void PSInitLocalEventWindowWithSize(PathIOMouse*, PathIOKeyboard*, int width, int height, const char* title);
void PSPollLocalEventWindow();
void PSUpdateWindowFramebuffer(const std::uint8_t* data, int width, int height, int rowStrideBytes);
} // namespace SP
#endif

namespace {

constexpr int kCanvasWidth = 320;
constexpr int kCanvasHeight = 240;
constexpr int kBrushSizePx = 8;

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
    std::size_t payload_offset = 0;
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

        bucket.command_offsets.push_back(static_cast<std::uint32_t>(payload_offset));
        bucket.command_counts.push_back(1);
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

        auto previousSize = bucket.command_payload.size();
        bucket.command_payload.resize(previousSize + sizeof(UIScene::RectCommand));
        std::memcpy(bucket.command_payload.data() + previousSize, &stroke.rect, sizeof(UIScene::RectCommand));
        payload_offset += sizeof(UIScene::RectCommand);

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

auto present_frame(PathSpace& space,
                   Builders::WindowPath const& windowPath,
                   std::string_view viewName,
                   int width,
                   int height) -> void {
    auto presentResult = Builders::Window::Present(space, windowPath, viewName);
    if (!presentResult) {
        std::cerr << "present failed";
        if (presentResult.error().message.has_value()) {
            std::cerr << ": " << *presentResult.error().message;
        }
        std::cerr << std::endl;
        return;
    }
    if (!presentResult->framebuffer.empty()) {
#if defined(__APPLE__)
        SP::PSUpdateWindowFramebuffer(presentResult->framebuffer.data(),
                                      width,
                                      height,
                                      width * 4);
#endif
    }
}

auto to_canvas_y(int viewY) -> int {
    int clamped = std::clamp(viewY, 0, kCanvasHeight - 1);
    return (kCanvasHeight - 1) - clamped;
}

auto add_stroke(std::vector<Stroke>& strokes,
                std::uint64_t& nextId,
                int x,
                int y,
                std::array<float, 4> const& color) -> bool {
    int canvasX = std::clamp(x, 0, kCanvasWidth - 1);
    int canvasY = to_canvas_y(y);
    float half = static_cast<float>(kBrushSizePx) * 0.5f;
    float minX = std::clamp(static_cast<float>(canvasX) - half, 0.0f, static_cast<float>(kCanvasWidth));
    float minY = std::clamp(static_cast<float>(canvasY) - half, 0.0f, static_cast<float>(kCanvasHeight));
    float maxX = std::clamp(minX + static_cast<float>(kBrushSizePx), 0.0f, static_cast<float>(kCanvasWidth));
    float maxY = std::clamp(minY + static_cast<float>(kBrushSizePx), 0.0f, static_cast<float>(kCanvasHeight));
    if (maxX <= minX || maxY <= minY) {
        return false;
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
    return true;
}

} // namespace

int main() {
#if !defined(__APPLE__)
    std::cerr << "paint_example currently supports only macOS builds." << std::endl;
    return 1;
#else
    PathSpace space;

    auto mouse = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Off);
    auto keyboard = std::make_unique<PathIOKeyboard>(PathIOKeyboard::BackendMode::Off);
    PathIOMouse* mousePtr = mouse.get();
    PathIOKeyboard* keyboardPtr = keyboard.get();

    auto insert_mouse = space.insert<"/system/devices/in/pointer/default">(std::move(mouse));
    if (!insert_mouse.errors.empty()) {
        std::cerr << "failed to mount mouse provider: ";
        if (insert_mouse.errors.front().message.has_value()) {
            std::cerr << *insert_mouse.errors.front().message;
        }
        std::cerr << std::endl;
        return 1;
    }
    auto insert_keyboard = space.insert<"/system/devices/in/text/default">(std::move(keyboard));
    if (!insert_keyboard.errors.empty()) {
        std::cerr << "failed to mount keyboard provider: ";
        if (insert_keyboard.errors.front().message.has_value()) {
            std::cerr << *insert_keyboard.errors.front().message;
        }
        std::cerr << std::endl;
        return 1;
    }

    SP::PSInitLocalEventWindowWithSize(mousePtr, keyboardPtr, kCanvasWidth, kCanvasHeight, "PathSpace Paint");

    SP::App::AppRootPath appRoot{"/system/applications/paint"};
    auto rootView = SP::App::AppRootPathView{appRoot.getPath()};

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
    surfaceDesc.size_px.width = kCanvasWidth;
    surfaceDesc.size_px.height = kCanvasHeight;
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

    Builders::WindowParams windowParams{};
    windowParams.name = "window";
    windowParams.title = "PathSpace Paint";
    windowParams.width = kCanvasWidth;
    windowParams.height = kCanvasHeight;
    auto windowPath = unwrap_or_exit(Builders::Window::Create(space, rootView, windowParams),
                                     "failed to create window");
    unwrap_or_exit(Builders::Window::AttachSurface(space, windowPath, "main", surfacePath),
                   "failed to attach surface to window");

    UIScene::SceneSnapshotBuilder builder{space, rootView, scenePath};

    std::vector<Stroke> strokes;
    std::uint64_t nextId = 1;
    // Background stroke (white)
    UIScene::RectCommand background{};
    background.min_x = 0.0f;
    background.min_y = 0.0f;
    background.max_x = static_cast<float>(kCanvasWidth);
    background.max_y = static_cast<float>(kCanvasHeight);
    background.color = {1.0f, 1.0f, 1.0f, 1.0f};
    Stroke backgroundStroke{
        .drawable_id = nextId++,
        .rect = background,
        .authoring_id = "nodes/paint/background",
    };
    strokes.push_back(backgroundStroke);

    auto bucket = build_bucket(strokes);
    publish_snapshot(space, builder, scenePath, bucket);
    present_frame(space, windowPath, "main", kCanvasWidth, kCanvasHeight);

    bool drawing = false;
    std::optional<std::pair<int, int>> lastAbsolute;
    std::array<float, 4> brushColor{0.9f, 0.1f, 0.3f, 1.0f};

    while (true) {
        SP::PSPollLocalEventWindow();

        bool updated = false;
        while (true) {
            auto evt = space.take<"/system/devices/in/pointer/default/events", PathIOMouse::Event>();
            if (!evt) {
                break;
            }
            auto const& e = *evt;
            switch (e.type) {
            case MouseEventType::AbsoluteMove:
                lastAbsolute = std::pair<int, int>{e.x, e.y};
                if (drawing && lastAbsolute) {
                    updated |= add_stroke(strokes, nextId, lastAbsolute->first, lastAbsolute->second, brushColor);
                }
                break;
            case MouseEventType::ButtonDown:
                if (e.button == MouseButton::Left) {
                    drawing = true;
                    if (lastAbsolute) {
                        updated |= add_stroke(strokes, nextId, lastAbsolute->first, lastAbsolute->second, brushColor);
                    }
                }
                break;
            case MouseEventType::ButtonUp:
                if (e.button == MouseButton::Left) {
                    drawing = false;
                }
                break;
            case MouseEventType::Move:
            case MouseEventType::Wheel:
                // Ignore relative move/wheel for painting.
                break;
            }
        }

        if (updated) {
            bucket = build_bucket(strokes);
            publish_snapshot(space, builder, scenePath, bucket);
            present_frame(space, windowPath, "main", kCanvasWidth, kCanvasHeight);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    return 0;
#endif
}
