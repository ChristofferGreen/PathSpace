#if defined(__APPLE__) && PATHSPACE_UI_METAL

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace SP::UI;
namespace UIScene = SP::UI::Scene;
using SP::UI::Builders::Diagnostics::PathSpaceError;

namespace {

auto make_surface_desc() -> Builders::SurfaceDesc {
    Builders::SurfaceDesc desc{};
    desc.size_px.width = 4;
    desc.size_px.height = 4;
    desc.pixel_format = Builders::PixelFormat::BGRA8Unorm;
    desc.color_space = Builders::ColorSpace::sRGB;
    desc.premultiplied_alpha = true;
    return desc;
}

} // namespace

namespace {

constexpr std::array<std::uint8_t, 78> kTestPngRgba = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,
    0,0,0,21,73,68,65,84,120,156,99,248,207,192,240,31,8,27,24,128,52,8,56,0,0,68,19,8,185,
    109,230,62,33,0,0,0,0,73,69,78,68,174,66,96,130
};

auto format_revision(std::uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto fingerprint_hex(std::uint64_t fingerprint) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << std::nouppercase << fingerprint;
    return oss.str();
}

auto encode_image_command(UIScene::ImageCommand const& image,
                          UIScene::DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::ImageCommand));
    std::memcpy(bucket.command_payload.data() + offset, &image, sizeof(UIScene::ImageCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Image));
}

struct ScopedEnv {
    std::string name;
    std::optional<std::string> previous;

    ScopedEnv(char const* key, char const* value) : name(key) {
        if (auto* existing = std::getenv(key)) {
            previous = std::string(existing);
        }
        if (value) {
            ::setenv(name.c_str(), value, 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }

    ~ScopedEnv() {
        if (previous.has_value()) {
            ::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

struct MetalBuildersFixture {
    SP::PathSpace            space;
    SP::App::AppRootPath     app_root{"/system/applications/test_app"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }
};

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (std::size_t i = 0; i < t.elements.size(); ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

auto encode_rect_command(UIScene::RectCommand const& rect,
                         UIScene::DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(UIScene::RectCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));
}

auto make_rect_bucket() -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xABCDu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {UIScene::BoundingSphere{{0.0f, 0.0f, 0.0f}, 1.0f}};
    bucket.bounds_boxes = {UIScene::BoundingBox{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.layer_indices = {};
    bucket.clip_nodes = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {UIScene::DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0}};
    bucket.drawable_fingerprints = {0x1234u};

    UIScene::RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 1.0f,
        .max_y = 1.0f,
        .color = {0.4f, 0.4f, 0.4f, 1.0f},
    };
    encode_rect_command(rect, bucket);
    return bucket;
}

auto make_image_bucket(std::uint64_t fingerprint) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xF00Du};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {UIScene::BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {UIScene::BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {7};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.layer_indices = {};
    bucket.clip_nodes = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {UIScene::DrawableAuthoringMapEntry{bucket.drawable_ids[0], "image_node", 0, 0}};
    bucket.drawable_fingerprints = {fingerprint};

    UIScene::ImageCommand image{};
    image.min_x = 0.0f;
    image.min_y = 0.0f;
    image.max_x = 2.0f;
    image.max_y = 2.0f;
    image.uv_min_x = 0.0f;
    image.uv_min_y = 0.0f;
    image.uv_max_x = 1.0f;
    image.uv_max_y = 1.0f;
    image.image_fingerprint = fingerprint;
    image.tint = {1.0f, 1.0f, 1.0f, 1.0f};

    encode_image_command(image, bucket);
    return bucket;
}

auto publish_minimal_scene(MetalBuildersFixture& fx,
                           Builders::ScenePath const& scenePath) -> void {
    auto bucket = make_rect_bucket();
    UIScene::SceneSnapshotBuilder builder{fx.space, fx.root_view(), scenePath};
    UIScene::SnapshotPublishOptions opts{};
    opts.metadata.author = "tests";
    opts.metadata.tool_version = "tests";
    opts.metadata.created_at = std::chrono::system_clock::time_point{};
    opts.metadata.drawable_count = bucket.drawable_ids.size();
    opts.metadata.command_count = bucket.command_kinds.size();
    auto revision = builder.publish(opts, bucket);
    REQUIRE(revision);
    auto ready = Builders::Scene::WaitUntilReady(fx.space, scenePath, std::chrono::milliseconds{10});
    REQUIRE(ready);
}

auto create_scene(MetalBuildersFixture& fx,
                  std::string const& name) -> Builders::ScenePath {
    Builders::SceneParams params{
        .name = name,
        .description = "Metal test scene",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), params);
    REQUIRE(scene);
    publish_minimal_scene(fx, *scene);
    return *scene;
}

auto create_renderer(MetalBuildersFixture& fx,
                     std::string const& name,
                     Builders::RendererKind kind) -> Builders::RendererPath {
    Builders::RendererParams params{
        .name = name,
        .kind = kind,
        .description = "Renderer",
    };
    auto renderer = Builders::Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(renderer);
    return *renderer;
}

auto create_surface(MetalBuildersFixture& fx,
                    std::string const& name,
                    Builders::SurfaceDesc desc,
                    std::string const& rendererName) -> Builders::SurfacePath {
    Builders::SurfaceParams params{};
    params.name = name;
    params.desc = desc;
    params.renderer = rendererName;
    auto surface = Builders::Surface::Create(fx.space, fx.root_view(), params);
    REQUIRE(surface);
    return *surface;
}

auto resolve_target(MetalBuildersFixture& fx,
                    Builders::SurfacePath const& surfacePath) -> SP::ConcretePathString {
    auto targetRel = fx.space.read<std::string, std::string>(std::string(surfacePath.getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);
    return SP::ConcretePathString{targetAbs->getPath()};
}

} // namespace

TEST_CASE("PathWindowView presents Metal texture when uploads enabled") {
    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        INFO("PATHSPACE_ENABLE_METAL_UPLOADS is not set; skipping Metal presenter verification");
        return;
    }

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            INFO("No Metal device available; skipping Metal presenter verification");
            return;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        REQUIRE(static_cast<bool>(queue));

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.contentsScale = 1.0;
        layer.drawableSize = CGSizeMake(4.0, 4.0);

        PathWindowView::MetalPresenterConfig config{
            .layer = (__bridge void*)layer,
            .device = (__bridge void*)device,
            .command_queue = (__bridge void*)queue,
            .contents_scale = 1.0,
        };
        PathWindowView::ConfigureMetalPresenter(config);

        auto desc = make_surface_desc();
        desc.metal.storage_mode = Builders::MetalStorageMode::Shared;
        desc.metal.texture_usage = static_cast<std::uint8_t>(Builders::MetalTextureUsage::ShaderRead)
                                   | static_cast<std::uint8_t>(Builders::MetalTextureUsage::RenderTarget);
        PathSurfaceSoftware software{desc};
        PathSurfaceMetal metal{desc};

        std::vector<MaterialDescriptor> materials;
        MaterialDescriptor descriptor{};
        descriptor.material_id = 5;
        descriptor.pipeline_flags = 0x10;
        descriptor.primary_draw_kind = static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect);
        descriptor.drawable_count = 2;
        descriptor.command_count = 3;
        descriptor.tint_rgba = {1.0f, 1.0f, 1.0f, 1.0f};
        descriptor.color_rgba = {0.2f, 0.4f, 0.6f, 0.8f};
        descriptor.uses_image = false;
        materials.push_back(descriptor);
        metal.update_material_descriptors(materials);
        auto shader_keys = metal.shader_keys();
        CHECK(shader_keys.size() == materials.size());
        CHECK(shader_keys.front().pipeline_flags == materials.front().pipeline_flags);
        CHECK(shader_keys.front().uses_image == materials.front().uses_image);
        CHECK(metal.resource_residency().empty());

        std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0x7Fu);
        metal.update_from_rgba8(pixels, 4u * 4u, 5, 9);
        auto texture = metal.acquire_texture();

        PathWindowView::PresentRequest request{
            .now = std::chrono::steady_clock::now(),
            .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5},
            .framebuffer = {},
            .dirty_tiles = {},
            .surface_width_px = desc.size_px.width,
            .surface_height_px = desc.size_px.height,
            .has_metal_texture = texture.texture != nullptr,
            .metal_texture = texture,
            .allow_iosurface_sharing = false,
        };

        PathWindowView view;
        auto stats = view.present(software, {}, request);

        CHECK(stats.presented);
        CHECK(stats.used_metal_texture);
        CHECK(stats.gpu_encode_ms >= 0.0);
        CHECK(stats.gpu_present_ms >= 0.0);
        CHECK(stats.present_ms >= 0.0);
        CHECK_FALSE(stats.skipped);

        id<MTLTexture> tex = (__bridge id<MTLTexture>)texture.texture;
        bool hasTexture = (tex != nil);
        CHECK(hasTexture);
        CHECK(tex.storageMode == MTLStorageModeShared);
        CHECK((tex.usage & MTLTextureUsageShaderRead) != 0);
        CHECK((tex.usage & MTLTextureUsageRenderTarget) != 0);

        auto material_span = metal.material_descriptors();
        CHECK(material_span.size() == materials.size());
        CHECK(material_span.front().material_id == materials.front().material_id);

        PathWindowView::ResetMetalPresenter();
    }
}

TEST_CASE("PathWindowView surfaces Metal presenter errors and falls back") {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            INFO("No Metal device available; skipping Metal presenter error verification");
            return;
        }

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.contentsScale = 1.0;
        layer.drawableSize = CGSizeMake(4.0, 4.0);

        PathWindowView::MetalPresenterConfig config{
            .layer = (__bridge void*)layer,
            .device = (__bridge void*)device,
            .command_queue = nullptr, // intentionally missing to trigger presenter error
            .contents_scale = 1.0,
        };
        PathWindowView::ConfigureMetalPresenter(config);

        auto desc = make_surface_desc();
        PathSurfaceSoftware software{desc};

        // Seed the buffered frame so the CPU fallback can succeed.
        auto staging = software.staging_span();
        std::fill(staging.begin(), staging.end(), 0x3Fu);
        software.publish_buffered_frame({
            .frame_index = 12,
            .revision = 34,
            .render_ms = 1.0,
        });

        PathSurfaceMetal metal{desc};
        auto texture = metal.acquire_texture();

        std::vector<std::uint8_t> framebuffer(software.frame_bytes(), 0);
        PathWindowView::PresentRequest request{
            .now = std::chrono::steady_clock::now(),
            .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5},
            .framebuffer = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()},
            .dirty_tiles = {},
            .surface_width_px = desc.size_px.width,
            .surface_height_px = desc.size_px.height,
            .has_metal_texture = texture.texture != nullptr,
            .metal_texture = texture,
            .allow_iosurface_sharing = false,
        };

        PathWindowView view;
        auto stats = view.present(software, {}, request);

        CHECK(stats.presented);
        CHECK(stats.buffered_frame_consumed);
        CHECK_FALSE(stats.used_metal_texture);
        CHECK(stats.error == "Metal command queue unavailable");
        CHECK(stats.gpu_encode_ms == doctest::Approx(0.0));
        CHECK(stats.gpu_present_ms == doctest::Approx(0.0));

        // Surface the error via diagnostics so GPU failures are visible to callers.
        SP::PathSpace space;
        SP::ConcretePathString targetPath{"/system/applications/test/renderers/2d/targets/surfaces/canvas"};
        auto targetView = SP::ConcretePathStringView{targetPath.getPath().c_str()};

        stats.backend_kind = "Metal2D";
        stats.present_ms = 1.0;
        stats.frame.render_ms = 0.5;

        PathWindowPresentPolicy policy{};
        policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
        policy.frame_timeout = std::chrono::milliseconds{5};
        policy.frame_timeout_ms_value = 5.0;
        policy.staleness_budget = std::chrono::milliseconds{2};
        policy.staleness_budget_ms_value = 2.0;

        auto writeMetrics = Builders::Diagnostics::WritePresentMetrics(space,
                                                                       targetView,
                                                                       stats,
                                                                       policy);
        REQUIRE(writeMetrics);

        auto metrics = Builders::Diagnostics::ReadTargetMetrics(space, targetView);
        REQUIRE(metrics);
        CHECK(metrics->backend_kind == "Metal2D");
        CHECK_FALSE(metrics->used_metal_texture);
        CHECK(metrics->last_error == "Metal command queue unavailable");
        CHECK(metrics->last_error_code == 3000);
        CHECK(metrics->last_error_severity == PathSpaceError::Severity::Recoverable);
        CHECK(metrics->last_error_timestamp_ns > 0);

        PathWindowView::ResetMetalPresenter();
    }
}

TEST_CASE("Metal pipeline publishes residency metrics and material descriptors") {
    ScopedEnv enableMetal{"PATHSPACE_ENABLE_METAL_UPLOADS", "1"};
    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        INFO("Failed to enable PATHSPACE_ENABLE_METAL_UPLOADS; skipping Metal residency verification");
        return;
    }

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            INFO("No Metal device available; skipping Metal residency verification");
            return;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            INFO("Failed to create Metal command queue; skipping Metal residency verification");
            return;
        }

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.contentsScale = 1.0;
        layer.drawableSize = CGSizeMake(4.0, 4.0);

        PathWindowView::MetalPresenterConfig config{
            .layer = (__bridge void*)layer,
            .device = (__bridge void*)device,
            .command_queue = (__bridge void*)queue,
            .contents_scale = 1.0,
        };
        PathWindowView::ConfigureMetalPresenter(config);

        MetalBuildersFixture fx;

        auto scene = create_scene(fx, "scene_metal_metrics");
        auto renderer = create_renderer(fx, "renderer_metal_metrics", Builders::RendererKind::Metal2D);

        auto desc = make_surface_desc();
        desc.size_px.width = 8;
        desc.size_px.height = 8;
        desc.metal.iosurface_backing = true;
        desc.metal.storage_mode = Builders::MetalStorageMode::Shared;
        desc.metal.texture_usage = static_cast<std::uint8_t>(Builders::MetalTextureUsage::ShaderRead)
                                   | static_cast<std::uint8_t>(Builders::MetalTextureUsage::RenderTarget);

        auto surface = create_surface(fx, "surface_metal_metrics", desc, renderer.getPath());
        REQUIRE(Builders::Surface::SetScene(fx.space, surface, scene));

        Builders::WindowParams windowParams{
            .name = "MainWindow",
            .title = "Metal Window",
            .width = 8,
            .height = 8,
            .scale = 1.0f,
            .background = "#000000",
        };
        auto window = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
        REQUIRE(window);
        REQUIRE(Builders::Window::AttachSurface(fx.space, *window, "main", surface));

        auto renderFuture = Builders::Surface::RenderOnce(fx.space, surface, std::nullopt);
        if (!renderFuture) {
            auto err = renderFuture.error();
            INFO("Surface::RenderOnce error code = " << static_cast<int>(err.code));
            INFO("Surface::RenderOnce error message = " << err.message.value_or("<none>"));
        }
        REQUIRE(renderFuture);
        CHECK(renderFuture->ready());

        auto present = Builders::Window::Present(fx.space, *window, "main");
        if (!present) {
            INFO("Window::Present error code = " << static_cast<int>(present.error().code));
            INFO("Window::Present error message = " << present.error().message.value_or("<none>"));
        }
        REQUIRE(present);
        CHECK(present->stats.presented);
        CHECK(present->stats.used_metal_texture);
        CHECK(present->stats.backend_kind == "Metal2D");

        auto targetPath = resolve_target(fx, surface);
        auto metrics = Builders::Diagnostics::ReadTargetMetrics(fx.space,
                                                                Builders::ConcretePathView{targetPath.getPath()});
        REQUIRE(metrics);
        CHECK(metrics->backend_kind == "Metal2D");
        CHECK(metrics->used_metal_texture);
        CHECK(metrics->frame_index >= 1);
        CHECK(metrics->render_ms >= 0.0);
        CHECK(metrics->gpu_bytes > 0);
        CHECK(metrics->cpu_bytes > 0);
        auto textureBytesPath = std::string(targetPath.getPath()) + "/output/v1/common/textureGpuBytes";
        auto textureBytes = fx.space.read<std::uint64_t>(textureBytesPath);
        REQUIRE(textureBytes);
        CHECK(*textureBytes > 0);

        REQUIRE(metrics->material_count == 1);
        REQUIRE(metrics->materials.size() == 1);
        auto const& material = metrics->materials.front();
        CHECK(material.material_id == 1);
        CHECK(material.primary_draw_kind == static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));
        CHECK(material.drawable_count == 1);
        CHECK(material.command_count >= 1);
        CHECK(material.uses_image == false);
        CHECK(material.color_rgba[0] == doctest::Approx(0.4f));
        CHECK(material.color_rgba[1] == doctest::Approx(0.4f));
        CHECK(material.color_rgba[2] == doctest::Approx(0.4f));
        CHECK(material.color_rgba[3] == doctest::Approx(1.0f));

        auto gpuBytesPath = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency/gpuBytes";
        auto storedGpuBytes = fx.space.read<std::uint64_t>(gpuBytesPath);
        REQUIRE(storedGpuBytes);
        CHECK(*storedGpuBytes == metrics->gpu_bytes);

        auto settings = Builders::Renderer::ReadSettings(fx.space,
                                                         Builders::ConcretePathView{targetPath.getPath()});
        REQUIRE(settings);
        CHECK(settings->renderer.backend_kind == Builders::RendererKind::Metal2D);
        CHECK(settings->renderer.metal_uploads_enabled);

        PathWindowView::ResetMetalPresenter();
    }
}

TEST_CASE("Metal pipeline publishes image residency watermarks") {
    ScopedEnv enableMetal{"PATHSPACE_ENABLE_METAL_UPLOADS", "1"};
    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        INFO("Failed to enable PATHSPACE_ENABLE_METAL_UPLOADS; skipping Metal image residency verification");
        return;
    }

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            INFO("No Metal device available; skipping Metal image residency verification");
            return;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            INFO("Failed to create Metal command queue; skipping Metal image residency verification");
            return;
        }

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.contentsScale = 1.0;
        layer.drawableSize = CGSizeMake(8.0, 8.0);

        PathWindowView::MetalPresenterConfig config{
            .layer = (__bridge void*)layer,
            .device = (__bridge void*)device,
            .command_queue = (__bridge void*)queue,
            .contents_scale = 1.0,
        };
        PathWindowView::ConfigureMetalPresenter(config);

        MetalBuildersFixture fx;

        auto scene = create_scene(fx, "scene_metal_image_metrics");
        auto renderer = create_renderer(fx, "renderer_metal_image_metrics", Builders::RendererKind::Metal2D);

        constexpr std::uint64_t kImageFingerprint = 0x0CDEF1234567890ull;
        auto imageBucket = make_image_bucket(kImageFingerprint);
        UIScene::SceneSnapshotBuilder builder{fx.space, fx.root_view(), scene};
        UIScene::SnapshotPublishOptions opts{};
        opts.metadata.author = "tests";
        opts.metadata.tool_version = "tests";
        opts.metadata.created_at = std::chrono::system_clock::time_point{};
        opts.metadata.drawable_count = imageBucket.drawable_ids.size();
        opts.metadata.command_count = imageBucket.command_kinds.size();
        auto revision = builder.publish(opts, imageBucket);
        REQUIRE(revision);
        auto revision_base = std::string(scene.getPath()) + "/builds/" + format_revision(*revision);
        auto image_path = revision_base + "/assets/images/" + fingerprint_hex(kImageFingerprint) + ".png";
        auto truncatedFingerprint = kImageFingerprint & 0x0FFFFFFFFFFFFFFFULL;
        auto truncated_path = revision_base + "/assets/images/" + fingerprint_hex(truncatedFingerprint) + ".png";
        std::vector<std::uint8_t> png_bytes(kTestPngRgba.begin(), kTestPngRgba.end());
        auto inserted = fx.space.insert(image_path, png_bytes);
        REQUIRE(inserted.errors.empty());
        if (truncated_path != image_path) {
            auto inserted_trunc = fx.space.insert(truncated_path, png_bytes);
            REQUIRE(inserted_trunc.errors.empty());
        }
        auto ready = Builders::Scene::WaitUntilReady(fx.space, scene, std::chrono::milliseconds{10});
        REQUIRE(ready);

        auto desc = make_surface_desc();
        desc.size_px.width = 16;
        desc.size_px.height = 16;
        desc.metal.iosurface_backing = true;
        desc.metal.storage_mode = Builders::MetalStorageMode::Shared;
        desc.metal.texture_usage = static_cast<std::uint8_t>(Builders::MetalTextureUsage::ShaderRead)
                                   | static_cast<std::uint8_t>(Builders::MetalTextureUsage::RenderTarget);

        auto surface = create_surface(fx, "surface_metal_image_metrics", desc, renderer.getPath());
        REQUIRE(Builders::Surface::SetScene(fx.space, surface, scene));

        Builders::WindowParams windowParams{
            .name = "ImageWindow",
            .title = "Metal Image Window",
            .width = 16,
            .height = 16,
            .scale = 1.0f,
            .background = "#000000",
        };
        auto window = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
        REQUIRE(window);
        REQUIRE(Builders::Window::AttachSurface(fx.space, *window, "main", surface));

        constexpr std::uint64_t kCpuSoftBytes = 4096;
        constexpr std::uint64_t kCpuHardBytes = 8192;
        constexpr std::uint64_t kGpuSoftBytes = 2048;
        constexpr std::uint64_t kGpuHardBytes = 4096;

        Builders::RenderSettings overrides{};
        overrides.surface.size_px.width = desc.size_px.width;
        overrides.surface.size_px.height = desc.size_px.height;
        overrides.surface.dpi_scale = 1.0f;
        overrides.surface.visibility = true;
        overrides.surface.metal = desc.metal;
        overrides.clear_color = {0.05f, 0.05f, 0.05f, 1.0f};
        overrides.time.delta_ms = 16.0;
        overrides.time.frame_index = 0;
        overrides.time.time_ms = 0.0;
        overrides.renderer.backend_kind = Builders::RendererKind::Metal2D;
        overrides.renderer.metal_uploads_enabled = true;
        overrides.cache.cpu_soft_bytes = kCpuSoftBytes;
        overrides.cache.cpu_hard_bytes = kCpuHardBytes;
        overrides.cache.gpu_soft_bytes = kGpuSoftBytes;
        overrides.cache.gpu_hard_bytes = kGpuHardBytes;

        auto renderFuture = Builders::Surface::RenderOnce(fx.space, surface, overrides);
        if (!renderFuture) {
            auto err = renderFuture.error();
            INFO("Surface::RenderOnce error code = " << static_cast<int>(err.code));
            INFO("Surface::RenderOnce error message = " << err.message.value_or("<none>"));
        }
        REQUIRE(renderFuture);
        CHECK(renderFuture->ready());

        auto present = Builders::Window::Present(fx.space, *window, "main");
        if (!present) {
            INFO("Window::Present error code = " << static_cast<int>(present.error().code));
            INFO("Window::Present error message = " << present.error().message.value_or("<none>"));
        }
        REQUIRE(present);
        CHECK(present->stats.presented);
        CHECK(present->stats.used_metal_texture);
        CHECK(present->stats.backend_kind == "Metal2D");

        auto targetPath = resolve_target(fx, surface);
        auto metrics = Builders::Diagnostics::ReadTargetMetrics(fx.space,
                                                                Builders::ConcretePathView{targetPath.getPath()});
        REQUIRE(metrics);
        CHECK(metrics->backend_kind == "Metal2D");
        CHECK(metrics->used_metal_texture);
        CHECK(metrics->gpu_bytes > 0);
        CHECK(metrics->cpu_bytes > 0);
        auto textureBytesPath = std::string(targetPath.getPath()) + "/output/v1/common/textureGpuBytes";
        auto textureBytes = fx.space.read<std::uint64_t>(textureBytesPath);
        REQUIRE(textureBytes);
        CHECK(*textureBytes > 0);

        REQUIRE(metrics->material_count == 1);
        REQUIRE(metrics->materials.size() == 1);
        auto const& material = metrics->materials.front();
        CHECK(material.uses_image);
        auto const actualFingerprint = material.resource_fingerprint;
        CHECK(actualFingerprint != 0);
        CAPTURE(actualFingerprint);
        CHECK(actualFingerprint == kImageFingerprint);
        CHECK(material.command_count >= 1);

        REQUIRE(metrics->material_resource_count >= 1);
        REQUIRE_FALSE(metrics->material_resources.empty());
        auto const& resource = metrics->material_resources.front();
        CHECK(resource.fingerprint == kImageFingerprint);
        CHECK(resource.uses_image);
        CHECK(resource.gpu_bytes > 0);
        CHECK(resource.cpu_bytes > 0);

        CHECK(metrics->cpu_soft_bytes == kCpuSoftBytes);
        CHECK(metrics->cpu_hard_bytes == kCpuHardBytes);
        CHECK(metrics->gpu_soft_bytes == kGpuSoftBytes);
        CHECK(metrics->gpu_hard_bytes == kGpuHardBytes);

        auto residencyBase = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";
        auto cpuSoft = fx.space.read<std::uint64_t>(residencyBase + "/cpuSoftBytes");
        REQUIRE(cpuSoft);
        CHECK(*cpuSoft == kCpuSoftBytes);
        auto cpuHard = fx.space.read<std::uint64_t>(residencyBase + "/cpuHardBytes");
        REQUIRE(cpuHard);
        CHECK(*cpuHard == kCpuHardBytes);
        auto gpuSoft = fx.space.read<std::uint64_t>(residencyBase + "/gpuSoftBytes");
        REQUIRE(gpuSoft);
        CHECK(*gpuSoft == kGpuSoftBytes);
        auto gpuHard = fx.space.read<std::uint64_t>(residencyBase + "/gpuHardBytes");
        REQUIRE(gpuHard);
        CHECK(*gpuHard == kGpuHardBytes);

        auto settings = Builders::Renderer::ReadSettings(fx.space,
                                                         Builders::ConcretePathView{targetPath.getPath()});
        REQUIRE(settings);
        CHECK(settings->renderer.backend_kind == Builders::RendererKind::Metal2D);
        CHECK(settings->renderer.metal_uploads_enabled);
        CHECK(settings->cache.cpu_soft_bytes == kCpuSoftBytes);
        CHECK(settings->cache.cpu_hard_bytes == kCpuHardBytes);
        CHECK(settings->cache.gpu_soft_bytes == kGpuSoftBytes);
        CHECK(settings->cache.gpu_hard_bytes == kGpuHardBytes);

        PathWindowView::ResetMetalPresenter();
    }
}

#endif // defined(__APPLE__) && PATHSPACE_UI_METAL
