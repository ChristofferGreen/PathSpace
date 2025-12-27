#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>
#include <pathspace/ui/screenshot/ScreenshotSlot.hpp>

#include "../../examples/declarative_example_shared.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 360;

auto hash_file(std::filesystem::path const& path) -> std::uint64_t {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    std::uint64_t hash = 14695981039346656037ull; // FNV-1a 64-bit offset basis
    constexpr std::uint64_t prime = 1099511628211ull;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        auto count = static_cast<std::size_t>(file.gcount());
        for (std::size_t i = 0; i < count; ++i) {
            hash ^= static_cast<std::uint8_t>(buffer[i]);
            hash *= prime;
        }
    }
    return hash;
}

auto capture_button_screenshot(SP::PathSpace& space,
                               SP::Scene::CreateResult const& scene,
                               SP::Window::CreateResult const& window,
                               std::filesystem::path const& output) -> std::uint64_t {
    if (std::filesystem::exists(output)) {
        std::error_code ec;
        std::filesystem::remove(output, ec);
    }

    SP::UI::Screenshot::DeclarativeScreenshotOptions options{};
    options.width = kWindowWidth;
    options.height = kWindowHeight;
    options.output_png = output;
    options.require_present = true;
    options.present_before_capture = true;
    options.allow_software_fallback = true;
    options.force_software = false;
    options.present_timeout = std::chrono::milliseconds{2000};

    auto start = std::chrono::steady_clock::now();
    auto capture = SP::UI::Screenshot::CaptureDeclarative(space, scene.path, window.path, options);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (!capture) {
        CAPTURE(SP::describeError(capture.error()));
    }
    REQUIRE(capture);
    CHECK_MESSAGE(elapsed < std::chrono::milliseconds{5000},
                  "Declarative screenshot should finish quickly");
    REQUIRE(std::filesystem::exists(output));
    return hash_file(output);
}

auto capture_button_screenshot_simple(SP::PathSpace& space,
                                      SP::Scene::CreateResult const& scene,
                                      SP::Window::CreateResult const& window,
                                      std::filesystem::path const& output) -> std::uint64_t {
    if (std::filesystem::exists(output)) {
        std::error_code ec;
        std::filesystem::remove(output, ec);
    }

    SP::UI::Screenshot::CaptureDeclarativeSimple(space,
                                                 scene.path,
                                                 window.path,
                                                 output,
                                                 kWindowWidth,
                                                 kWindowHeight);

    SP::App::RunOptions run_options{};
    run_options.window_width = kWindowWidth;
    run_options.window_height = kWindowHeight;
    run_options.window_title = "Declarative Button Simple";
    run_options.run_once = true;

    auto run_ui = SP::App::RunUI(space, scene, window, run_options);
    if (!run_ui) {
        CAPTURE(SP::describeError(run_ui.error()));
    }
    REQUIRE(run_ui);

    auto slot_paths = SP::UI::Screenshot::MakeScreenshotSlotPaths(window.path, window.view_name);
    auto status = space.read<std::string, std::string>(slot_paths.status);
    REQUIRE(status);
    CHECK_NE(*status, "pending");
    REQUIRE(std::filesystem::exists(output));
    return hash_file(output);
}

} // namespace

TEST_SUITE("ui") {

TEST_CASE("Declarative button screenshot is stable") {
    SP::PathSpace space;

    SP::System::LaunchOptions launch{};
    launch.start_input_runtime = true;
    launch.start_widget_event_trellis = true;
    launch.start_io_trellis = false;
    launch.start_io_pump = false;
    launch.start_io_telemetry_control = false;
    launch.start_paint_gpu_uploader = false;

    auto launched = SP::System::LaunchStandard(space, launch);
    REQUIRE(launched);

    auto app = SP::App::Create(space,
                               "declarative_button_screenshot_test",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);
    auto disable_metal = PathSpaceExamples::force_window_software_renderer(space,
                                                                           window->path,
                                                                           window->view_name);
    REQUIRE(disable_metal);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene", .view = window->view_name});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    REQUIRE(surface_rel);

    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()},
                                                     *surface_rel);
    REQUIRE(surface_abs);

    auto surface_status = SP::UI::Surface::SetScene(space,
                                                    SP::UI::SurfacePath{surface_abs->getPath()},
                                                    scene->path);
    REQUIRE(surface_status);

    auto stack = SP::UI::Declarative::Stack::Create(
        space,
        window_view,
        "button_column",
        SP::UI::Declarative::Stack::Args{.panels = {
            {.id = "hello_button",
             .fragment = SP::UI::Declarative::Button::Fragment(
                 SP::UI::Declarative::Button::Args{.label = "Say Hello"})},
            {.id = "goodbye_button",
             .fragment = SP::UI::Declarative::Button::Fragment(
                 SP::UI::Declarative::Button::Args{.label = "Say Goodbye"})},
        }});
    REQUIRE(stack);

    // Capture twice and ensure the PNG is stable.
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto first_path = tmp_dir / "pathspace_decl_button_capture_1.png";
    auto second_path = tmp_dir / "pathspace_decl_button_capture_2.png";
    auto simple_path = tmp_dir / "pathspace_decl_button_capture_simple.png";

    auto first_hash = capture_button_screenshot(space, *scene, *window, first_path);
    auto second_hash = capture_button_screenshot(space, *scene, *window, second_path);
    auto simple_hash = capture_button_screenshot_simple(space, *scene, *window, simple_path);

    CHECK_EQ(first_hash, second_hash);
    CHECK_EQ(first_hash, simple_hash);

    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative simple screenshot errors when no present occurs") {
    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    REQUIRE(launch);

    auto app = SP::App::Create(space,
                               "declarative_simple_timeout",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene_simple_timeout", .view = window->view_name});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    REQUIRE(surface_rel);

    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()},
                                                     *surface_rel);
    REQUIRE(surface_abs);

    auto surface_status = SP::UI::Surface::SetScene(space,
                                                    SP::UI::SurfacePath{surface_abs->getPath()},
                                                    scene->path);
    REQUIRE(surface_status);

    auto window_view = SP::App::ConcretePathView{window_view_path};
    auto stack = SP::UI::Declarative::Stack::Create(
        space,
        window_view,
        "button_column_simple_timeout",
        SP::UI::Declarative::Stack::Args{.panels = {
            {.id = "hello_button",
             .fragment = SP::UI::Declarative::Button::Fragment(
                 SP::UI::Declarative::Button::Args{.label = "Say Hello"})},
        }});
    REQUIRE(stack);

    auto output = std::filesystem::temp_directory_path() / "pathspace_decl_simple_timeout.png";
    SP::UI::Screenshot::CaptureDeclarativeSimple(space,
                                                 scene->path,
                                                 window->path,
                                                 output,
                                                 kWindowWidth,
                                                 kWindowHeight);

    auto slot_paths = SP::UI::Screenshot::MakeScreenshotSlotPaths(window->path, window->view_name);
    auto wait = SP::UI::Screenshot::WaitForScreenshotSlotResult(space,
                                                                slot_paths,
                                                                std::chrono::milliseconds{100});
    CHECK_FALSE(wait);
    if (!wait) {
        CHECK(wait.error().code == SP::Error::Code::Timeout);
    }
    auto status = space.read<std::string, std::string>(slot_paths.status);
    REQUIRE(status);
    CHECK_EQ(*status, "timeout");
    auto armed = space.read<bool>(slot_paths.armed);
    if (armed) {
        CHECK_FALSE(*armed);
    }
    auto error_message = space.read<std::string, std::string>(slot_paths.error);
    if (error_message) {
        CHECK_FALSE(error_message->empty());
    }

    std::error_code ec;
    std::filesystem::remove(output, ec);
    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative screenshot token is reusable") {
    SP::PathSpace space;

    auto launched = SP::System::LaunchStandard(space);
    REQUIRE(launched);

    auto app = SP::App::Create(space,
                               "declarative_button_screenshot_token",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);
    auto disable_metal = PathSpaceExamples::force_window_software_renderer(space,
                                                                           window->path,
                                                                           window->view_name);
    REQUIRE(disable_metal);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene_token", .view = window->view_name});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    REQUIRE(surface_rel);

    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()},
                                                     *surface_rel);
    REQUIRE(surface_abs);

    auto surface_status = SP::UI::Surface::SetScene(space,
                                                    SP::UI::SurfacePath{surface_abs->getPath()},
                                                    scene->path);
    REQUIRE(surface_status);

    auto stack = SP::UI::Declarative::Stack::Create(
        space,
        window_view,
        "button_column",
        SP::UI::Declarative::Stack::Args{.panels = {
            {.id = "hello_button",
             .fragment = SP::UI::Declarative::Button::Fragment(
                 SP::UI::Declarative::Button::Args{.label = "Say Hello"})},
        }});
    REQUIRE(stack);

    auto tmp_dir = std::filesystem::temp_directory_path();
    auto first_path = tmp_dir / "pathspace_decl_button_token_1.png";
    auto second_path = tmp_dir / "pathspace_decl_button_token_2.png";

    auto first_hash = capture_button_screenshot(space, *scene, *window, first_path);
    auto second_hash = capture_button_screenshot(space, *scene, *window, second_path);

    CHECK_EQ(first_hash, second_hash);

    auto slot_paths = SP::UI::Screenshot::MakeScreenshotSlotPaths(window->path, window->view_name);
    auto armed = space.read<bool>(slot_paths.armed);
    REQUIRE(armed);
    CHECK_FALSE(*armed);
    auto token_value = space.read<bool>(slot_paths.token);
    REQUIRE(token_value);
    CHECK(*token_value);

    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative screenshot times out when deadline already passed and no presents run") {
    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    REQUIRE(launch);

    auto app = SP::App::Create(space,
                               "declarative_button_screenshot_deadline",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene_deadline", .view = window->view_name});
    REQUIRE(scene);

    SP::UI::Screenshot::DeclarativeScreenshotOptions options{};
    options.width = kWindowWidth;
    options.height = kWindowHeight;
    options.output_png = std::filesystem::temp_directory_path() / "pathspace_decl_deadline.png";
    options.view_name = window->view_name;
    options.capture_mode = "deadline_ns";
    options.capture_deadline = std::chrono::nanoseconds{-1000000};
    options.present_before_capture = false;
    options.slot_timeout = std::chrono::milliseconds{50};
    options.token_timeout = std::chrono::milliseconds{50};
    options.force_software = true;
    options.allow_software_fallback = true;
    options.wait_for_runtime_metrics = false;
    options.readiness_options.wait_for_runtime_metrics = false;
    options.readiness_options.wait_for_structure = false;
    options.readiness_options.wait_for_buckets = false;
    options.readiness_options.wait_for_revision = false;

    auto capture = SP::UI::Screenshot::CaptureDeclarative(space,
                                                          scene->path,
                                                          window->path,
                                                          options);
    REQUIRE_FALSE(capture);
    auto code = capture.error().code;
    CAPTURE(static_cast<int>(code));
    CAPTURE(SP::describeError(capture.error()));
    CHECK((code == SP::Error::Code::Timeout || code == SP::Error::Code::NotFound || code == SP::Error::Code::NoObjectFound));

    auto slot_paths = SP::UI::Screenshot::MakeScreenshotSlotPaths(window->path, window->view_name);
    if (auto armed = space.read<bool>(slot_paths.armed)) {
        CHECK_FALSE(*armed);
    }
    if (auto status = space.read<std::string, std::string>(slot_paths.status)) {
        CHECK_EQ(*status, "timeout");
    }
    if (auto token_value = space.read<bool>(slot_paths.token)) {
        CHECK(*token_value);
    }

    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative screenshot requires output path") {
    SP::PathSpace space;
    auto launched = SP::System::LaunchStandard(space);
    REQUIRE(launched);

    auto app = SP::App::Create(space,
                               "declarative_button_screenshot_test_missing_output",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene", .view = window->view_name});
    REQUIRE(scene);

    SP::UI::Screenshot::DeclarativeScreenshotOptions options{};
    options.width = kWindowWidth;
    options.height = kWindowHeight;
    options.view_name = window->view_name;

    auto capture = SP::UI::Screenshot::CaptureDeclarative(space,
                                                          scene->path,
                                                          window->path,
                                                          options);
    CHECK_FALSE(capture);
    if (!capture) {
        CHECK(capture.error().code == SP::Error::Code::InvalidPath); // output_png missing -> invalid input
    }

    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative screenshot token contends and both captures succeed") {
    SP::PathSpace space;
    auto launched = SP::System::LaunchStandard(space);
    REQUIRE(launched);

    auto app = SP::App::Create(space,
                               "declarative_button_screenshot_contention",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene_contention", .view = window->view_name});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    REQUIRE(surface_rel);

    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()},
                                                     *surface_rel);
    REQUIRE(surface_abs);

    auto surface_status = SP::UI::Surface::SetScene(space,
                                                    SP::UI::SurfacePath{surface_abs->getPath()},
                                                    scene->path);
    REQUIRE(surface_status);

    auto stack = SP::UI::Declarative::Stack::Create(
        space,
        window_view,
        "button_column",
        SP::UI::Declarative::Stack::Args{.panels = {
            {.id = "hello_button",
             .fragment = SP::UI::Declarative::Button::Fragment(
                 SP::UI::Declarative::Button::Args{.label = "Say Hello"})},
        }});
    REQUIRE(stack);

    auto tmp_dir = std::filesystem::temp_directory_path();
    auto first_path = tmp_dir / "pathspace_decl_button_token_contention_1.png";
    auto second_path = tmp_dir / "pathspace_decl_button_token_contention_2.png";

    auto slot_paths = SP::UI::Screenshot::MakeScreenshotSlotPaths(window->path, window->view_name);
    auto guard = SP::UI::Screenshot::AcquireScreenshotToken(space,
                                                            slot_paths.token,
                                                            std::chrono::milliseconds{100});
    REQUIRE(guard);

    auto make_capture = [&](std::filesystem::path output_path) {
        SP::UI::Screenshot::DeclarativeScreenshotOptions options{};
        options.width = kWindowWidth;
        options.height = kWindowHeight;
        options.output_png = output_path;
        options.require_present = true;
        options.present_before_capture = true;
        options.allow_software_fallback = true;
        options.slot_timeout = std::chrono::milliseconds{2000};
        options.token_timeout = std::chrono::milliseconds{1000};
        options.view_name = window->view_name;
        return SP::UI::Screenshot::CaptureDeclarative(space, scene->path, window->path, options);
    };

    std::promise<void> start_barrier;
    auto blocker = start_barrier.get_future().share();

    auto first_future = std::async(std::launch::async, [&, blocker]() {
        blocker.wait();
        return make_capture(first_path);
    });
    auto second_future = std::async(std::launch::async, [&, blocker]() {
        blocker.wait();
        return make_capture(second_path);
    });

    // Release the pre-held token only after both requests are waiting.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    guard->release();

    start_barrier.set_value();

    auto first_result = first_future.get();
    auto second_result = second_future.get();

    REQUIRE(first_result);
    REQUIRE(second_result);
    auto first_artifact = first_result->artifact.empty() ? first_path : first_result->artifact;
    auto second_artifact = second_result->artifact.empty() ? second_path : second_result->artifact;
    CHECK(std::filesystem::exists(first_artifact));
    CHECK(std::filesystem::exists(second_artifact));

    auto token_value = space.read<bool>(slot_paths.token);
    REQUIRE(token_value);
    CHECK(*token_value);
    auto armed = space.read<bool>(slot_paths.armed);
    if (armed) {
    CHECK_FALSE(*armed);
    }

    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative screenshot errors when no framebuffer is available") {
    SP::PathSpace space;
    auto launched = SP::System::LaunchStandard(space);
    REQUIRE(launched);

    auto app = SP::App::Create(space,
                               "declarative_screenshot_missing_framebuffer",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene_missing_framebuffer", .view = window->view_name});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    REQUIRE(surface_rel);

    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()},
                                                     *surface_rel);
    REQUIRE(surface_abs);

    auto surface_status = SP::UI::Surface::SetScene(space,
                                                    SP::UI::SurfacePath{surface_abs->getPath()},
                                                    scene->path);
    REQUIRE(surface_status);

    auto window_view = SP::App::ConcretePathView{window_view_path};
    auto stack = SP::UI::Declarative::Stack::Create(
        space,
        window_view,
        "button_column_missing_framebuffer",
        SP::UI::Declarative::Stack::Args{.panels = {
            {.id = "hello_button",
             .fragment = SP::UI::Declarative::Button::Fragment(
                 SP::UI::Declarative::Button::Args{.label = "Say Hello"})},
        }});
    REQUIRE(stack);

    auto output_path = std::filesystem::temp_directory_path() / "pathspace_decl_missing_framebuffer.png";

    SP::UI::Screenshot::DeclarativeScreenshotOptions options{};
    options.width = kWindowWidth;
    options.height = kWindowHeight;
    options.output_png = output_path;
    options.view_name = window->view_name;
    options.wait_for_runtime_metrics = false;
    options.readiness_options.wait_for_runtime_metrics = false;
    options.readiness_options.wait_for_structure = false;
    options.readiness_options.wait_for_buckets = false;
    options.readiness_options.wait_for_revision = false;
    options.slot_timeout = std::chrono::milliseconds{200};
    options.token_timeout = std::chrono::milliseconds{200};

    auto capture = SP::UI::Screenshot::CaptureDeclarative(space,
                                                          scene->path,
                                                          window->path,
                                                          options);
    REQUIRE_FALSE(capture);
    CAPTURE(SP::describeError(capture.error()));
    CHECK_EQ(capture.error().code, SP::Error::Code::NoObjectFound);
    if (capture.error().message) {
        CHECK(capture.error().message->find("no framebuffer available") != std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove(output_path, ec);
    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative screenshot reuses cached framebuffer without another present") {
    SP::PathSpace space;
    auto launched = SP::System::LaunchStandard(space);
    REQUIRE(launched);

    auto app = SP::App::Create(space,
                               "declarative_screenshot_cached_frame",
                               {.title = "Declarative Button"});
    REQUIRE(app);

    auto window = SP::Window::Create(space,
                                     *app,
                                     "Declarative Button",
                                     kWindowWidth,
                                     kWindowHeight);
    REQUIRE(window);
    auto disable_metal = PathSpaceExamples::force_window_software_renderer(space,
                                                                           window->path,
                                                                           window->view_name);
    REQUIRE(disable_metal);

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene_cached_frame", .view = window->view_name});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    REQUIRE(surface_rel);
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()},
                                                     *surface_rel);
    REQUIRE(surface_abs);
    auto surface_status = SP::UI::Surface::SetScene(space,
                                                    SP::UI::SurfacePath{surface_abs->getPath()},
                                                    scene->path);
    REQUIRE(surface_status);

    auto window_view = SP::App::ConcretePathView{window_view_path};
    auto stack = SP::UI::Declarative::Stack::Create(
        space,
        window_view,
        "button_column_cached_frame",
        SP::UI::Declarative::Stack::Args{.panels = {
            {.id = "hello_button",
             .fragment = SP::UI::Declarative::Button::Fragment(
                 SP::UI::Declarative::Button::Args{.label = "Say Hello"})},
        }});
    REQUIRE(stack);

    SP::UI::Declarative::DeclarativeReadinessOptions readiness{};
    readiness.wait_for_runtime_metrics = false;
    readiness.wait_for_structure = false;
    readiness.wait_for_buckets = false;
    readiness.wait_for_revision = false;
    auto ready = SP::UI::Declarative::EnsureDeclarativeSceneReady(space,
                                                                  scene->path,
                                                                  window->path,
                                                                  window->view_name,
                                                                  readiness);
    REQUIRE(ready);

    auto mark_dirty = SP::UI::Declarative::SceneLifecycle::MarkDirty(space,
                                                                     scene->path,
                                                                     SP::UI::Runtime::Scene::DirtyKind::All);
    REQUIRE(mark_dirty);

    auto handles = SP::UI::Declarative::BuildPresentHandles(space, window->path, window->view_name);
    REQUIRE(handles);
    auto frame_index_path = std::string(handles->target.getPath()) + "/output/v1/common/frameIndex";

    auto first_output = std::filesystem::temp_directory_path() / "pathspace_decl_cached_frame_first.png";
    auto second_output = std::filesystem::temp_directory_path() / "pathspace_decl_cached_frame_second.png";

    SP::UI::Screenshot::DeclarativeScreenshotOptions common_options{};
    common_options.width = kWindowWidth;
    common_options.height = kWindowHeight;
    common_options.view_name = window->view_name;
    common_options.wait_for_runtime_metrics = false;
    common_options.readiness_options.wait_for_runtime_metrics = false;
    common_options.readiness_options.wait_for_structure = false;
    common_options.readiness_options.wait_for_buckets = false;
    common_options.readiness_options.wait_for_revision = false;

    auto first_options = common_options;
    first_options.output_png = first_output;
    first_options.present_before_capture = true;
    auto first_capture = SP::UI::Screenshot::CaptureDeclarative(space,
                                                                scene->path,
                                                                window->path,
                                                                first_options);
    if (!first_capture) {
        CAPTURE(SP::describeError(first_capture.error()));
    }
    REQUIRE(first_capture);

    auto before_frame_index = space.read<std::uint64_t>(frame_index_path);
    REQUIRE(before_frame_index);

    auto second_options = common_options;
    second_options.output_png = second_output;
    auto second_capture = SP::UI::Screenshot::CaptureDeclarative(space,
                                                                 scene->path,
                                                                 window->path,
                                                                 second_options);
    if (!second_capture) {
        CAPTURE(SP::describeError(second_capture.error()));
    }
    REQUIRE(second_capture);

    auto after_frame_index = space.read<std::uint64_t>(frame_index_path);
    REQUIRE(after_frame_index);
    CHECK_EQ(*before_frame_index, *after_frame_index);

    SP::System::ShutdownDeclarativeRuntime(space);
    std::error_code ec;
    std::filesystem::remove(first_output, ec);
    std::filesystem::remove(second_output, ec);
}

} // TEST_SUITE
