#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(PATHSPACE_ENABLE_UI)
int main() {
    std::cerr << "pixel_noise_example requires PATHSPACE_ENABLE_UI=ON.\n";
    return 1;
}
#elif !defined(__APPLE__)
int main() {
    std::cerr << "pixel_noise_example currently supports only macOS builds.\n";
    return 1;
}
#else

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <pathspace/ui/LocalWindowBridge.hpp>

using namespace SP;
using namespace SP::UI;
namespace Builders = SP::UI::Builders;
namespace UIScene = SP::UI::Scene;

namespace {

struct Options {
    int width = 1280;
    int height = 720;
    bool headless = false;
    bool capture_framebuffer = false;
    bool report_metrics = false;
    bool report_present_call_time = false;
    double present_refresh_hz = 60.0;
    std::size_t max_frames = 0;
    std::chrono::duration<double> report_interval = std::chrono::seconds{1};
    std::uint64_t seed = 0;
    std::optional<std::chrono::duration<double>> runtime_limit{};
};

struct NoiseState {
    explicit NoiseState(std::uint64_t seed_value)
        : rng(static_cast<std::mt19937::result_type>(seed_value))
        , channel_dist(0, 255)
        , frame_index(0)
    {}

    std::mt19937 rng;
    std::uniform_int_distribution<int> channel_dist;
    std::uint64_t frame_index;
};

std::atomic<bool> g_running{true};

void handle_signal(int) {
    g_running.store(false, std::memory_order_release);
}

template <typename T>
auto expect_or_exit(SP::Expected<T> value, char const* context) -> T {
    if (value) {
        return std::move(*value);
    }
    auto const& err = value.error();
    std::cerr << "pixel_noise_example: " << context << " failed";
    if (err.message.has_value()) {
        std::cerr << ": " << *err.message;
    } else {
        std::cerr << " (code " << static_cast<int>(err.code) << ')';
    }
    std::cerr << '\n';
    std::exit(1);
}

template <typename T>
void replace_value_or_exit(PathSpace& space,
                           std::string const& path,
                           T const& value,
                           char const* context) {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        std::cerr << "pixel_noise_example: " << context << " failed to clear old value";
        if (error.message.has_value()) {
            std::cerr << ": " << *error.message;
        }
        std::cerr << '\n';
        std::exit(1);
    }

    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        auto const& err = inserted.errors.front();
        std::cerr << "pixel_noise_example: " << context << " insert failed";
        if (err.message.has_value()) {
            std::cerr << ": " << *err.message;
        }
        std::cerr << '\n';
        std::exit(1);
    }
}

inline void expect_or_exit(SP::Expected<void> value, char const* context) {
    if (value) {
        return;
    }
    auto const& err = value.error();
    std::cerr << "pixel_noise_example: " << context << " failed";
    if (err.message.has_value()) {
        std::cerr << ": " << *err.message;
    } else {
        std::cerr << " (code " << static_cast<int>(err.code) << ')';
    }
    std::cerr << '\n';
    std::exit(1);
}

auto parse_int(std::string_view text, char const* label) -> int {
    try {
        size_t consumed = 0;
        int value = std::stoi(std::string{text}, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument{"trailing characters"};
        }
        return value;
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_size(std::string_view text, char const* label) -> std::size_t {
    try {
        size_t consumed = 0;
        unsigned long long value = std::stoull(std::string{text}, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument{"trailing characters"};
        }
        return static_cast<std::size_t>(value);
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_seconds(std::string_view text, char const* label) -> std::chrono::duration<double> {
    try {
        size_t consumed = 0;
        double value = std::stod(std::string{text}, &consumed);
        if (consumed != text.size() || value <= 0.0) {
            throw std::invalid_argument{"expected positive number"};
        }
        return std::chrono::duration<double>(value);
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_seed(std::string_view text) -> std::uint64_t {
    try {
        size_t consumed = 0;
        unsigned long long value = std::stoull(std::string{text}, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument{"trailing characters"};
        }
        return static_cast<std::uint64_t>(value);
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid seed '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_minutes(std::string_view text, char const* label) -> std::chrono::duration<double> {
    auto seconds = parse_seconds(text, label);
    return std::chrono::duration<double>(seconds.count() * 60.0);
}

auto parse_positive_double(std::string_view text, char const* label) -> double {
    try {
        size_t consumed = 0;
        double value = std::stod(std::string{text}, &consumed);
        if (consumed != text.size() || value < 0.0) {
            throw std::invalid_argument{"expected non-negative number"};
        }
        return value;
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_options(int argc, char** argv) -> Options {
    Options opts{};
    opts.seed = static_cast<std::uint64_t>(std::random_device{}());

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--windowed") {
            opts.headless = false;
        } else if (arg.rfind("--width=", 0) == 0) {
            opts.width = parse_int(arg.substr(8), "width");
        } else if (arg.rfind("--height=", 0) == 0) {
            opts.height = parse_int(arg.substr(9), "height");
        } else if (arg.rfind("--frames=", 0) == 0) {
            opts.max_frames = parse_size(arg.substr(9), "frames");
        } else if (arg.rfind("--report-interval=", 0) == 0) {
            opts.report_interval = parse_seconds(arg.substr(18), "report interval");
        } else if (arg.rfind("--seed=", 0) == 0) {
            opts.seed = parse_seed(arg.substr(7));
        } else if (arg.rfind("--present-refresh=", 0) == 0) {
            opts.present_refresh_hz = parse_positive_double(arg.substr(18), "present refresh");
        } else if (arg == "--capture-framebuffer") {
            opts.capture_framebuffer = true;
        } else if (arg == "--report-metrics") {
            opts.report_metrics = true;
        } else if (arg == "--report-extended") {
            opts.report_metrics = true;
            opts.report_present_call_time = true;
        } else if (arg == "--present-call-metric") {
            opts.report_present_call_time = true;
        } else if (arg.rfind("--runtime-minutes=", 0) == 0) {
            opts.runtime_limit = parse_minutes(arg.substr(18), "runtime minutes");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: pixel_noise_example [options]\n"
                      << "Options:\n"
                      << "  --width=<pixels>          Surface width (default 1280)\n"
                      << "  --height=<pixels>         Surface height (default 720)\n"
                      << "  --frames=<count>          Stop after N presented frames\n"
                      << "  --report-interval=<sec>   Stats print interval (default 1.0)\n"
                      << "  --present-refresh=<hz>    Limit window presents to this rate (default 60, 0=every frame)\n"
                      << "  --report-metrics          Print FPS/render metrics every interval\n"
                      << "  --report-extended         Metrics plus Window::Present call timing\n"
                      << "  --present-call-metric     Track Window::Present duration (pairs well with --report-metrics)\n"
                      << "  --runtime-minutes=<min>   Stop after the given number of minutes\n"
                      << "  --headless                Skip local window presentation\n"
                      << "  --windowed                Show the local window while computing frames (default)\n"
                      << "  --capture-framebuffer     Enable framebuffer capture in the present policy\n"
                      << "  --seed=<value>            PRNG seed\n";
            std::exit(0);
        } else {
            std::cerr << "pixel_noise_example: unknown option '" << arg << "'\n";
            std::cerr << "Use --help to see available options.\n";
            std::exit(1);
        }
    }

    if (opts.width <= 0 || opts.height <= 0) {
        std::cerr << "pixel_noise_example: width and height must be positive.\n";
        std::exit(1);
    }

    return opts;
}

auto make_identity_transform() -> UIScene::Transform {
    UIScene::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto build_background_bucket(int width, int height) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    constexpr std::uint64_t kDrawableId = 0xC0FFEE01u;

    bucket.drawable_ids = {kDrawableId};
    bucket.world_transforms = {make_identity_transform()};

    UIScene::BoundingSphere sphere{};
    sphere.center = {static_cast<float>(width) * 0.5f,
                     static_cast<float>(height) * 0.5f,
                     0.0f};
    sphere.radius = std::sqrt(sphere.center[0] * sphere.center[0]
                              + sphere.center[1] * sphere.center[1]);
    bucket.bounds_spheres = {sphere};

    UIScene::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {static_cast<float>(width),
               static_cast<float>(height),
               0.0f};
    bucket.bounds_boxes = {box};
    bucket.bounds_box_valid = {1};

    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        UIScene::DrawableAuthoringMapEntry{
            kDrawableId,
            "pixel_noise/background",
            0,
            0,
        }
    };
    bucket.drawable_fingerprints = {kDrawableId};

    UIScene::RectCommand rect{};
    rect.min_x = 0.0f;
    rect.min_y = 0.0f;
    rect.max_x = static_cast<float>(width);
    rect.max_y = static_cast<float>(height);
    rect.color = {0.0f, 0.0f, 0.0f, 1.0f};

    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data() + offset,
                &rect,
                sizeof(UIScene::RectCommand));
    bucket.command_kinds = {
        static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect),
    };

    return bucket;
}

struct SceneSetup {
    Builders::ScenePath scene;
    std::uint64_t revision = 0;
};

auto publish_scene(PathSpace& space,
                   SP::App::AppRootPathView root,
                   int width,
                   int height) -> SceneSetup {
    Builders::SceneParams scene_params{};
    scene_params.name = "pixel_noise_scene";
    scene_params.description = "Pixel noise perf harness scene";

    auto scene_path = expect_or_exit(Builders::Scene::Create(space, root, scene_params),
                                     "create scene");

    UIScene::SceneSnapshotBuilder builder{space, root, scene_path};
    auto bucket = build_background_bucket(width, height);

    UIScene::SnapshotPublishOptions publish{};
    publish.metadata.author = "pixel_noise_example";
    publish.metadata.tool_version = "pixel_noise_example";
    publish.metadata.created_at = std::chrono::system_clock::now();
    publish.metadata.drawable_count = bucket.drawable_ids.size();
    publish.metadata.command_count = bucket.command_counts.size();

    auto revision = expect_or_exit(builder.publish(publish, bucket),
                                   "publish scene snapshot");

    return SceneSetup{
        .scene = scene_path,
        .revision = revision,
    };
}

void present_to_local_window(Builders::Window::WindowPresentResult const& present,
                             int width,
                             int height,
                             bool headless) {
    if (headless) {
        return;
    }

    bool presented = false;

    if (present.stats.iosurface && present.stats.iosurface->valid()) {
        auto iosurface_ref = present.stats.iosurface->retain_for_external_use();
        if (iosurface_ref) {
            SP::UI::PresentLocalWindowIOSurface(static_cast<void*>(iosurface_ref),
                                                width,
                                                height,
                                                static_cast<int>(present.stats.iosurface->row_bytes()));
            presented = true;
            CFRelease(iosurface_ref);
        }
    }

    if (!presented) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::cerr << "pixel_noise_example: IOSurface unavailable; "
                         "skipping presentation to avoid CPU blit.\n";
        }
    }
}

struct HookGuard {
    HookGuard() = default;
    HookGuard(HookGuard const&) = delete;
    HookGuard& operator=(HookGuard const&) = delete;
    HookGuard(HookGuard&& other) noexcept
        : active(other.active) {
        other.active = false;
    }
    HookGuard& operator=(HookGuard&& other) noexcept {
        if (this != &other) {
            if (active) {
                Builders::Window::TestHooks::ResetBeforePresentHook();
            }
            active = other.active;
            other.active = false;
        }
        return *this;
    }
    ~HookGuard() {
        if (active) {
            Builders::Window::TestHooks::ResetBeforePresentHook();
        }
    }

private:
    bool active = true;
};

auto install_noise_hook(std::shared_ptr<NoiseState> state) -> HookGuard {
    Builders::Window::TestHooks::SetBeforePresentHook(
        [state = std::move(state)](PathSurfaceSoftware& surface,
                                   PathWindowView::PresentPolicy& /*policy*/,
                                   std::vector<std::size_t>& dirty_tiles) mutable {
            auto desc = surface.desc();
            auto width = std::max(0, desc.size_px.width);
            auto height = std::max(0, desc.size_px.height);
            if (width == 0 || height == 0) {
                return;
            }

            auto buffer = surface.staging_span();
            auto stride = surface.row_stride_bytes();
            if (buffer.size() < static_cast<std::size_t>(height) * stride
                || stride == 0) {
                return;
            }

            auto const start = std::chrono::steady_clock::now();
            auto worker_count = std::max<int>(1, static_cast<int>(std::thread::hardware_concurrency()));
            worker_count = std::min(worker_count, std::max(1, height));

            std::vector<std::uint64_t> seeds(static_cast<std::size_t>(worker_count));
            for (int i = 0; i < worker_count; ++i) {
                auto hi = static_cast<std::uint64_t>(state->rng());
                auto lo = static_cast<std::uint64_t>(state->rng());
                seeds[static_cast<std::size_t>(i)] = (hi << 32)
                                                     ^ lo
                                                     ^ (static_cast<std::uint64_t>(state->frame_index + 1) << 17)
                                                     ^ static_cast<std::uint64_t>(i);
            }

            auto rows_per_worker = (height + worker_count - 1) / worker_count;
            std::vector<std::thread> workers;
            workers.reserve(static_cast<std::size_t>(worker_count));

            for (int worker = 0; worker < worker_count; ++worker) {
                int row_begin = worker * rows_per_worker;
                int row_end = std::min(height, row_begin + rows_per_worker);
                if (row_begin >= row_end) {
                    break;
                }
                workers.emplace_back([row_begin,
                                      row_end,
                                      width,
                                      stride,
                                      buffer_data = buffer.data(),
                                      seed = seeds[static_cast<std::size_t>(worker)]]() {
                    std::uniform_int_distribution<int> dist(0, 255);
                    std::seed_seq seq{
                        static_cast<std::uint32_t>(seed & 0xFFFFFFFFu),
                        static_cast<std::uint32_t>((seed >> 32) & 0xFFFFFFFFu)};
                    std::mt19937 rng(seq);
                    for (int y = row_begin; y < row_end; ++y) {
                        auto* row = buffer_data + static_cast<std::size_t>(y) * stride;
                        for (int x = 0; x < width; ++x) {
                            auto channel0 = static_cast<std::uint32_t>(dist(rng));
                            auto channel1 = static_cast<std::uint32_t>(dist(rng));
                            auto channel2 = static_cast<std::uint32_t>(dist(rng));
                            std::uint32_t noise = channel0
                                                  | (channel1 << 8)
                                                  | (channel2 << 16)
                                                  | 0xFF000000u;
                            std::memcpy(row + static_cast<std::size_t>(x) * 4u, &noise, sizeof(noise));
                        }
                    }
                });
            }

            for (auto& worker : workers) {
                worker.join();
            }

            auto const finish = std::chrono::steady_clock::now();
            auto render_ms = std::chrono::duration<double, std::milli>(finish - start).count();

            ++state->frame_index;
            PathSurfaceSoftware::FrameInfo info{};
            info.frame_index = state->frame_index;
            info.revision = state->frame_index;
            info.render_ms = render_ms;
            surface.publish_buffered_frame(info);

            dirty_tiles.clear();
        });

    return HookGuard{};
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/pixel_noise_example"};
    SP::App::AppRootPathView app_root_view{app_root.getPath()};

    auto scene_setup = publish_scene(space, app_root_view, options.width, options.height);

    Builders::App::BootstrapParams bootstrap_params{};
    bootstrap_params.renderer.name = "noise_renderer";
    bootstrap_params.renderer.kind = Builders::RendererKind::Software2D;
    bootstrap_params.renderer.description = "pixel noise renderer";

    bootstrap_params.surface.name = "noise_surface";
    bootstrap_params.surface.desc.size_px.width = options.width;
    bootstrap_params.surface.desc.size_px.height = options.height;
    bootstrap_params.surface.desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    bootstrap_params.surface.desc.color_space = Builders::ColorSpace::sRGB;
    bootstrap_params.surface.desc.premultiplied_alpha = true;

    bootstrap_params.window.name = "noise_window";
    bootstrap_params.window.title = "PathSpace Pixel Noise";
    bootstrap_params.window.width = options.width;
    bootstrap_params.window.height = options.height;
    bootstrap_params.window.scale = 1.0f;
    bootstrap_params.window.background = "#101218";

    bootstrap_params.present_policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    bootstrap_params.present_policy.capture_framebuffer = options.capture_framebuffer;
    bootstrap_params.present_policy.auto_render_on_present = true;
    bootstrap_params.present_policy.vsync_align = false;

    auto bootstrap = expect_or_exit(Builders::App::Bootstrap(space,
                                                             app_root_view,
                                                             scene_setup.scene,
                                                             bootstrap_params),
                                    "bootstrap application");

    expect_or_exit(Builders::Surface::SetScene(space, bootstrap.surface, scene_setup.scene),
                   "bind scene to surface");

    auto noise_state = std::make_shared<NoiseState>(options.seed);
    auto hook_guard = install_noise_hook(noise_state);

    auto target_field = std::string(bootstrap.surface.getPath()) + "/target";
    auto target_relative = expect_or_exit(space.read<std::string, std::string>(target_field),
                                          "read surface target");
    auto target_absolute = expect_or_exit(SP::App::resolve_app_relative(app_root_view, target_relative),
                                          "resolve surface target");

    std::string surface_desc_path = std::string(bootstrap.surface.getPath()) + "/desc";
    std::string target_desc_path = std::string(target_absolute.getPath()) + "/desc";

    auto surface_desc = expect_or_exit(space.read<Builders::SurfaceDesc, std::string>(surface_desc_path),
                                       "read surface desc");
    int current_surface_width = surface_desc.size_px.width;
    int current_surface_height = surface_desc.size_px.height;

    if (!options.headless) {
        SP::UI::SetLocalWindowCallbacks({});
        SP::UI::InitLocalWindowWithSize(options.width,
                                        options.height,
                                        "PathSpace Pixel Noise");
    }

    std::cout << "pixel_noise_example: width=" << options.width
              << " height=" << options.height
              << " seed=" << options.seed
              << (options.headless ? " headless" : " windowed")
              << (options.capture_framebuffer ? " capture" : "")
              << (options.report_metrics ? " metrics" : "")
              << (options.report_present_call_time ? " present-call" : "")
              << '\n';

    auto now = std::chrono::steady_clock::now();
    auto last_report = now;
    auto start_time = now;
    auto present_interval = options.present_refresh_hz > 0.0
                                ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(1.0 / options.present_refresh_hz))
                                : std::chrono::steady_clock::duration::zero();
    auto last_window_present = start_time;
    std::size_t frames_since_report = 0;
    double accumulated_present_ms = 0.0;
    double accumulated_render_ms = 0.0;
    double interval_present_call_ms = 0.0;
    double total_present_call_ms = 0.0;
    std::size_t interval_present_call_samples = 0;
    std::size_t total_present_call_samples = 0;
    std::size_t total_presented = 0;

    while (g_running.load(std::memory_order_acquire)) {
        if (options.max_frames != 0 && total_presented >= options.max_frames) {
            break;
        }

        if (!options.headless) {
            SP::UI::PollLocalWindow();
            int window_width = 0;
            int window_height = 0;
            SP::UI::GetLocalWindowContentSize(&window_width, &window_height);
            if (window_width <= 0 || window_height <= 0) {
                std::cout << "pixel_noise_example: window closed, exiting loop.\n";
                break;
            }

            if ((window_width != current_surface_width || window_height != current_surface_height)) {
                surface_desc.size_px.width = window_width;
                surface_desc.size_px.height = window_height;
                replace_value_or_exit(space,
                                      surface_desc_path,
                                      surface_desc,
                                      "update surface desc");
                replace_value_or_exit(space,
                                      target_desc_path,
                                      surface_desc,
                                      "update target desc");
                current_surface_width = window_width;
                current_surface_height = window_height;
                options.width = window_width;
                options.height = window_height;
            }
        }

        if (options.runtime_limit) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed >= *options.runtime_limit) {
                std::cout << "pixel_noise_example: runtime limit reached ("
                          << std::chrono::duration_cast<std::chrono::seconds>(*options.runtime_limit).count()
                          << " seconds), exiting loop.\n";
                break;
            }
        }

        std::chrono::steady_clock::time_point present_call_start{};
        if (options.report_present_call_time) {
            present_call_start = std::chrono::steady_clock::now();
        }
        auto present = Builders::Window::Present(space,
                                                 bootstrap.window,
                                                 bootstrap.view_name);
        if (options.report_present_call_time) {
            auto present_call_finish = std::chrono::steady_clock::now();
            double call_ms = std::chrono::duration<double, std::milli>(present_call_finish - present_call_start).count();
            total_present_call_ms += call_ms;
            ++total_present_call_samples;
            if (options.report_metrics) {
                interval_present_call_ms += call_ms;
                ++interval_present_call_samples;
            }
        }
        if (!present) {
            auto const& err = present.error();
            std::cerr << "pixel_noise_example: present failed";
            if (err.message.has_value()) {
                std::cerr << ": " << *err.message;
            } else {
                std::cerr << " (code " << static_cast<int>(err.code) << ')';
            }
            std::cerr << '\n';
            break;
        }

        if (!options.headless) {
            auto current_time = std::chrono::steady_clock::now();
            bool should_present_window = options.present_refresh_hz <= 0.0
                || (present_interval.count() == 0)
                || ((current_time - last_window_present) >= present_interval);
            if (should_present_window) {
                present_to_local_window(*present,
                                        options.width,
                                        options.height,
                                        false);
                last_window_present = current_time;
            }
        }

        if (present->stats.presented) {
            ++total_presented;
            if (options.report_metrics) {
                ++frames_since_report;
                accumulated_present_ms += present->stats.present_ms;
                accumulated_render_ms += present->stats.frame.render_ms;
            }
        }

        if (options.report_metrics) {
            now = std::chrono::steady_clock::now();
            if (now - last_report >= options.report_interval) {
                double seconds = std::chrono::duration<double>(now - last_report).count();
                double fps = seconds > 0.0 ? static_cast<double>(frames_since_report) / seconds : 0.0;
                double avg_present = frames_since_report > 0
                                         ? accumulated_present_ms / static_cast<double>(frames_since_report)
                                         : 0.0;
                double avg_render = frames_since_report > 0
                                        ? accumulated_render_ms / static_cast<double>(frames_since_report)
                                        : 0.0;

                std::cout << std::fixed << std::setprecision(2)
                          << "[pixel_noise_example] "
                          << "frames=" << total_presented
                          << " fps=" << fps
                          << " avgPresentMs=" << avg_present
                          << " avgRenderMs=" << avg_render;
                if (options.report_present_call_time && interval_present_call_samples > 0) {
                    double avg_call = interval_present_call_ms / static_cast<double>(interval_present_call_samples);
                    std::cout << " avgPresentCallMs=" << avg_call;
                }
                std::cout << " lastFrameIndex=" << present->stats.frame.frame_index
                          << " lastPresentMs=" << present->stats.present_ms
                          << " lastRenderMs=" << present->stats.frame.render_ms
                          << '\n';
                std::cout.unsetf(std::ios::floatfield);

                frames_since_report = 0;
                accumulated_present_ms = 0.0;
                accumulated_render_ms = 0.0;
                interval_present_call_ms = 0.0;
                interval_present_call_samples = 0;
                last_report = now;
            }
        }
    }

    std::cout << "pixel_noise_example: presented " << total_presented << " frames.\n";
    if (options.report_present_call_time && total_present_call_samples > 0) {
        std::cout << std::fixed << std::setprecision(3)
                  << "pixel_noise_example: avgPresentCallMs="
                  << (total_present_call_ms / static_cast<double>(total_present_call_samples))
                  << " over " << total_present_call_samples << " samples\n";
        std::cout.unsetf(std::ios::floatfield);
    }
    return 0;
}

#endif // PATHSPACE_ENABLE_UI / __APPLE__
