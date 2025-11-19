#include "declarative_example_shared.hpp"

#include <pathspace/history/UndoableSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <charconv>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>

using namespace PathSpaceExamples;

namespace {

struct CommandLineOptions {
    int width = 1280;
    int height = 800;
    bool headless = false;
    std::optional<std::filesystem::path> screenshot_path;
};

auto parse_options(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--headless") {
            opts.headless = true;
            continue;
        }
        auto parse_dimension = [&](std::string_view prefix, int& target) {
            if (!arg.starts_with(prefix)) {
                return false;
            }
            auto value_view = arg.substr(prefix.size());
            if (value_view.empty()) {
                return true;
            }
            int value = target;
            auto result = std::from_chars(value_view.begin(), value_view.end(), value);
            if (result.ec == std::errc{}) {
                target = value;
            }
            return true;
        };
        if (parse_dimension("--width=", opts.width)) {
            continue;
        }
        if (parse_dimension("--height=", opts.height)) {
            continue;
        }
        constexpr std::string_view kScreenshotPrefix = "--screenshot=";
        if (arg == "--screenshot") {
            if (i + 1 >= argc) {
                std::cerr << "paint_example: --screenshot requires a path\n";
                continue;
            }
            ++i;
            opts.screenshot_path = std::filesystem::path{argv[i]};
            opts.headless = true;
            continue;
        }
        if (arg.rfind(kScreenshotPrefix, 0) == 0) {
            auto value = arg.substr(kScreenshotPrefix.size());
            if (value.empty()) {
                std::cerr << "paint_example: --screenshot requires a path\n";
                continue;
            }
            opts.screenshot_path = std::filesystem::path{std::string{value}};
            opts.headless = true;
            continue;
        }
        std::cerr << "paint_example: ignoring unknown argument '" << arg << "'\n";
    }
    opts.width = std::max(800, opts.width);
    opts.height = std::max(600, opts.height);
    return opts;
}

struct PaletteColor {
    const char* id;
    const char* label;
    std::array<float, 4> color;
};

auto palette_colors() -> std::vector<PaletteColor> {
    return {
        {"paint_palette_red", "Red", {0.905f, 0.173f, 0.247f, 1.0f}},
        {"paint_palette_orange", "Orange", {0.972f, 0.545f, 0.192f, 1.0f}},
        {"paint_palette_yellow", "Yellow", {0.995f, 0.847f, 0.207f, 1.0f}},
        {"paint_palette_green", "Green", {0.172f, 0.701f, 0.368f, 1.0f}},
        {"paint_palette_blue", "Blue", {0.157f, 0.407f, 0.933f, 1.0f}},
        {"paint_palette_purple", "Purple", {0.560f, 0.247f, 0.835f, 1.0f}},
    };
}

auto log_error(SP::Expected<void> const& status, std::string const& context) -> void {
    if (status) {
        return;
    }
    auto const& error = status.error();
    std::cerr << "paint_example: " << context << " failed";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

template <typename T>
auto replace_value(SP::PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void> {
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto format_brush_state(float size, std::array<float, 4> const& color) -> std::string {
    std::ostringstream stream;
    stream << "Brush size: " << std::clamp(size, 1.0f, 128.0f) << " | Color: rgb("
           << std::clamp(color[0], 0.0f, 1.0f) << ", "
           << std::clamp(color[1], 0.0f, 1.0f) << ", "
           << std::clamp(color[2], 0.0f, 1.0f) << ")";
    return stream.str();
}

auto apply_brush_size(SP::PathSpace& space, std::string const& widget_path, float size) -> SP::Expected<void> {
    return replace_value(space, widget_path + "/state/brush/size", size);
}

auto apply_brush_color(SP::PathSpace& space,
                       std::string const& widget_path,
                       std::array<float, 4> const& color) -> SP::Expected<void> {
    return replace_value(space, widget_path + "/state/brush/color", color);
}

auto log_expected_error(std::string const& context, SP::Error const& error) -> void;

using WidgetAction = SP::UI::Builders::Widgets::Reducers::WidgetAction;
using WidgetOpKind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind;

auto make_paint_action(std::string const& widget_path,
                       WidgetOpKind kind,
                       std::uint64_t stroke_id,
                       float x,
                       float y) -> WidgetAction {
    WidgetAction action{};
    action.widget_path = widget_path;
    action.kind = kind;
    action.target_id = std::string{"paint_surface/stroke/"} + std::to_string(stroke_id);
    action.pointer.has_local = true;
    action.pointer.local_x = x;
    action.pointer.local_y = y;
    return action;
}

auto scripted_stroke_actions(std::string const& widget_path) -> std::vector<WidgetAction> {
    constexpr std::uint64_t kPrimaryStroke = 1;
    constexpr std::uint64_t kAccentStroke = 2;
    std::vector<WidgetAction> actions;
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeBegin,
                                       kPrimaryStroke,
                                       80.0f,
                                       120.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeUpdate,
                                       kPrimaryStroke,
                                       320.0f,
                                       260.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeCommit,
                                       kPrimaryStroke,
                                       460.0f,
                                       420.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeBegin,
                                       kAccentStroke,
                                       420.0f,
                                       140.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeCommit,
                                       kAccentStroke,
                                       160.0f,
                                       420.0f));
    return actions;
}

auto playback_scripted_strokes(SP::PathSpace& space, std::string const& widget_path) -> bool {
    auto actions = scripted_stroke_actions(widget_path);
    for (auto& action : actions) {
        auto handled = SP::UI::Declarative::PaintRuntime::HandleAction(space, action);
        if (!handled) {
            log_expected_error("PaintRuntime::HandleAction", handled.error());
            return false;
        }
        if (!*handled) {
            std::cerr << "paint_example: scripted stroke had no effect\n";
            return false;
        }
    }
    return true;
}

auto write_framebuffer_png(SP::UI::Builders::Window::WindowPresentResult const& present,
                           std::filesystem::path const& output_path,
                           int width,
                           int height) -> bool {
    auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "paint_example: failed to create directory '" << parent.string() << "': " << ec.message() << '\n';
            return false;
        }
    }
    if (present.framebuffer.empty() || width <= 0 || height <= 0) {
        std::cerr << "paint_example: framebuffer capture is empty\n";
        return false;
    }
    auto row_stride = width * 4;
    if (stbi_write_png(output_path.string().c_str(),
                       width,
                       height,
                       4,
                       present.framebuffer.data(),
                       row_stride) == 0) {
        std::cerr << "paint_example: failed to write PNG to '" << output_path.string() << "'\n";
        return false;
    }
    return true;
}

struct HistoryBinding {
    std::shared_ptr<SP::History::UndoableSpace> undo;
    std::string root;
};

auto make_history_binding(SP::PathSpace& space, std::string root_path) -> SP::Expected<HistoryBinding> {
    auto upstream = std::shared_ptr<SP::PathSpaceBase>(&space, [](SP::PathSpaceBase*) {});
    auto alias = std::make_unique<SP::PathAlias>(upstream, "/");
    SP::History::HistoryOptions defaults{};
    defaults.allowNestedUndo = true;
    defaults.maxEntries = 1024;
    defaults.ramCacheEntries = 64;
    defaults.useMutationJournal = true;
    auto undo_space = std::make_shared<SP::History::UndoableSpace>(std::move(alias), defaults);
    auto enable = undo_space->enableHistory(SP::ConcretePathStringView{root_path});
    if (!enable) {
        return std::unexpected(enable.error());
    }
    return HistoryBinding{
        .undo = std::move(undo_space),
        .root = std::move(root_path),
    };
}

struct BrushState {
    float size = 12.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

auto log_expected_error(std::string const& context, SP::Error const& error) -> void {
    std::cerr << "paint_example: " << context << " error (code=" << static_cast<int>(error.code) << ")";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);

    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::cerr << "paint_example: failed to launch declarative runtime\n";
        return 1;
    }

    auto app = SP::App::Create(space,
                               "paint_example",
                               {.title = "Declarative Paint"});
    if (!app) {
        std::cerr << "paint_example: failed to create app\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "paint_window";
    window_opts.title = "Declarative Paint Surface";
    window_opts.width = options.width;
    window_opts.height = options.height;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    if (!window) {
        std::cerr << "paint_example: failed to create window\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "paint_scene";
    scene_opts.description = "Declarative paint scene";
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    if (!scene) {
        std::cerr << "paint_example: failed to create scene\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto scene_result = *scene;

    auto bootstrap = build_bootstrap_from_window(space,
                                                 app_root_view,
                                                 window->path,
                                                 window->view_name);
    if (!bootstrap) {
        log_expected_error("failed to prepare presenter bootstrap", bootstrap.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    bootstrap->present_policy.capture_framebuffer = true;

    auto bind_scene = SP::UI::Builders::Surface::SetScene(space, (*bootstrap).surface, scene_result.path);
    if (!bind_scene) {
        log_expected_error("Surface::SetScene", bind_scene.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    ensure_device_push_config(space, std::string{kPointerDevice}, "paint_example");
    ensure_device_push_config(space, std::string{kKeyboardDevice}, "paint_example");
    auto pointer_devices = std::vector<std::string>{std::string{kPointerDevice}};
    auto keyboard_devices = std::vector<std::string>{std::string{kKeyboardDevice}};
    subscribe_window_devices(space,
                             window->path,
                             std::span<const std::string>(pointer_devices),
                             std::span<const std::string>{},
                             std::span<const std::string>(keyboard_devices));

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto status_label = SP::UI::Declarative::Label::Create(space,
                                                           window_view,
                                                           "status_label",
                                                           {.text = "Pick a color and drag on the canvas"});
    if (!status_label) {
        std::cerr << "paint_example: failed to create status label\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto brush_state = std::make_shared<BrushState>();
    auto brush_label = SP::UI::Declarative::Label::Create(space,
                                                          window_view,
                                                          "brush_label",
                                                          {.text = format_brush_state(brush_state->size,
                                                                                      brush_state->color)});
    if (!brush_label) {
        std::cerr << "paint_example: failed to create brush label\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::UI::Declarative::PaintSurface::Args paint_args{};
    paint_args.brush_size = brush_state->size;
    paint_args.brush_color = brush_state->color;
    paint_args.buffer_width = static_cast<std::uint32_t>(options.width);
    paint_args.buffer_height = static_cast<std::uint32_t>(options.height);
    paint_args.on_draw = [status_label_path = *status_label](SP::UI::Declarative::PaintSurfaceContext& ctx) {
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      "Stroke recorded"),
                  "Label::SetText");
    };
    auto paint_surface = SP::UI::Declarative::PaintSurface::Create(space,
                                                                   window_view,
                                                                   "paint_surface",
                                                                   paint_args);
    if (!paint_surface) {
        std::cerr << "paint_example: failed to create paint surface\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    std::string paint_widget_path = paint_surface->getPath();

    auto history_binding_result = make_history_binding(space, paint_widget_path);
    if (!history_binding_result) {
        log_expected_error("failed to enable UndoableSpace history", history_binding_result.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto history_binding = std::make_shared<HistoryBinding>(std::move(*history_binding_result));

    SP::UI::Declarative::Slider::Args slider_args{};
    slider_args.minimum = 1.0f;
    slider_args.maximum = 64.0f;
    slider_args.step = 1.0f;
    slider_args.value = brush_state->size;
    slider_args.on_change = [brush_state,
                             paint_widget_path,
                             brush_label_path = *brush_label,
                             status_label_path = *status_label](SP::UI::Declarative::SliderContext& ctx) {
        brush_state->size = ctx.value;
        auto status = apply_brush_size(ctx.space, paint_widget_path, brush_state->size);
        if (!status) {
            log_error(status, "apply_brush_size");
            return;
        }
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      brush_label_path,
                                                      format_brush_state(brush_state->size, brush_state->color)),
                  "Label::SetText");
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      "Updated brush size"),
                  "Label::SetText");
    };
    auto brush_slider = SP::UI::Declarative::Slider::Create(space,
                                                            window_view,
                                                            "brush_slider",
                                                            slider_args);
    if (!brush_slider) {
        std::cerr << "paint_example: failed to create brush slider\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    for (auto const& entry : palette_colors()) {
        SP::UI::Declarative::Button::Args palette_args{};
        palette_args.label = entry.label;
        palette_args.on_press = [brush_state,
                                 paint_widget_path,
                                 brush_label_path = *brush_label,
                                 status_label_path = *status_label,
                                 entry](SP::UI::Declarative::ButtonContext& ctx) {
            brush_state->color = entry.color;
            auto status = apply_brush_color(ctx.space, paint_widget_path, brush_state->color);
            if (!status) {
                log_error(status, "apply_brush_color");
                return;
            }
            log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                          brush_label_path,
                                                          format_brush_state(brush_state->size, brush_state->color)),
                      "Label::SetText");
            std::ostringstream message;
            message << "Selected " << entry.label << " paint";
            log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                          status_label_path,
                                                          message.str()),
                      "Label::SetText");
        };
        auto button = SP::UI::Declarative::Button::Create(space,
                                                          window_view,
                                                          entry.id,
                                                          palette_args);
        if (!button) {
            std::cerr << "paint_example: failed to create palette button '" << entry.label << "'\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
    }

    auto undo_button = SP::UI::Declarative::Button::Create(
        space,
        window_view,
        "undo_button",
        SP::UI::Declarative::Button::Args{
            .label = "Undo Stroke",
            .on_press = [history_binding,
                         status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
                auto undo = history_binding->undo->undo(SP::ConcretePathStringView{history_binding->root});
                if (!undo) {
                    log_error(undo, "UndoableSpace::undo");
                    return;
                }
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              status_label_path,
                                                              "Undo applied"),
                          "Label::SetText");
            },
        });
    if (!undo_button) {
        std::cerr << "paint_example: failed to create undo button\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto redo_button = SP::UI::Declarative::Button::Create(
        space,
        window_view,
        "redo_button",
        SP::UI::Declarative::Button::Args{
            .label = "Redo Stroke",
            .on_press = [history_binding,
                         status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
                auto redo = history_binding->undo->redo(SP::ConcretePathStringView{history_binding->root});
                if (!redo) {
                    log_error(redo, "UndoableSpace::redo");
                    return;
                }
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              status_label_path,
                                                              "Redo applied"),
                          "Label::SetText");
            },
        });
    if (!redo_button) {
        std::cerr << "paint_example: failed to create redo button\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    bool screenshot_mode = options.screenshot_path.has_value();
    if (screenshot_mode) {
        if (!playback_scripted_strokes(space, paint_widget_path)) {
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        options.headless = true;
    }

    if (options.headless && !screenshot_mode) {
        std::cout << "paint_example: headless mode enabled, declarative widgets mounted at\n"
                  << "  " << paint_widget_path << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    PresentLoopHooks hooks{};
    bool screenshot_taken = false;
    std::filesystem::path screenshot_path;
    if (screenshot_mode) {
        screenshot_path = *options.screenshot_path;
        auto view_base = std::string(window->path.getPath()) + "/views/" + window->view_name;
        space.insert(view_base + "/present/params/capture_framebuffer", true);
        hooks.on_present = [&](SP::UI::Builders::Window::WindowPresentResult const& present) {
            if (screenshot_taken) {
                return;
            }
            if (present.framebuffer.empty()) {
                std::cerr << "paint_example: debug framebuffer empty" << std::endl;
                return;
            }
            int capture_width = bootstrap->surface_desc.size_px.width;
            int capture_height = bootstrap->surface_desc.size_px.height;
            if (write_framebuffer_png(present, screenshot_path, capture_width, capture_height)) {
                std::cout << "paint_example: saved screenshot to " << screenshot_path.string() << "\n";
            } else {
                std::cerr << "paint_example: failed to save screenshot to '" << screenshot_path.string() << "'\n";
            }
            screenshot_taken = true;
            SP::UI::RequestLocalWindowQuit();
        };
    }

    run_present_loop(space,
                     window->path,
                     window->view_name,
                     *bootstrap,
                     options.width,
                     options.height,
                     hooks);

    if (screenshot_mode && !screenshot_taken) {
        std::cerr << "paint_example: screenshot request did not complete\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
