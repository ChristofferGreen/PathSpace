#include <pathspace/system/Standard.hpp>

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/LegacyBuildersDeprecation.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/PaintSurfaceUploader.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
namespace Declarative = SP::UI::Declarative;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
using SP::UI::Scene::DrawableBucketSnapshot;
using SP::PathSpace;
using WidgetPath = SP::UI::Builders::WidgetPath;

struct CommandLineOptions {
    int iterations = 200;
    std::uint32_t seed = 1337;
    bool verbose = false;
    std::string output_path;
};

struct SampleData {
    std::string button_label;
    BuilderWidgets::ButtonStyle button_style{};
    BuilderWidgets::ButtonState button_state{};

    BuilderWidgets::ToggleStyle toggle_style{};
    BuilderWidgets::ToggleState toggle_state{};

    BuilderWidgets::SliderStyle slider_style{};
    BuilderWidgets::SliderState slider_state{};
    BuilderWidgets::SliderRange slider_range{};

    BuilderWidgets::ListStyle list_style{};
    std::vector<BuilderWidgets::ListItem> list_items;
};

struct DeclarativeMetrics {
    double mutate_total_ms = 0.0;
    double bucket_total_ms = 0.0;
    double dirty_per_sec = 0.0;
    double bucket_avg_ms = 0.0;
    double bucket_bytes_per_iter = 0.0;
    double paint_gpu_last_upload_ns = 0.0;
};

struct DeclarativePaths {
    WidgetPath button;
    WidgetPath toggle;
    WidgetPath slider;
    WidgetPath list;
    WidgetPath paint;
    SP::UI::Builders::ScenePath scene;
};

constexpr std::string_view kPaintUploaderMetrics = "/system/widgets/runtime/paint_gpu/metrics";

[[nodiscard]] auto parse_int(std::string_view arg) -> std::optional<int> {
    int value = 0;
    auto result = std::from_chars(arg.data(), arg.data() + arg.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] auto parse_uint(std::string_view arg) -> std::optional<std::uint32_t> {
    std::uint32_t value = 0;
    auto result = std::from_chars(arg.data(), arg.data() + arg.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] auto parse_args(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string_view current{argv[i]};
        if (current == "--verbose") {
            options.verbose = true;
            continue;
        }
        auto eq_pos = current.find('=');
        std::string_view key = current.substr(0, eq_pos);
        std::string_view value = eq_pos == std::string_view::npos ? std::string_view{} : current.substr(eq_pos + 1);
        if (key == "--iterations" && !value.empty()) {
            if (auto parsed = parse_int(value); parsed && *parsed > 0) {
                options.iterations = *parsed;
            }
            continue;
        }
        if (key == "--seed" && !value.empty()) {
            if (auto parsed = parse_uint(value)) {
                options.seed = *parsed;
            }
            continue;
        }
        if (key == "--write-json" && !value.empty()) {
            options.output_path.assign(value.begin(), value.end());
            continue;
        }
        std::cerr << "widget_pipeline_benchmark: unknown argument '" << current << "'\n";
        std::exit(2);
    }
    return options;
}

[[nodiscard]] auto format_suffix(std::string_view prefix, int index) -> std::string {
    char buffer[64];
    auto prefix_string = std::string(prefix);
    std::snprintf(buffer, sizeof(buffer), "%s_%02d", prefix_string.c_str(), index);
    return std::string(buffer);
}

auto make_sample_data() -> SampleData {
    SampleData data;
    data.button_label = "Bench Button";
    data.toggle_state.checked = false;
    data.slider_range.minimum = 0.0f;
    data.slider_range.maximum = 100.0f;
    data.slider_state.value = 35.0f;
    data.slider_style.width = 320.0f;
    data.list_style.width = 320.0f;
    data.list_style.item_height = 32.0f;
    data.list_style.corner_radius = 6.0f;
    for (int i = 0; i < 16; ++i) {
        BuilderWidgets::ListItem item{};
        item.id = format_suffix("item", i);
        item.label = "Item " + std::to_string(i);
        item.enabled = (i % 3) != 0;
        data.list_items.push_back(item);
    }
    return data;
}

[[nodiscard]] auto bucket_bytes(DrawableBucketSnapshot const& bucket) -> std::size_t {
    auto sum_vector = [](auto const& vec) {
        using Value = typename std::decay_t<decltype(vec)>::value_type;
        return vec.size() * sizeof(Value);
    };

    std::size_t total = 0;
    total += sum_vector(bucket.drawable_ids);
    total += sum_vector(bucket.world_transforms);
    total += sum_vector(bucket.bounds_spheres);
    total += sum_vector(bucket.bounds_boxes);
    total += sum_vector(bucket.bounds_box_valid);
    total += sum_vector(bucket.layers);
    total += sum_vector(bucket.z_values);
    total += sum_vector(bucket.material_ids);
    total += sum_vector(bucket.pipeline_flags);
    total += sum_vector(bucket.visibility);
    total += sum_vector(bucket.command_offsets);
    total += sum_vector(bucket.command_counts);
    total += sum_vector(bucket.opaque_indices);
    total += sum_vector(bucket.alpha_indices);
    total += sum_vector(bucket.command_kinds);
    total += sum_vector(bucket.command_payload);
    total += sum_vector(bucket.stroke_points);
    total += sum_vector(bucket.clip_nodes);
    total += sum_vector(bucket.clip_head_indices);
    total += sum_vector(bucket.drawable_fingerprints);
    total += sum_vector(bucket.glyph_vertices);

    for (auto const& entry : bucket.layer_indices) {
        total += sizeof(entry);
        total += entry.indices.size() * sizeof(std::uint32_t);
    }
    for (auto const& entry : bucket.authoring_map) {
        total += sizeof(entry);
        total += entry.authoring_node_id.size();
    }
    for (auto const& entry : bucket.font_assets) {
        total += sizeof(entry);
        total += entry.resource_root.size();
    }
    return total;
}

[[nodiscard]] auto rotate_items(std::vector<BuilderWidgets::ListItem> const& base,
                                 std::size_t offset)
    -> std::vector<BuilderWidgets::ListItem> {
    if (base.empty()) {
        return {};
    }
    std::vector<BuilderWidgets::ListItem> rotated = base;
    offset %= rotated.size();
    std::rotate(rotated.begin(), rotated.begin() + static_cast<long>(offset), rotated.end());
    if (!rotated.empty()) {
        rotated.front().enabled = true;
        rotated.front().label = "Item " + std::to_string(offset);
    }
    return rotated;
}

auto create_app(PathSpace& space) -> SP::Expected<SP::App::AppRootPath> {
    SP::App::CreateOptions opts{};
    opts.title = "Widget Pipeline Benchmark";
    opts.default_theme = "default";
    return SP::App::Create(space, "widget_pipeline_benchmark", opts);
}

[[nodiscard]] auto setup_declarative_scene(PathSpace& space,
                                           SampleData const& sample,
                                           DeclarativePaths& paths)
    -> SP::Expected<std::vector<WidgetPath>> {
    SP::System::LaunchOptions launch{};
    launch.start_input_runtime = false;
    launch.start_io_trellis = false;
    launch.start_io_pump = false;
    launch.start_io_telemetry_control = false;
    launch.start_widget_event_trellis = false;
    launch.start_paint_gpu_uploader = false;
    auto launch_status = SP::System::LaunchStandard(space, launch);
    if (!launch_status) {
        return std::unexpected(launch_status.error());
    }

    auto app_root = create_app(space);
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    auto app_view = SP::App::AppRootPathView{app_root->getPath()};

    SP::Window::CreateOptions window_opts{};
    window_opts.title = "widget_pipeline_window";
    window_opts.name = "widget_pipeline_window";
    window_opts.width = 1280;
    window_opts.height = 720;
    window_opts.visible = false;
    auto window = SP::Window::Create(space, app_view, window_opts);
    if (!window) {
        return std::unexpected(window.error());
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "widget_pipeline_scene";
    scene_opts.description = "widget pipeline benchmark";
    scene_opts.attach_to_window = false;
    auto scene = SP::Scene::Create(space, app_view, window->path, scene_opts);
    if (!scene) {
        return std::unexpected(scene.error());
    }

    auto window_view = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto parent = SP::App::ConcretePathView{window_view};

    Declarative::Button::Args button_args{};
    button_args.label = sample.button_label;
    auto button = Declarative::Button::Create(space, parent, "bench_button", button_args);
    if (!button) {
        return std::unexpected(button.error());
    }

    Declarative::Toggle::Args toggle_args{};
    toggle_args.on_toggle = std::nullopt;
    auto toggle = Declarative::Toggle::Create(space, parent, "bench_toggle", toggle_args);
    if (!toggle) {
        return std::unexpected(toggle.error());
    }

    Declarative::Slider::Args slider_args{};
    slider_args.minimum = sample.slider_range.minimum;
    slider_args.maximum = sample.slider_range.maximum;
    slider_args.value = sample.slider_state.value;
    auto slider = Declarative::Slider::Create(space, parent, "bench_slider", slider_args);
    if (!slider) {
        return std::unexpected(slider.error());
    }

    Declarative::List::Args list_args{};
    list_args.items = sample.list_items;
    list_args.style = sample.list_style;
    auto list = Declarative::List::Create(space, parent, "bench_list", list_args);
    if (!list) {
        return std::unexpected(list.error());
    }

    Declarative::PaintSurface::Args paint_args{};
    paint_args.gpu_enabled = true;
    paint_args.buffer_width = 512;
    paint_args.buffer_height = 512;
    auto paint = Declarative::PaintSurface::Create(space, parent, "bench_paint", paint_args);
    if (!paint) {
        return std::unexpected(paint.error());
    }

    paths.button = *button;
    paths.toggle = *toggle;
    paths.slider = *slider;
    paths.list = *list;
    paths.paint = *paint;
    paths.scene = scene->path;

    std::vector<WidgetPath> widget_order;
    widget_order.push_back(*button);
    widget_order.push_back(*toggle);
    widget_order.push_back(*slider);
    widget_order.push_back(*list);
    widget_order.push_back(*paint);
    return widget_order;
}

[[nodiscard]] auto pointer_for(float x, float y) -> BuilderWidgets::Bindings::PointerInfo {
    auto info = BuilderWidgets::Bindings::PointerInfo::Make(x, y);
    info.WithInside(true).WithPrimary(true).WithLocal(x, y);
    return info;
}

[[nodiscard]] auto paint_actions(std::string const& widget_root,
                                std::uint64_t seed) -> std::vector<BuilderWidgets::Reducers::WidgetAction> {
    std::vector<BuilderWidgets::Reducers::WidgetAction> actions;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(32.0f, 480.0f);
    std::uint64_t sequence = 1;
    for (std::uint64_t stroke = 0; stroke < 4; ++stroke) {
        auto stroke_id = std::string("paint_surface/stroke/") + std::to_string(stroke + 1);
        auto begin_point = pointer_for(dist(rng), dist(rng));
        BuilderWidgets::Reducers::WidgetAction begin{};
        begin.kind = BuilderWidgets::Bindings::WidgetOpKind::PaintStrokeBegin;
        begin.widget_path = widget_root;
        begin.target_id = stroke_id;
        begin.pointer = begin_point;
        begin.sequence = sequence++;
        begin.timestamp_ns = sequence * 1'000;
        actions.push_back(begin);

        for (int step = 0; step < 3; ++step) {
            BuilderWidgets::Reducers::WidgetAction update{};
            update.kind = BuilderWidgets::Bindings::WidgetOpKind::PaintStrokeUpdate;
            update.widget_path = widget_root;
            update.target_id = stroke_id;
            auto point = pointer_for(dist(rng), dist(rng));
            update.pointer = point;
            update.sequence = sequence++;
            update.timestamp_ns = sequence * 1'000;
            actions.push_back(update);
        }

        BuilderWidgets::Reducers::WidgetAction commit{};
        commit.kind = BuilderWidgets::Bindings::WidgetOpKind::PaintStrokeCommit;
        commit.widget_path = widget_root;
        commit.target_id = stroke_id;
        commit.pointer = pointer_for(dist(rng), dist(rng));
        commit.sequence = sequence++;
        commit.timestamp_ns = sequence * 1'000;
        actions.push_back(commit);
    }
    return actions;
}

[[nodiscard]] auto apply_paint_strokes(PathSpace& space,
                                       WidgetPath const& paint,
                                       std::uint32_t seed) -> SP::Expected<void> {
    auto actions = paint_actions(paint.getPath(), seed);
    for (auto const& action : actions) {
        auto handled = Declarative::PaintRuntime::HandleAction(space, action);
        if (!handled) {
            return std::unexpected(handled.error());
        }
    }
    return {};
}

[[nodiscard]] auto read_last_upload(PathSpace& space) -> double {
    auto path = std::string(kPaintUploaderMetrics) + "/last_upload_ns";
    auto stored = space.read<std::uint64_t, std::string>(path);
    if (!stored) {
        return 0.0;
    }
    return static_cast<double>(*stored);
}

[[nodiscard]] auto run_paint_gpu_cycle(PathSpace& space,
                                       WidgetPath const& paint,
                                       std::uint32_t seed)
    -> std::optional<double> {
    Declarative::PaintSurfaceUploaderOptions uploader_opts{};
    uploader_opts.poll_interval = std::chrono::milliseconds{5};
    auto started = Declarative::CreatePaintSurfaceUploader(space, uploader_opts);
    if (!started) {
        return std::nullopt;
    }

    struct UploaderGuard {
        PathSpace* target = nullptr;
        ~UploaderGuard() {
            if (target != nullptr) {
                Declarative::ShutdownPaintSurfaceUploader(*target);
            }
        }
    } guard{&space};

    auto strokes = apply_paint_strokes(space, paint, seed);
    if (!strokes) {
        return std::nullopt;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    return read_last_upload(space);
}

[[nodiscard]] auto mutate_widgets(PathSpace& space,
                                  DeclarativePaths const& paths,
                                  SampleData const& sample,
                                  int iteration)
    -> SP::Expected<std::size_t> {
    std::size_t operations = 0;

    auto label = std::string("Bench ") + std::to_string(iteration % 100);
    if (auto status = Declarative::Button::SetLabel(space, paths.button, label); !status) {
        return std::unexpected(status.error());
    }
    ++operations;

    auto checked = (iteration % 3) == 0;
    if (auto status = Declarative::Toggle::SetChecked(space, paths.toggle, checked); !status) {
        return std::unexpected(status.error());
    }
    ++operations;

    auto phase = static_cast<float>((iteration * 7) % 100) / 100.0f;
    auto slider_value = sample.slider_range.minimum
                         + (sample.slider_range.maximum - sample.slider_range.minimum) * phase;
    if (auto status = Declarative::Slider::SetValue(space, paths.slider, slider_value); !status) {
        return std::unexpected(status.error());
    }
    ++operations;

    auto list_items = rotate_items(sample.list_items, static_cast<std::size_t>(iteration));
    if (auto status = Declarative::List::SetItems(space, paths.list, std::move(list_items)); !status) {
        return std::unexpected(status.error());
    }
    ++operations;

    return operations;
}

[[nodiscard]] auto run_declarative_pipeline(PathSpace& space,
                                            SampleData const& sample,
                                            std::vector<WidgetPath> const& widget_order,
                                            DeclarativePaths const& paths,
                                            CommandLineOptions const& options)
    -> SP::Expected<DeclarativeMetrics> {
    DeclarativeMetrics metrics{};
    std::size_t total_dirty_ops = 0;
    std::size_t total_bucket_bytes = 0;
    auto mutate_duration = Clock::duration::zero();
    auto bucket_duration = Clock::duration::zero();

    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        auto mutate_start = Clock::now();
        auto mutated = mutate_widgets(space, paths, sample, iteration);
        if (!mutated) {
            return std::unexpected(mutated.error());
        }
        mutate_duration += Clock::now() - mutate_start;
        total_dirty_ops += *mutated;

        auto bucket_start = Clock::now();
        for (auto const& path : widget_order) {
            auto descriptor = Declarative::LoadWidgetDescriptor(space, path);
            if (!descriptor) {
                return std::unexpected(descriptor.error());
            }
            auto bucket = Declarative::BuildWidgetBucket(space, *descriptor);
            if (!bucket) {
                return std::unexpected(bucket.error());
            }
            total_bucket_bytes += bucket_bytes(*bucket);
        }
        bucket_duration += Clock::now() - bucket_start;
    }

    metrics.mutate_total_ms = std::chrono::duration<double, std::milli>(mutate_duration).count();
    metrics.bucket_total_ms = std::chrono::duration<double, std::milli>(bucket_duration).count();
    metrics.bucket_avg_ms = metrics.bucket_total_ms / static_cast<double>(options.iterations);
    metrics.bucket_bytes_per_iter = static_cast<double>(total_bucket_bytes)
                                    / static_cast<double>(options.iterations);
    auto mutate_seconds = std::chrono::duration<double>(mutate_duration).count();
    if (mutate_seconds > 0.0) {
        metrics.dirty_per_sec = static_cast<double>(total_dirty_ops) / mutate_seconds;
    }

    auto gpu_upload = run_paint_gpu_cycle(space, paths.paint, options.seed + 42u);
    metrics.paint_gpu_last_upload_ns = gpu_upload.value_or(0.0);
    return metrics;
}

auto write_report_json(CommandLineOptions const& options,
                       DeclarativeMetrics const& declarative) -> nlohmann::json {
    nlohmann::json json;
    json["command"] = {
        {"iterations", options.iterations},
        {"seed", options.seed},
    };

    json["metrics"] = {
        {"declarative.bucketAvgMs", declarative.bucket_avg_ms},
        {"declarative.bucketBytesPerIter", declarative.bucket_bytes_per_iter},
        {"declarative.dirtyWidgetsPerSec", declarative.dirty_per_sec},
        {"declarative.paintGpuLastUploadNs", declarative.paint_gpu_last_upload_ns},
    };

    json["metadata"] = {
        {"declarative", {
             {"bucketTotalMs", declarative.bucket_total_ms},
             {"mutateTotalMs", declarative.mutate_total_ms},
        }},
    };
    return json;
}

} // namespace

int main(int argc, char** argv) {
    SP::UI::LegacyBuilders::ScopedAllow legacy_allow{};
    auto options = parse_args(argc, argv);
    auto sample = make_sample_data();

    SP::PathSpace space;
    DeclarativePaths paths;
    auto widget_order = setup_declarative_scene(space, sample, paths);
    if (!widget_order) {
        std::cerr << "widget_pipeline_benchmark: failed to set up declarative scene\n";
        return 1;
    }

    auto declarative = run_declarative_pipeline(space, sample, *widget_order, paths, options);
    if (!declarative) {
        std::cerr << "widget_pipeline_benchmark: failed to run declarative pipeline\n";
        return 1;
    }

    if (auto shutdown = SP::Scene::Shutdown(space, paths.scene); !shutdown) {
        std::cerr << "widget_pipeline_benchmark: failed to shutdown scene\n";
    }
    SP::System::ShutdownDeclarativeRuntime(space);

    auto report = write_report_json(options, *declarative);
    if (!options.output_path.empty()) {
        std::ofstream file(options.output_path);
        if (!file) {
            std::cerr << "widget_pipeline_benchmark: failed to write '" << options.output_path << "'\n";
            return 1;
        }
        file << std::setw(2) << report << '\n';
    } else {
        std::cout << std::setw(2) << report << '\n';
    }

    if (options.verbose) {
        std::cout << "declarative.bucketAvgMs=" << declarative->bucket_avg_ms << "\n";
        std::cout << "declarative.dirtyWidgetsPerSec=" << declarative->dirty_per_sec << "\n";
        std::cout << "declarative.paintGpuLastUploadNs=" << declarative->paint_gpu_last_upload_ns << "\n";
    }

    return 0;
}
