#include <pathspace/ui/declarative/PaintSurfaceUploader.hpp>
#include <pathspace/ui/declarative/Detail.hpp>

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Out.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace SP::UI::Declarative {

namespace {

namespace Detail = SP::UI::Declarative::Detail;
using DirtyRectHint = SP::UI::Builders::DirtyRectHint;

constexpr std::string_view kAppsRoot = "/system/applications";

template <typename T>
auto ensure_value(PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void> {
    auto existing = Detail::read_optional<T>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return {};
    }
    return Detail::replace_single(space, path, value);
}

auto now_ns() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

auto read_gpu_state(PathSpace& space, std::string const& widget_path) -> PaintGpuState {
    auto state = Detail::read_optional<std::string>(space, widget_path + "/render/gpu/state");
    if (!state || !state->has_value()) {
        return PaintGpuState::Idle;
    }
    return PaintGpuStateFromString(**state);
}

auto set_gpu_state(PathSpace& space,
                   std::string const& widget_path,
                   PaintGpuState state) -> void {
    (void)Detail::replace_single(space,
                                        widget_path + "/render/gpu/state",
                                        std::string(PaintGpuStateToString(state)));
}

auto gpu_enabled(PathSpace& space, std::string const& widget_path) -> bool {
    auto enabled = Detail::read_optional<bool>(space, widget_path + "/render/gpu/enabled");
    if (!enabled || !enabled->has_value()) {
        return false;
    }
    return **enabled;
}

struct RasterizeResult {
    PaintTexturePayload payload;
    std::uint64_t bytes = 0;
};

class PaintSurfaceUploaderWorker {
public:
    PaintSurfaceUploaderWorker(PathSpace& space, PaintSurfaceUploaderOptions options)
        : space_(space)
        , options_(std::move(options)) {}

    ~PaintSurfaceUploaderWorker() {
        stop();
    }

    auto start() -> SP::Expected<void> {
        if (running_.load(std::memory_order_acquire)) {
            return {};
        }
        if (auto status = ensure_runtime_roots(); !status) {
            return status;
        }
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { this->run(); });
        return {};
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        stop_flag_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        (void)Detail::replace_single(space_, options_.state_path, false);
    }

private:
    auto ensure_runtime_roots() -> SP::Expected<void> {
        if (auto status = ensure_value(space_, options_.state_path, false); !status) {
            return status;
        }
        auto uploads = options_.metrics_root + "/uploads_total";
        if (auto status = ensure_value(space_, uploads, std::uint64_t{0}); !status) {
            return status;
        }
        auto partial = options_.metrics_root + "/partial_uploads_total";
        if (auto status = ensure_value(space_, partial, std::uint64_t{0}); !status) {
            return status;
        }
        auto full = options_.metrics_root + "/full_uploads_total";
        if (auto status = ensure_value(space_, full, std::uint64_t{0}); !status) {
            return status;
        }
        auto failures = options_.metrics_root + "/failures_total";
        if (auto status = ensure_value(space_, failures, std::uint64_t{0}); !status) {
            return status;
        }
        auto widgets_pending = options_.metrics_root + "/widgets_pending";
        if (auto status = ensure_value(space_, widgets_pending, std::uint64_t{0}); !status) {
            return status;
        }
        auto last_duration = options_.metrics_root + "/last_upload_ns";
        if (auto status = ensure_value(space_, last_duration, std::uint64_t{0}); !status) {
            return status;
        }
        (void)Detail::replace_single(space_, options_.state_path, true);
        return {};
    }

    void run() {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            pump();
            std::this_thread::sleep_for(options_.poll_interval);
        }
    }

    void pump() {
        auto widgets = enumerate_paint_widgets();
        std::uint64_t pending = 0;
        for (auto const& widget_path : widgets) {
            if (!gpu_enabled(space_, widget_path)) {
                continue;
            }
            auto state = read_gpu_state(space_, widget_path);
            if (state == PaintGpuState::DirtyPartial || state == PaintGpuState::DirtyFull) {
                ++pending;
                upload_widget(widget_path, state);
            }
        }
        write_metric("widgets_pending", pending);
    }

    void write_metric(std::string_view leaf, std::uint64_t value) {
        std::string path = options_.metrics_root;
        path.push_back('/');
        path.append(leaf);
        (void)Detail::replace_single(space_, path, value);
    }

    void increment_metric(std::string_view leaf, std::uint64_t delta = 1) {
        std::string path = options_.metrics_root;
        path.push_back('/');
        path.append(leaf);
        auto current = Detail::read_optional<std::uint64_t>(space_, path);
        if (!current || !current->has_value()) {
            (void)Detail::replace_single(space_, path, delta);
            return;
        }
        (void)Detail::replace_single(space_, path, **current + delta);
    }

    void log_error(std::string message) {
        if (options_.log_root.empty()) {
            return;
        }
        (void)space_.insert(options_.log_root, std::move(message));
    }

    void upload_widget(std::string const& widget_path, PaintGpuState state) {
        set_gpu_state(space_, widget_path, PaintGpuState::Uploading);
        auto start_ns = now_ns();
        (void)Detail::replace_single(space_, widget_path + "/render/gpu/fence/start", start_ns);

        auto metrics = PaintRuntime::ReadBufferMetrics(space_, widget_path);
        if (!metrics) {
            fail_widget(widget_path, metrics.error().message.value_or("failed to read buffer metrics"));
            return;
        }
        auto strokes = PaintRuntime::LoadStrokeRecords(space_, widget_path);
        if (!strokes) {
            fail_widget(widget_path, strokes.error().message.value_or("failed to load strokes"));
            return;
        }
        auto revision = Detail::read_optional<std::uint64_t>(space_, widget_path + "/render/buffer/revision");
        std::uint64_t current_revision = 0;
        if (revision && revision->has_value()) {
            current_revision = **revision;
        }

        auto rasterized = rasterize_texture(*metrics, *strokes);
        rasterized.payload.revision = current_revision;

        auto texture_path = widget_path + "/assets/texture";
        auto stored = Detail::replace_single(space_, texture_path, rasterized.payload);
        if (!stored) {
            fail_widget(widget_path, stored.error().message.value_or("failed to write texture payload"));
            return;
        }

        (void)Detail::replace_single(space_, widget_path + "/render/buffer/pendingDirty", std::vector<DirtyRectHint>{});
        drain_dirty_queue(widget_path);

        auto end_ns = now_ns();
        (void)Detail::replace_single(space_, widget_path + "/render/gpu/fence/end", end_ns);

        update_widget_stats(widget_path, state, rasterized.bytes, end_ns - start_ns, current_revision);
        set_gpu_state(space_, widget_path, PaintGpuState::Ready);
        write_metric("last_upload_ns", end_ns - start_ns);
    }

    void fail_widget(std::string const& widget_path, std::string const& reason) {
        log_error(std::string(widget_path).append(": ").append(reason));
        set_gpu_state(space_, widget_path, PaintGpuState::Error);
        increment_metric("failures_total");
    }

    void update_widget_stats(std::string const& widget_path,
                             PaintGpuState state,
                             std::uint64_t upload_bytes,
                             std::uint64_t duration_ns,
                             std::uint64_t revision) {
        auto stats_path = widget_path + "/render/gpu/stats";
        auto stats_value = Detail::read_optional<PaintGpuStats>(space_, stats_path);
        PaintGpuStats stats = stats_value && stats_value->has_value() ? **stats_value : PaintGpuStats{};
        stats.uploads_total += 1;
        if (state == PaintGpuState::DirtyFull) {
            stats.full_uploads += 1;
            increment_metric("full_uploads_total");
        } else {
            stats.partial_uploads += 1;
            increment_metric("partial_uploads_total");
        }
        stats.last_upload_bytes = upload_bytes;
        stats.last_upload_duration_ns = duration_ns;
        stats.last_revision = revision;
        (void)Detail::replace_single(space_, stats_path, stats);
        increment_metric("uploads_total");
    }

    auto rasterize_texture(PaintBufferMetrics const& metrics,
                           std::vector<PaintStrokeRecord> const& strokes) -> RasterizeResult {
        RasterizeResult result{};
        auto width = std::max<std::uint32_t>(1u, metrics.width);
        auto height = std::max<std::uint32_t>(1u, metrics.height);
        auto stride = width * 4u;
        result.payload.width = width;
        result.payload.height = height;
        result.payload.stride = stride;
        result.payload.pixels.assign(static_cast<std::size_t>(stride) * height, 0);

        for (auto const& stroke : strokes) {
            auto radius = std::max(stroke.meta.brush_size * 0.5f, 1.0f);
            std::array<std::uint8_t, 4> rgba{};
            for (std::size_t i = 0; i < 4; ++i) {
                rgba[i] = static_cast<std::uint8_t>(std::clamp(stroke.meta.color[i], 0.0f, 1.0f) * 255.0f);
            }
            for (auto const& point : stroke.points) {
                auto min_x = std::max(0, static_cast<int>(std::floor(point.x - radius)));
                auto max_x = std::min(static_cast<int>(width), static_cast<int>(std::ceil(point.x + radius)));
                auto min_y = std::max(0, static_cast<int>(std::floor(point.y - radius)));
                auto max_y = std::min(static_cast<int>(height), static_cast<int>(std::ceil(point.y + radius)));
                auto radius_sq = radius * radius;
                for (int y = min_y; y < max_y; ++y) {
                    for (int x = min_x; x < max_x; ++x) {
                        float dx = static_cast<float>(x) - point.x;
                        float dy = static_cast<float>(y) - point.y;
                        if ((dx * dx + dy * dy) > radius_sq) {
                            continue;
                        }
                        auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
                        result.payload.pixels[offset + 0] = rgba[0];
                        result.payload.pixels[offset + 1] = rgba[1];
                        result.payload.pixels[offset + 2] = rgba[2];
                        result.payload.pixels[offset + 3] = rgba[3];
                    }
                }
            }
        }
        result.bytes = result.payload.pixels.size();
        return result;
    }

    void drain_dirty_queue(std::string const& widget_path) {
        auto queue_path = widget_path + "/render/gpu/dirtyRects";
        while (true) {
            auto rect = space_.take<DirtyRectHint>(queue_path, SP::Out{} & SP::Block{0ms});
            if (!rect) {
                break;
            }
        }
    }

    auto enumerate_paint_widgets() -> std::vector<std::string> {
        std::vector<std::string> widgets;
        auto apps = space_.listChildren(SP::ConcretePathStringView{std::string{kAppsRoot}});
        for (auto const& app : apps) {
            auto app_root = std::string{kAppsRoot};
            app_root.append("/").append(app);
            collect_window_widgets(app_root + "/windows", widgets);
            collect_widget_subtree(app_root + "/widgets", widgets);
        }
        return widgets;
    }

    void collect_window_widgets(std::string const& root, std::vector<std::string>& widgets) {
        auto windows = space_.listChildren(SP::ConcretePathStringView{root});
        for (auto const& window_name : windows) {
            auto window_root = root + "/" + window_name;
            auto views_root = window_root + "/views";
            auto views = space_.listChildren(SP::ConcretePathStringView{views_root});
            for (auto const& view_name : views) {
                auto view_root = views_root + "/" + view_name + "/widgets";
                collect_widget_subtree(view_root, widgets);
            }
        }
    }

    void collect_widget_subtree(std::string const& root, std::vector<std::string>& widgets) {
        auto entries = space_.listChildren(SP::ConcretePathStringView{root});
        for (auto const& name : entries) {
            auto widget_root = root + "/" + name;
            auto descriptor = Detail::read_optional<RenderDescriptor>(space_, widget_root + "/render/synthesize");
            if (descriptor && descriptor->has_value()
                && descriptor->value().kind == WidgetKind::PaintSurface) {
                widgets.push_back(widget_root);
            }
            collect_widget_subtree(widget_root + "/children", widgets);
        }
    }

    PathSpace& space_;
    PaintSurfaceUploaderOptions options_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::thread worker_;
};

std::mutex g_uploader_mutex;
std::unique_ptr<PaintSurfaceUploaderWorker> g_uploader;

} // namespace

auto CreatePaintSurfaceUploader(PathSpace& space,
                                PaintSurfaceUploaderOptions const& options) -> SP::Expected<bool> {
    std::lock_guard<std::mutex> guard(g_uploader_mutex);
    if (g_uploader) {
        return false;
    }
    auto worker = std::make_unique<PaintSurfaceUploaderWorker>(space, options);
    auto started = worker->start();
    if (!started) {
        return std::unexpected(started.error());
    }
    g_uploader = std::move(worker);
    return true;
}

auto ShutdownPaintSurfaceUploader(PathSpace& space) -> void {
    (void)space;
    std::lock_guard<std::mutex> guard(g_uploader_mutex);
    if (!g_uploader) {
        return;
    }
    g_uploader->stop();
    g_uploader.reset();
}

} // namespace SP::UI::Declarative
