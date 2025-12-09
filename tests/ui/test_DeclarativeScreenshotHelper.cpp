#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/declarative/InputTask.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>

#include "../../examples/declarative_example_shared.hpp"

#include <third_party/stb_image.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct ScopedFile {
    std::filesystem::path path;

    ~ScopedFile() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

auto unique_png_path(std::string_view prefix) -> std::filesystem::path {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string filename(prefix);
    filename.push_back('_');
    filename.append(std::to_string(stamp));
    filename.append(".png");
    return temp_dir / filename;
}

auto load_png_rgba(std::filesystem::path const& path,
                   int& width,
                   int& height) -> std::vector<std::uint8_t> {
    std::ifstream file(path, std::ios::binary);
    INFO("png path: " << path);
    REQUIRE(file.good());
    std::vector<std::uint8_t> buffer((std::istreambuf_iterator<char>(file)), {});
    REQUIRE_FALSE(buffer.empty());
    int components = 0;
    auto* data = stbi_load_from_memory(buffer.data(),
                                       static_cast<int>(buffer.size()),
                                       &width,
                                       &height,
                                       &components,
                                       4);
    INFO(stbi_failure_reason());
    REQUIRE(data != nullptr);
    auto total = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    std::vector<std::uint8_t> pixels(data, data + total);
    stbi_image_free(data);
    return pixels;
}

auto count_unique_colors(std::span<const std::uint8_t> pixels) -> std::size_t {
    std::unordered_set<std::uint32_t> colors;
    colors.reserve(pixels.size() / 16);
    for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
        std::uint32_t color = static_cast<std::uint32_t>(pixels[offset])
            | (static_cast<std::uint32_t>(pixels[offset + 1]) << 8u)
            | (static_cast<std::uint32_t>(pixels[offset + 2]) << 16u)
            | (static_cast<std::uint32_t>(pixels[offset + 3]) << 24u);
        colors.insert(color);
    }
    return colors.size();
}

struct DeclarativeScreenshotHarness {
    SP::PathSpace space;
    SP::App::AppRootPath app_root;
    SP::UI::WindowPath window_path;
    SP::UI::ScenePath scene_path;
    SP::UI::SurfacePath surface_path;
    std::string view_name;

    DeclarativeScreenshotHarness() {
        SP::System::LaunchOptions launch_options{};
        launch_options.start_io_telemetry_control = false;
        auto launch = SP::System::LaunchStandard(space, launch_options);
        REQUIRE(launch);

        auto app = SP::App::Create(space, "screenshot_helper_test");
        REQUIRE(app);
        app_root = *app;

        SP::Window::CreateOptions window_options{};
        window_options.title = "Screenshot Helper";
        window_options.name = "screenshot_helper_window";
        window_options.width = 320;
        window_options.height = 200;
        window_options.visible = false;
        auto window = SP::Window::Create(space, app_root, window_options);
        REQUIRE(window);
        window_path = window->path;
        view_name = window->view_name;

        auto disable_metal = PathSpaceExamples::force_window_software_renderer(space,
                                                                              window_path,
                                                                              view_name);
        REQUIRE(disable_metal);

        SP::Scene::CreateOptions scene_options{};
        scene_options.name = "screenshot_helper_scene";
        scene_options.view = view_name;
        auto scene = SP::Scene::Create(space, app_root, window_path, scene_options);
        REQUIRE(scene);
        scene_path = scene->path;

        auto window_view_path = PathSpaceExamples::make_window_view_path(window_path, view_name);
        auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
        REQUIRE(surface_rel);
        auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root.getPath()},
                                                         *surface_rel);
        REQUIRE(surface_abs);
        surface_path = SP::UI::SurfacePath{surface_abs->getPath()};
        auto set_scene = SP::UI::Runtime::Surface::SetScene(space, surface_path, scene_path);
        REQUIRE(set_scene);

        auto window_view = SP::App::ConcretePathView{window_view_path};
        mount_ui(window_view);

        auto pump_widgets = SP::UI::Declarative::PumpWindowWidgetsOnce(space, window_path, view_name);
        REQUIRE(pump_widgets);
        auto pump_scene = SP::UI::Declarative::SceneLifecycle::PumpSceneOnce(space, scene_path);
        REQUIRE(pump_scene);
        auto render_future = SP::UI::Runtime::Surface::RenderOnce(space, surface_path, std::nullopt);
        REQUIRE(render_future);
        CHECK(render_future->ready());
    }

    ~DeclarativeScreenshotHarness() { SP::System::ShutdownDeclarativeRuntime(space); }

    void mount_ui(SP::App::ConcretePathView parent) {
        SP::UI::Declarative::Button::Args button_args{};
        button_args.label = "Capture";
        button_args.style.width = 260.0f;
        button_args.style.height = 72.0f;
        button_args.style.corner_radius = 20.0f;
        button_args.style_override().background_color({0.12f, 0.35f, 0.85f, 1.0f});
        button_args.style_override().text_color({0.98f, 0.98f, 0.98f, 1.0f});
        auto button = SP::UI::Declarative::Button::Create(space,
                                                          parent,
                                                          "capture_button",
                                                          button_args);
        REQUIRE(button);

        SP::UI::Declarative::Label::Args label_args{};
        label_args.text = "Declarative Screenshot Helper";
        label_args.color = {0.95f, 0.80f, 0.20f, 1.0f};
        label_args.typography.font_size = 28.0f;
        label_args.typography.line_height = 32.0f;
        auto label = SP::UI::Declarative::Label::Create(space,
                                                        parent,
                                                        "status_label",
                                                        label_args);
        REQUIRE(label);
    }

    auto make_default_options(std::filesystem::path const& output) const
        -> SP::UI::Screenshot::DeclarativeScreenshotOptions {
        SP::UI::Screenshot::DeclarativeScreenshotOptions options{};
        options.output_png = output;
        options.view_name = view_name;
        options.width = 320;
        options.height = 200;
        options.force_software = true;
        options.allow_software_fallback = true;
        options.present_when_force_software = false;
        options.force_publish = false;
        options.mark_dirty_before_publish = false;
        options.require_present = false;
        options.present_before_capture = false;
        options.enable_capture_framebuffer = true;
        options.readiness_timeout = 400ms;
        options.publish_timeout = 400ms;
        options.present_timeout = 400ms;
        options.wait_for_runtime_metrics = false;
        options.readiness_options.wait_for_runtime_metrics = false;
        options.readiness_options.runtime_metrics_timeout = 400ms;
        options.readiness_options.wait_for_structure = false;
        options.readiness_options.wait_for_buckets = false;
        options.readiness_options.wait_for_revision = false;
        return options;
    }
};

} // namespace

TEST_SUITE("DeclarativeScreenshotHelper") {

TEST_CASE("CaptureDeclarative writes a live framebuffer PNG") {
    DeclarativeScreenshotHarness harness;
    ScopedFile png_file{unique_png_path("screenshot_helper_live")};
    auto options = harness.make_default_options(png_file.path);

    auto capture = SP::UI::Screenshot::CaptureDeclarative(harness.space,
                                                          harness.scene_path,
                                                          harness.window_path,
                                                          options);
    if (!capture) {
        CAPTURE(SP::describeError(capture.error()));
    }
    REQUIRE(capture);

    int width = 0;
    int height = 0;
    auto pixels = load_png_rgba(png_file.path, width, height);
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    auto unique_colors = count_unique_colors(pixels);
    CHECK(unique_colors >= 4);
}

TEST_CASE("CaptureDeclarative reports readiness errors") {
    DeclarativeScreenshotHarness harness;
    ScopedFile png_file{unique_png_path("screenshot_helper_readiness")};
    auto options = harness.make_default_options(png_file.path);
    options.wait_for_runtime_metrics = true;
    options.readiness_options.wait_for_runtime_metrics = true;
    options.readiness_options.runtime_metrics_timeout = 50ms;
    options.readiness_timeout = 50ms;

    auto capture = SP::UI::Screenshot::CaptureDeclarative(harness.space,
                                                          harness.scene_path,
                                                          harness.window_path,
                                                          options);
    REQUIRE_FALSE(capture);
    auto code = capture.error().code;
    CAPTURE(static_cast<int>(code));
    bool matched_code = (code == SP::Error::Code::Timeout) || (code == SP::Error::Code::NotFound);
    CHECK(matched_code);
}

TEST_CASE("CaptureDeclarative surfaces force publish failures") {
    DeclarativeScreenshotHarness harness;
    ScopedFile png_file{unique_png_path("screenshot_helper_force_publish")};
    auto options = harness.make_default_options(png_file.path);
    options.readiness_options.wait_for_structure = false;
    options.readiness_options.wait_for_buckets = false;
    options.readiness_options.wait_for_revision = false;
    options.readiness_options.wait_for_runtime_metrics = false;

    auto stop = SP::UI::Declarative::SceneLifecycle::Stop(harness.space, harness.scene_path);
    REQUIRE(stop);

    auto capture = SP::UI::Screenshot::CaptureDeclarative(harness.space,
                                                          harness.scene_path,
                                                          harness.window_path,
                                                          options);
    REQUIRE_FALSE(capture);
    auto code = capture.error().code;
    CAPTURE(static_cast<int>(code));
    bool matched_code = (code == SP::Error::Code::Timeout) || (code == SP::Error::Code::NotFound);
    CHECK(matched_code);
}

} // TEST_SUITE
