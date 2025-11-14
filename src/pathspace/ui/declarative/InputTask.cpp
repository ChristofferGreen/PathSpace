#include <pathspace/ui/declarative/InputTask.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/ui/Builders.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SP::UI::Declarative {

namespace {

using namespace SP::UI::Builders;
using namespace SP::UI::Builders::Detail;

constexpr std::string_view kRuntimeBase = "/system/widgets/runtime/input";
constexpr std::string_view kStateRunning = "/system/widgets/runtime/input/state/running";
constexpr std::string_view kMetricsBase = "/system/widgets/runtime/input/metrics";
constexpr std::string_view kMetricsLastPump = "/system/widgets/runtime/input/metrics/last_pump_ns";
constexpr std::string_view kMetricsWidgets = "/system/widgets/runtime/input/metrics/widgets_processed_total";
constexpr std::string_view kMetricsActions = "/system/widgets/runtime/input/metrics/actions_published_total";
constexpr std::string_view kMetricsActive = "/system/widgets/runtime/input/metrics/widgets_with_work_total";
constexpr std::string_view kLogErrors = "/system/widgets/runtime/input/log/errors/queue";

template <typename T>
auto ensure_value(PathSpace& space,
                  std::string const& path,
                  T const& value) -> SP::Expected<void> {
    auto existing = read_optional<T>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return {};
    }
    return replace_single<T>(space, path, value);
}

auto ensure_runtime_roots(PathSpace& space) -> SP::Expected<void> {
    if (auto status = ensure_value<bool>(space, std::string{kStateRunning}, false); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsWidgets}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsActions}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsActive}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsLastPump}, 0); !status) {
        return status;
    }
    return {};
}

auto enqueue_error(PathSpace& space, std::string message) -> void {
    auto inserted = space.insert(std::string{kLogErrors}, std::move(message));
    (void)inserted;
}

auto list_children(PathSpace& space, std::string const& path) -> std::vector<std::string> {
    return space.listChildren(SP::ConcretePathStringView{path});
}

struct PumpStats {
    std::size_t widgets_processed = 0;
    std::size_t widgets_with_work = 0;
    std::size_t actions_published = 0;
};

auto pump_widget(PathSpace& space,
                 std::string const& widget_root,
                 std::size_t max_actions,
                 PumpStats& stats) -> void {
    auto widget_path = WidgetPath{widget_root};
    auto processed = Widgets::Reducers::ProcessPendingActions(space, widget_path, max_actions);
    if (!processed) {
        std::ostringstream oss;
        oss << "ProcessPendingActions failed for " << widget_root << ": ";
        if (processed.error().message) {
            oss << *processed.error().message;
        } else {
            oss << "unknown error";
        }
        enqueue_error(space, oss.str());
        return;
    }
    stats.widgets_processed++;
    if (!processed->actions.empty()) {
        stats.widgets_with_work++;
        stats.actions_published += processed->actions.size();
    }
}

auto pump_once(PathSpace& space, InputTaskOptions const& options) -> PumpStats {
    PumpStats stats{};

    auto apps = list_children(space, "/system/applications");
    for (auto const& app : apps) {
        std::string widgets_root = "/system/applications/";
        widgets_root.append(app);
        widgets_root.append("/widgets");

        auto widgets = list_children(space, widgets_root);
        for (auto const& widget : widgets) {
            std::string widget_root = widgets_root;
            widget_root.push_back('/');
            widget_root.append(widget);
            pump_widget(space, widget_root, options.max_actions_per_widget, stats);
        }
    }

    return stats;
}

auto now_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

class InputRuntimeWorker {
public:
    InputRuntimeWorker(PathSpace& space, InputTaskOptions options)
        : space_(space)
        , options_(options) {
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { this->run(); });
    }

    ~InputRuntimeWorker() {
        stop();
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
        (void)replace_single<bool>(space_, std::string{kStateRunning}, false);
    }

private:
    void run() {
        (void)replace_single<bool>(space_, std::string{kStateRunning}, true);
        auto sleep_interval = options_.poll_interval.count() <= 0
            ? std::chrono::milliseconds{1}
            : options_.poll_interval;
        while (!stop_flag_.load(std::memory_order_acquire)) {
            auto stats = pump_once(space_, options_);
            publish_metrics(stats);
            std::this_thread::sleep_for(sleep_interval);
        }
    }

    void publish_metrics(PumpStats const& stats) {
        total_widgets_ += stats.widgets_processed;
        total_actions_ += stats.actions_published;
        total_active_widgets_ += stats.widgets_with_work;

        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsWidgets}, total_widgets_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsActions}, total_actions_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsActive}, total_active_widgets_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsLastPump}, now_ns());
    }

private:
    PathSpace& space_;
    InputTaskOptions options_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::uint64_t total_widgets_ = 0;
    std::uint64_t total_actions_ = 0;
    std::uint64_t total_active_widgets_ = 0;
};

std::mutex g_runtime_mutex;
std::unordered_map<PathSpace*, std::shared_ptr<InputRuntimeWorker>> g_runtime_workers;

} // namespace

auto EnsureInputTask(PathSpace& space,
                     InputTaskOptions const& options) -> SP::Expected<bool> {
    if (auto ensured = ensure_runtime_roots(space); !ensured) {
        return std::unexpected(ensured.error());
    }

    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    auto it = g_runtime_workers.find(&space);
    if (it != g_runtime_workers.end()) {
        if (it->second) {
            return false;
        }
    }

    auto worker = std::make_shared<InputRuntimeWorker>(space, options);
    g_runtime_workers[&space] = worker;
    return true;
}

auto ShutdownInputTask(PathSpace& space) -> void {
    std::shared_ptr<InputRuntimeWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        auto it = g_runtime_workers.find(&space);
        if (it != g_runtime_workers.end()) {
            worker = std::move(it->second);
            g_runtime_workers.erase(it);
        }
    }
    if (worker) {
        worker->stop();
    }
}

} // namespace SP::UI::Declarative
