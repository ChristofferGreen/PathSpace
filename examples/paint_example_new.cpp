#include "declarative_example_shared.hpp"

#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/examples/paint/PaintExampleNewUI.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/declarative/StackReadiness.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

using PathSpaceExamples::LocalInputBridge;
using PathSpaceExamples::install_local_window_bridge;

struct Options {
    int width = 800;
    int height = 600;
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> screenshot_compare_path;
    std::optional<std::filesystem::path> screenshot_diff_path;
    std::optional<std::filesystem::path> screenshot_metrics_path;
    double screenshot_max_mean_error = 0.0015;
    bool screenshot_require_present = false;
};

auto parse_options(int argc, char** argv) -> Options {
    Options opts;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("paint_example_new");

    cli.add_int("--width", {.on_value = [&](int value) { opts.width = value; }});
    cli.add_int("--height", {.on_value = [&](int value) { opts.height = value; }});
    cli.add_double("--screenshot-max-mean-error", {.on_value = [&](double value) {
                       opts.screenshot_max_mean_error = value;
                   }});
    cli.add_flag("--screenshot-require-present", {.on_set = [&]() {
                     opts.screenshot_require_present = true;
                 }});
    cli.add_value("--screenshot", {.on_value = [&](std::optional<std::string_view> value)
                                                 -> ExampleCli::ParseError {
        if (!value) {
            return "missing value for --screenshot";
        }
        opts.screenshot_path = std::filesystem::path{std::string(*value)};
        return std::nullopt;
    }});
    cli.add_value("--screenshot-compare",
                  {.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
                      if (!value) {
                          return "missing value for --screenshot-compare";
                      }
                      opts.screenshot_compare_path = std::filesystem::path{std::string(*value)};
                      return std::nullopt;
                  }});
    cli.add_value("--screenshot-diff",
                  {.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
                      if (!value) {
                          return "missing value for --screenshot-diff";
                      }
                      opts.screenshot_diff_path = std::filesystem::path{std::string(*value)};
                      return std::nullopt;
                  }});
    cli.add_value("--screenshot-metrics",
                  {.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
                      if (!value) {
                          return "missing value for --screenshot-metrics";
                      }
                      opts.screenshot_metrics_path = std::filesystem::path{std::string(*value)};
                      return std::nullopt;
                  }});

    (void)cli.parse(argc, argv);

    opts.width = std::clamp(opts.width, 320, 3840);
    opts.height = std::clamp(opts.height, 240, 2160);
    return opts;
}

auto exit_with_error(SP::PathSpace& space,
                     std::string_view message,
                     std::optional<SP::Error> error = std::nullopt) -> int {
    std::cerr << "paint_example_new: " << message;
    if (error) {
        std::cerr << " (" << SP::describeError(*error) << ")";
    }
    std::cerr << "\n";
    SP::System::ShutdownDeclarativeRuntime(space);
    return 1;
}

auto window_component_name(SP::UI::WindowPath const& window_path) -> std::string {
    auto raw = std::string(window_path.getPath());
    auto slash = raw.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= raw.size()) {
        return raw;
    }
    return raw.substr(slash + 1);
}

auto capture_screenshot(SP::PathSpace& space,
                        SP::Scene::CreateResult const& scene,
                        SP::Window::CreateResult const& window,
                        std::string const& window_widgets_root,
                        Options const& options) -> SP::Expected<void> {
    bool debug_logging = std::getenv("PAINT_EXAMPLE_NEW_DEBUG") != nullptr;
    auto readiness = PathSpaceExamples::ensure_declarative_scene_ready(space,
                                                                       scene.path,
                                                                       window.path,
                                                                       window.view_name);
    if (!readiness) {
        return std::unexpected(readiness.error());
    }

    auto widget_count = readiness->widget_count;
    if (widget_count == 0) {
        widget_count = space.listChildren(SP::ConcretePathStringView{window_widgets_root}).size();
        if (widget_count == 0) {
            widget_count = 1; // expect button widget
        }
    }

    auto scene_widgets_root = PathSpaceExamples::make_scene_widgets_root(scene.path,
                                                                         window.path,
                                                                         window.view_name);
    if (debug_logging) {
        auto scene_widgets = space.listChildren(SP::ConcretePathStringView{scene_widgets_root});
        std::cerr << "paint_example_new(debug): scene widgets (capture) =";
        for (auto const& name : scene_widgets) {
            std::cerr << " " << name;
        }
        if (scene_widgets.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
    }

    auto readiness_verify = PathSpaceExamples::ensure_declarative_scene_ready(space,
                                                                              scene.path,
                                                                              window.path,
                                                                              window.view_name);
    if (!readiness_verify) {
        return std::unexpected(readiness_verify.error());
    }
    auto scene_widget_entries = space.listChildren(SP::ConcretePathStringView{scene_widgets_root});
    if (debug_logging) {
        std::cerr << "paint_example_new(debug): scene widgets (post-wait) =";
        for (auto const& name : scene_widget_entries) {
            std::cerr << " " << name;
        }
        if (scene_widget_entries.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
        if (!scene_widget_entries.empty()) {
            auto bucket_path = scene_widgets_root + "/" + scene_widget_entries.front() + "/render/bucket/drawables.bin";
            auto bucket_data = space.read<std::vector<std::uint8_t>, std::string>(bucket_path);
            if (bucket_data) {
                std::cerr << "paint_example_new(debug): capture bucket bytes=" << bucket_data->size() << "\n";
            } else {
                std::cerr << "paint_example_new(debug): capture bucket read error "
                          << SP::describeError(bucket_data.error()) << "\n";
            }
        }
    }
    auto mark_dirty = SP::UI::Declarative::SceneLifecycle::MarkDirty(space,
                                                                     scene.path,
                                                                     SP::UI::Runtime::Scene::DirtyKind::All);
    if (!mark_dirty) {
        return std::unexpected(mark_dirty.error());
    }

    auto revision_path = std::string(scene.path.getPath()) + "/current_revision";
    auto current_revision = space.read<std::uint64_t, std::string>(revision_path);
    std::uint64_t target_revision = current_revision.value_or(0);

    SP::UI::Declarative::SceneLifecycle::ForcePublishOptions publish_options{};
    publish_options.min_revision = target_revision;
    auto forced = SP::UI::Declarative::SceneLifecycle::ForcePublish(space, scene.path, publish_options);
    if (!forced) {
        return std::unexpected(forced.error());
    }
    target_revision = std::max(target_revision, *forced);
    auto wait_floor = target_revision > 0 ? target_revision - 1 : 0;
    auto ready_revision = PathSpaceExamples::wait_for_declarative_scene_revision(space,
                                                                                scene.path,
                                                                                std::chrono::seconds(5),
                                                                                wait_floor);
    if (!ready_revision) {
        return std::unexpected(ready_revision.error());
    }
    if (debug_logging) {
        auto format_revision = [](std::uint64_t revision) {
            std::ostringstream oss;
            oss << std::setw(16) << std::setfill('0') << revision;
            return oss.str();
        };
        auto revision_base = std::string(scene.path.getPath()) + "/builds/" + format_revision(*ready_revision);
        auto bucket = SP::UI::Scene::SceneSnapshotBuilder::decode_bucket(space, revision_base);
        if (!bucket) {
            std::cerr << "paint_example_new(debug): decode_bucket failed "
                      << SP::describeError(bucket.error()) << "\n";
        } else {
            std::cerr << "paint_example_new(debug): bucket drawable count=" << bucket->drawable_ids.size() << "\n";
            for (std::size_t i = 0; i < bucket->bounds_boxes.size(); ++i) {
                auto const& box = bucket->bounds_boxes[i];
                std::cerr << "  drawable[" << i << "] bounds=(" << box.min[0]
                          << ", " << box.min[1] << ") - (" << box.max[0]
                          << ", " << box.max[1] << ")";
                if (i < bucket->authoring_map.size()) {
                    std::cerr << " authoring=" << bucket->authoring_map[i].authoring_node_id;
                }
                std::cerr << "\n";
            }
        }
    }

    SP::UI::Screenshot::ScreenshotRequest request{
        .space = space,
        .window_path = window.path,
        .view_name = window.view_name,
        .width = options.width,
        .height = options.height,
        .output_png = *options.screenshot_path,
        .baseline_png = options.screenshot_compare_path,
        .diff_png = options.screenshot_diff_path,
        .metrics_json = options.screenshot_metrics_path,
        .max_mean_error = options.screenshot_max_mean_error,
        .require_present = options.screenshot_require_present
                            || options.screenshot_compare_path.has_value(),
        .present_timeout = std::chrono::milliseconds(2000),
    };
    request.telemetry_namespace = "paint_example_new";

    auto capture = SP::UI::Screenshot::ScreenshotService::Capture(request);
    if (!capture) {
        return std::unexpected(capture.error());
    }
    if (capture->matched_baseline) {
        std::cout << "paint_example_new: screenshot matched baseline (mean error "
                  << capture->mean_error.value_or(0.0) << ")\n";
    } else {
        std::cout << "paint_example_new: saved screenshot to "
                  << options.screenshot_path->string() << "\n";
    }
    return {};
}

auto enable_framebuffer_capture(SP::PathSpace& space,
                                SP::Window::CreateResult const& window,
                                bool enabled) -> SP::Expected<void> {
    auto capture_path = std::string(window.path.getPath())
                        + "/views/" + window.view_name + "/present/params/capture_framebuffer";
    auto inserted = space.insert(capture_path, enabled);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto bind_scene_to_surface(SP::PathSpace& space,
                           SP::App::AppRootPath const& app_root,
                           SP::Scene::CreateResult const& scene,
                           SP::Window::CreateResult const& window) -> SP::Expected<void> {
    auto view_base = std::string(window.path.getPath()) + "/views/" + window.view_name;
    auto surface_rel = space.read<std::string, std::string>(view_base + "/surface");
    if (!surface_rel) {
        return std::unexpected(surface_rel.error());
    }
    if (surface_rel->empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidPath, "window view missing surface binding"});
    }
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root.getPath()},
                                                     *surface_rel);
    if (!surface_abs) {
        return std::unexpected(surface_abs.error());
    }
    auto surface_path = SP::UI::SurfacePath{surface_abs->getPath()};
    return SP::UI::Surface::SetScene(space, surface_path, scene.path);
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);

    SP::PathSpace space;
    auto ensure_devices = PathSpaceExamples::PaintExampleNew::EnsureInputDevices(space);
    if (!ensure_devices) {
        return exit_with_error(space, "EnsureInputDevices failed", ensure_devices.error());
    }
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        return exit_with_error(space, "LaunchStandard failed", launch.error());
    }

    auto app = SP::App::Create(space, "paint_example_new", {.title = "Declarative Button"});
    if (!app) {
        return exit_with_error(space, "App::Create failed", app.error());
    }

    auto window = SP::Window::Create(space, *app, "Declarative Button", options.width, options.height);
    if (!window) {
        return exit_with_error(space, "Window::Create failed", window.error());
    }
    if (auto capture = enable_framebuffer_capture(space, *window, true); !capture) {
        return exit_with_error(space, "failed to enable framebuffer capture", capture.error());
    }

    auto scene = SP::Scene::Create(space, *app, window->path);
    if (!scene) {
        return exit_with_error(space, "Scene::Create failed", scene.error());
    }
    if (std::getenv("PAINT_EXAMPLE_NEW_DEBUG")) {
        std::cerr << "paint_example_new(debug): scene path=" << scene->path.getPath() << "\n";
    }
    if (auto bind = bind_scene_to_surface(space, *app, *scene, *window); !bind) {
        return exit_with_error(space, "Surface::SetScene failed", bind.error());
    }

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};
    auto window_widgets_root = window_view_path + "/widgets";

    auto pressed_toggle = std::make_shared<std::atomic<bool>>(false);

    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Press Me";
    button_args.style.width = 240.0f;
    button_args.style.height = 64.0f;
    button_args.style.corner_radius = 16.0f;
    button_args.style.text_color = {0.95f, 0.98f, 1.0f, 1.0f};
    button_args.style.typography.font_size = 30.0f;
    button_args.style.typography.line_height = 36.0f;
    button_args.on_press = [pressed_toggle](SP::UI::Declarative::ButtonContext& ctx) {
        bool new_state = !pressed_toggle->load(std::memory_order_acquire);
        pressed_toggle->store(new_state, std::memory_order_release);
        auto label = new_state ? "Thanks!" : "Press Me";
        if (auto status = SP::UI::Declarative::Button::SetLabel(ctx.space, ctx.widget, label); !status) {
            std::cerr << "paint_example_new: failed to update button label ("
                      << SP::describeError(status.error()) << ")\n";
        }
        std::cout << "paint_example_new: button pressed (" << (new_state ? "armed" : "reset") << ")\n";
    };

    auto mounted_ui = PathSpaceExamples::PaintExampleNew::MountButtonUI(space,
                                                                        window_view,
                                                                        options.width,
                                                                        options.height,
                                                                        std::move(button_args));
    if (!mounted_ui) {
        return exit_with_error(space, "MountButtonUI failed", mounted_ui.error());
    }
    auto stack_root = mounted_ui->stack_path.getPath();
    auto button_path = mounted_ui->button_path;
    auto layout_width = mounted_ui->layout_width;
    auto layout_height = mounted_ui->layout_height;

    auto enable_input = PathSpaceExamples::PaintExampleNew::EnableWindowInput(space, *window, "paint_example_new");
    if (!enable_input) {
        return exit_with_error(space, "EnableWindowInput failed", enable_input.error());
    }
    if (std::getenv("PAINT_EXAMPLE_NEW_DEBUG")) {
        auto lifecycle_state_path = std::string(scene->path.getPath()) + "/runtime/lifecycle/state/running";
        if (auto running = space.read<bool, std::string>(lifecycle_state_path); running) {
            std::cerr << "paint_example_new(debug): lifecycle running=" << (*running ? "true" : "false") << "\n";
        }
        auto layout_children_path = stack_root + "/layout/children";
        auto layout_children =
            space.read<std::vector<SP::UI::Runtime::Widgets::StackChildSpec>, std::string>(layout_children_path);
        if (layout_children) {
            std::cerr << "paint_example_new(debug): layout children ids=";
            if (layout_children->empty()) {
                std::cerr << " <none>";
            }
            for (auto const& spec : *layout_children) {
                std::cerr << " " << spec.id;
            }
            std::cerr << "\n";
        } else {
            std::cerr << "paint_example_new(debug): layout children read error "
                      << SP::describeError(layout_children.error()) << "\n";
        }
        auto print_children = [&](std::string const& root, int depth, auto&& self_ref) -> void {
            auto entries = space.listChildren(SP::ConcretePathStringView{root});
            for (auto const& entry : entries) {
                std::cerr << std::string(depth * 2, ' ') << "- " << entry << "\n";
                self_ref(root + "/" + entry, depth + 1, self_ref);
            }
        };
        auto widget_names = space.listChildren(SP::ConcretePathStringView{window_widgets_root});
        std::cerr << "paint_example_new(debug): window widgets=";
        for (auto const& name : widget_names) {
            std::cerr << " " << name;
        }
        if (widget_names.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
        auto window_component = window_component_name(window->path);
        auto scene_widgets_root = std::string(scene->path.getPath())
                                  + "/structure/widgets/windows/" + window_component
                                  + "/views/" + window->view_name + "/widgets";
        auto scene_window_root = std::string(scene->path.getPath()) + "/structure/widgets/windows";
        auto scene_window_ids = space.listChildren(SP::ConcretePathStringView{scene_window_root});
        std::cerr << "paint_example_new(debug): scene window ids=";
        for (auto const& id : scene_window_ids) {
            std::cerr << " " << id;
        }
        if (scene_window_ids.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
        auto scene_widgets = space.listChildren(SP::ConcretePathStringView{scene_widgets_root});
        std::cerr << "paint_example_new(debug): scene widgets=";
        for (auto const& name : scene_widgets) {
            std::cerr << " " << name;
        }
        if (scene_widgets.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
        auto lifecycle_error_queue = std::string(scene->path.getPath()) + "/runtime/lifecycle/log/errors/queue";
        if (auto lifecycle_error = space.take<std::string>(lifecycle_error_queue); lifecycle_error) {
            std::cerr << "paint_example_new(debug): lifecycle error " << *lifecycle_error << "\n";
        }
        for (auto const& name : widget_names) {
            std::cerr << "paint_example_new(debug): subtree for " << name << "\n";
            print_children(window_widgets_root + "/" + name, 1, print_children);
            auto target_path =
                space.read<std::string, std::string>(window_widgets_root + "/" + name + "/panels/button_panel/target");
            if (target_path) {
                std::cerr << "paint_example_new(debug): panel target=" << *target_path << "\n";
            }
            auto button_root = window_widgets_root + "/" + name + "/children/button_panel";
            auto style_value =
                space.read<SP::UI::Runtime::Widgets::ButtonStyle, std::string>(button_root + "/meta/style");
            if (style_value) {
                auto const& bg = style_value->background_color;
                std::cerr << "paint_example_new(debug): button style background=(" << bg[0] << ", " << bg[1]
                          << ", " << bg[2] << ", " << bg[3] << ")\n";
            }
            if (auto label = space.read<std::string, std::string>(button_root + "/meta/label"); label) {
                std::cerr << "paint_example_new(debug): button label='" << *label << "'\n";
            }
            auto error_queue = window_widgets_root + "/" + name + "/render/log/errors/queue";
            if (auto error = space.take<std::string>(error_queue); error) {
                std::cerr << "paint_example_new(debug): render error " << *error << "\n";
            }
            auto bucket_path = scene_widgets_root + "/" + name;
            auto bucket_file = bucket_path + "/render/bucket/drawables.bin";
            auto bucket_data = space.read<std::vector<std::uint8_t>, std::string>(bucket_file);
            if (bucket_data) {
                std::cerr << "paint_example_new(debug): bucket payload bytes=" << bucket_data->size() << "\n";
            } else {
                std::cerr << "paint_example_new(debug): bucket read error "
                          << SP::describeError(bucket_data.error()) << "\n";
            }
        }
    }

    auto readiness = PathSpaceExamples::ensure_declarative_scene_ready(space,
                                                                       scene->path,
                                                                       window->path,
                                                                       window->view_name);
    if (!readiness) {
        return exit_with_error(space, "scene readiness failed", readiness.error());
    }

    if (options.screenshot_path) {
        auto screenshot = capture_screenshot(space, *scene, *window, window_widgets_root, options);
        if (!screenshot) {
            return exit_with_error(space, "screenshot capture failed", screenshot.error());
        }
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    auto run = SP::App::RunUI(space,
                              *scene,
                              *window,
                              {.window_width = options.width,
                               .window_height = options.height,
                               .window_title = "Declarative Button"});
    if (!run) {
        return exit_with_error(space, "App::RunUI failed", run.error());
    }

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
