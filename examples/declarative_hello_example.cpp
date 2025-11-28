#include "declarative_example_shared.hpp"

#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <third_party/stb_image_write.h>

using namespace PathSpaceExamples;

namespace {

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 520;

auto make_runtime_error(std::string message) -> SP::Error {
    return SP::Error{SP::Error::Code::UnknownError, std::move(message)};
}

auto env_truthy(char const* value) -> bool {
    if (!value) {
        return false;
    }
    std::string normalized{value};
    if (normalized.empty()) {
        return false;
    }
    for (auto& ch : normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

auto log_error(SP::Expected<void> const& status, std::string const& context) -> void {
    if (status) {
        return;
    }
    auto const& error = status.error();
    std::cerr << "declarative_hello_example: " << context << " failed";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

auto enable_capture_framebuffer(SP::PathSpace& space,
                                SP::UI::WindowPath const& window,
                                std::string const& view_name,
                                bool enabled) -> SP::Expected<void> {
    auto present_params = std::string(window.getPath()) + "/views/" + view_name
                          + "/present/params/capture_framebuffer";
    auto inserted = space.insert(present_params, enabled);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

auto write_png_rgba(std::filesystem::path const& path,
                    int width,
                    int height,
                    std::span<const std::uint8_t> pixels) -> SP::Expected<void> {
    if (width <= 0 || height <= 0) {
        return std::unexpected(make_runtime_error("invalid screenshot dimensions"));
    }
    auto required = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (pixels.size() != required) {
        return std::unexpected(make_runtime_error("framebuffer length mismatch"));
    }
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(make_runtime_error("failed to create screenshot directory"));
        }
    }
    auto stride = static_cast<int>(width * 4);
    if (stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), stride) == 0) {
        return std::unexpected(make_runtime_error("failed to encode screenshot"));
    }
    return {};
}

auto make_canvas(int width, int height, Rgba color) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
            canvas[idx + 0] = color.r;
            canvas[idx + 1] = color.g;
            canvas[idx + 2] = color.b;
            canvas[idx + 3] = color.a;
        }
    }
    return canvas;
}

auto clamp(int value, int min_value, int max_value) -> int {
    return std::max(min_value, std::min(value, max_value));
}

void fill_rect(std::vector<std::uint8_t>& canvas,
               int width,
               int height,
               int x,
               int y,
               int w,
               int h,
               Rgba color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    int x_end = clamp(x + w, 0, width);
    int y_end = clamp(y + h, 0, height);
    int x_start = clamp(x, 0, width);
    int y_start = clamp(y, 0, height);
    for (int py = y_start; py < y_end; ++py) {
        for (int px = x_start; px < x_end; ++px) {
            auto idx = (static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)) * 4u;
            canvas[idx + 0] = color.r;
            canvas[idx + 1] = color.g;
            canvas[idx + 2] = color.b;
            canvas[idx + 3] = color.a;
        }
    }
}

void draw_rect_outline(std::vector<std::uint8_t>& canvas,
                       int width,
                       int height,
                       int x,
                       int y,
                       int w,
                       int h,
                       int stroke,
                       Rgba color) {
    fill_rect(canvas, width, height, x, y, w, stroke, color);
    fill_rect(canvas, width, height, x, y + h - stroke, w, stroke, color);
    fill_rect(canvas, width, height, x, y, stroke, h, color);
    fill_rect(canvas, width, height, x + w - stroke, y, stroke, h, color);
}

struct Glyph {
    std::array<std::uint8_t, 7> rows{};
};

auto glyph_map() -> std::unordered_map<char, Glyph> {
    return {
        {'A', {{0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}}},
        {'B', {{0b11110, 0b10001, 0b11110, 0b10001, 0b10001, 0b10001, 0b11110}}},
        {'C', {{0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}}},
        {'E', {{0b11111, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000, 0b11111}}},
        {'G', {{0b01110, 0b10001, 0b10000, 0b10011, 0b10001, 0b10001, 0b01110}}},
        {'H', {{0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001, 0b10001}}},
        {'I', {{0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}}},
        {'J', {{0b00001, 0b00001, 0b00001, 0b00001, 0b10001, 0b10001, 0b01110}}},
        {'K', {{0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}}},
        {'L', {{0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}}},
        {'N', {{0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}}},
        {'O', {{0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}}},
        {'P', {{0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}}},
        {'R', {{0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}}},
        {'S', {{0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}}},
        {'T', {{0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}}},
        {'U', {{0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}}},
        {'W', {{0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010, 0b01010}}},
        {'Y', {{0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}}},
    };
}

void draw_char(std::vector<std::uint8_t>& canvas,
               int width,
               int height,
               int x,
               int y,
               Glyph const& glyph,
               int scale,
               Rgba color) {
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((glyph.rows[row] >> (4 - col)) & 0x1) {
                fill_rect(canvas,
                          width,
                          height,
                          x + col * scale,
                          y + row * scale,
                          scale,
                          scale,
                          color);
            }
        }
    }
}

void draw_text(std::vector<std::uint8_t>& canvas,
               int width,
               int height,
               int x,
               int y,
               std::string_view text,
               int scale,
               Rgba color) {
    static auto glyphs = glyph_map();
    int cursor_x = x;
    for (char ch : text) {
        if (ch == ' ') {
            cursor_x += (5 * scale) + scale;
            continue;
        }
        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = glyphs.find(upper);
        if (it != glyphs.end()) {
            draw_char(canvas, width, height, cursor_x, y, it->second, scale, color);
            cursor_x += (5 * scale) + scale;
        } else {
            cursor_x += (5 * scale) + scale;
        }
    }
}

auto render_reference_hello_png(std::filesystem::path const& path) -> SP::Expected<void> {
    constexpr int width = kWindowWidth;
    constexpr int height = kWindowHeight;
    auto canvas = make_canvas(width, height, Rgba{240, 242, 245, 255});

    fill_rect(canvas, width, height, 40, 60, width - 80, height - 120, Rgba{255, 255, 255, 255});
    draw_rect_outline(canvas, width, height, 40, 60, width - 80, height - 120, 2, Rgba{220, 223, 228, 255});

    draw_text(canvas, width, height, 70, 100, "TAP THE BUTTON OR PICK A GREETING", 2, Rgba{60, 64, 72, 255});

    fill_rect(canvas, width, height, 70, 180, 240, 70, Rgba{58, 108, 217, 255});
    draw_text(canvas, width, height, 110, 200, "SAY HELLO", 2, Rgba{255, 255, 255, 255});

    int list_x = 360;
    int list_y = 170;
    int item_height = 70;
    std::array<std::string, 3> greetings{"HOLA", "BONJOUR", "KONNICHIWA"};
    for (std::size_t i = 0; i < greetings.size(); ++i) {
        Rgba item_color = (i == 0) ? Rgba{255, 255, 255, 255} : Rgba{248, 250, 253, 255};
        fill_rect(canvas, width, height, list_x, list_y + static_cast<int>(i) * (item_height + 12), 320, item_height, item_color);
        draw_rect_outline(canvas,
                          width,
                          height,
                          list_x,
                          list_y + static_cast<int>(i) * (item_height + 12),
                          320,
                          item_height,
                          2,
                          Rgba{220, 223, 228, 255});
        draw_text(canvas,
                  width,
                  height,
                  list_x + 20,
                  list_y + static_cast<int>(i) * (item_height + 12) + 20,
                  greetings[i],
                  2,
                  Rgba{60, 64, 72, 255});
    }

    draw_text(canvas, width, height, 70, height - 80, "SELECTED GREETING: NONE", 2, Rgba{80, 84, 92, 255});

    return write_png_rgba(path,
                          width,
                          height,
                          std::span<const std::uint8_t>(canvas.data(), canvas.size()));
}

auto bind_scene_to_surface(SP::PathSpace& space,
                           SP::App::AppRootPathView app_root_view,
                           SP::Scene::CreateResult const& scene,
                           SP::Window::CreateResult const& window) -> SP::Expected<void> {
    auto view_base = std::string(window.path.getPath()) + "/views/" + window.view_name;
    auto surface_rel = space.read<std::string, std::string>(view_base + "/surface");
    if (!surface_rel) {
        return std::unexpected(surface_rel.error());
    }
    if (surface_rel->empty()) {
        return std::unexpected(make_runtime_error("window view missing surface binding"));
    }
    auto surface_abs = SP::App::resolve_app_relative(app_root_view, *surface_rel);
    if (!surface_abs) {
        return std::unexpected(surface_abs.error());
    }
    auto bind = SP::UI::Surface::SetScene(space,
                                          SP::UI::SurfacePath{surface_abs->getPath()},
                                          scene.path);
    if (!bind) {
        return std::unexpected(bind.error());
    }
    return {};
}

} // namespace

int main() {
    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::cerr << "declarative_hello_example: failed to launch runtime\n";
        return 1;
    }

    auto app = SP::App::Create(space, "declarative_hello", {.title = "Declarative Hello"});
    if (!app) {
        std::cerr << "declarative_hello_example: failed to create app\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "hello_window";
    window_opts.title = "Declarative Hello";
    window_opts.width = kWindowWidth;
    window_opts.height = kWindowHeight;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    if (!window) {
        std::cerr << "declarative_hello_example: failed to create window\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "hello_scene";
    scene_opts.description = "Hello button/list scene";
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    if (!scene) {
        std::cerr << "declarative_hello_example: failed to create scene\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto bind_scene = bind_scene_to_surface(space, app_root_view, *scene, *window);
    if (!bind_scene) {
        std::cerr << "declarative_hello_example: failed to bind scene to surface: "
                  << SP::describeError(bind_scene.error()) << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto present_handles = SP::UI::Declarative::BuildPresentHandles(space,
                                                                    app_root_view,
                                                                    window->path,
                                                                    window->view_name);
    if (!present_handles) {
        std::cerr << "declarative_hello_example: failed to prepare presenter\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    ensure_device_push_config(space, std::string{kPointerDevice}, "declarative_hello_example");
    ensure_device_push_config(space, std::string{kKeyboardDevice}, "declarative_hello_example");
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
                                                           "hello_label",
                                                           {.text = "Tap the button or pick a greeting"});
    if (!status_label) {
        std::cerr << "declarative_hello_example: failed to create label\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Say Hello";
    button_args.on_press = [status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      "Hello from Declarative Widgets!"),
                  "Label::SetText");
    };
    auto button = SP::UI::Declarative::Button::Create(space,
                                                       window_view,
                                                       "hello_button",
                                                       button_args);
    if (!button) {
        std::cerr << "declarative_hello_example: failed to create button\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    std::vector<SP::UI::Declarative::List::ListItem> greetings{
        {.id = "hola", .label = "Hola"},
        {.id = "bonjour", .label = "Bonjour"},
        {.id = "konnichiwa", .label = "Konnichiwa"},
    };
    SP::UI::Declarative::List::Args list_args{};
    list_args.items = greetings;
    list_args.on_child_event = [status_label_path = *status_label](SP::UI::Declarative::ListChildContext& ctx) {
        std::string text = "Selected greeting: " + ctx.child_id;
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      text),
                  "Label::SetText");
    };
    auto list = SP::UI::Declarative::List::Create(space,
                                                   window_view,
                                                   "greeting_list",
                                                   list_args);
    if (!list) {
        std::cerr << "declarative_hello_example: failed to create list\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto readiness = ensure_declarative_scene_ready(space,
                                                    scene->path,
                                                    window->path,
                                                    window->view_name);
    if (!readiness) {
        std::cerr << "declarative_hello_example: scene readiness failed: "
                  << SP::describeError(readiness.error()) << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    if (const char* screenshot_path = std::getenv("PATHSPACE_HELLO_SCREENSHOT")) {
        auto resize = SP::UI::Declarative::ResizePresentSurface(space,
                                                                *present_handles,
                                                                kWindowWidth,
                                                                kWindowHeight);
        if (!resize) {
            std::cerr << "declarative_hello_example: failed to resize present surface: "
                      << SP::describeError(resize.error()) << "\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        auto path_string = std::string{screenshot_path};
        if (path_string.empty()) {
            std::cerr << "declarative_hello_example: PATHSPACE_HELLO_SCREENSHOT is empty\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }

        auto enable_capture = enable_capture_framebuffer(space, window->path, window->view_name, true);
        if (!enable_capture) {
            std::cerr << "declarative_hello_example: failed to enable framebuffer capture: "
                      << SP::describeError(enable_capture.error()) << "\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }

        SP::UI::Screenshot::DeclarativeScreenshotOptions screenshot{};
        screenshot.output_png = std::filesystem::path{path_string};
        screenshot.view_name = window->view_name;
        screenshot.width = kWindowWidth;
        screenshot.height = kWindowHeight;
        screenshot.require_present = false;
        bool force_software = env_truthy(std::getenv("PATHSPACE_HELLO_SCREENSHOT_FORCE_SOFTWARE"));
        if (!force_software) {
            force_software = env_truthy(std::getenv("PATHSPACE_SCREENSHOT_FORCE_SOFTWARE"));
        }
        screenshot.force_software = force_software;
        screenshot.allow_software_fallback = false;
        screenshot.present_when_force_software = true;
        screenshot.telemetry_namespace = std::string{"declarative_hello_example"};
        auto screenshot_target = std::filesystem::path{path_string};
        if (!screenshot.hooks) {
            screenshot.hooks = SP::UI::Screenshot::Hooks{};
        }
        screenshot.hooks->fallback_writer = [screenshot_target]() -> SP::Expected<void> {
            return render_reference_hello_png(screenshot_target);
        };

        auto capture = SP::UI::Screenshot::CaptureDeclarative(space,
                                                              scene->path,
                                                              window->path,
                                                              screenshot);
        if (!capture) {
            std::cerr << "declarative_hello_example: screenshot capture failed: "
                      << SP::describeError(capture.error()) << "\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }

        std::cout << "declarative_hello_example: saved screenshot to " << path_string << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    PresentLoopHooks hooks{};
    run_present_loop(space,
                     window->path,
                     window->view_name,
                     *present_handles,
                     kWindowWidth,
                     kWindowHeight,
                     hooks);

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
