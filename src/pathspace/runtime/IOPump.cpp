#include <pathspace/runtime/IOPump.hpp>

#include <pathspace/io/IoEvents.hpp>
#include <pathspace/log/TaggedLogger.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SP::Runtime {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kLogErrors = "/system/widgets/runtime/input/log/errors/queue";

struct WindowBinding {
    std::string token;
    std::string window_path;
    std::string pointer_queue;
    std::string button_queue;
    std::string text_queue;
    std::vector<std::string> pointer_devices;
    std::vector<std::string> button_devices;
    std::vector<std::string> text_devices;
};

auto now_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void enqueue_error(PathSpace& space, std::string message) {
    auto inserted = space.insert(std::string{kLogErrors}, std::move(message));
    (void)inserted;
}

std::string normalize_root(std::string root) {
    if (root.empty()) {
        return std::string{"/"};
    }
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    if (root.empty()) {
        return std::string{"/"};
    }
    return root;
}

std::chrono::microseconds clamp_duration(std::chrono::microseconds value,
                                         std::chrono::microseconds fallback,
                                         std::chrono::microseconds minimum) {
    if (value <= std::chrono::microseconds::zero()) {
        return std::max(fallback, minimum);
    }
    return std::max(value, minimum);
}

std::chrono::milliseconds clamp_duration(std::chrono::milliseconds value,
                                         std::chrono::milliseconds fallback,
                                         std::chrono::milliseconds minimum) {
    if (value <= std::chrono::milliseconds::zero()) {
        return std::max(fallback, minimum);
    }
    return std::max(value, minimum);
}

std::vector<std::string> list_children(PathSpace& space, std::string const& root) {
    SP::ConcretePathStringView view{root};
    return space.listChildren(view);
}

template <typename T>
auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T, std::string>(path);
        if (!taken) {
            auto const& error = taken.error();
            if (error.code == SP::Error::Code::NoObjectFound
                || error.code == SP::Error::Code::NoSuchPath) {
                break;
            }
            return std::unexpected(error);
        }
    }
    return {};
}

template <typename T>
auto replace_single(PathSpace& space,
                   std::string const& path,
                   T const& value) -> SP::Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

template <typename T>
auto read_optional(PathSpace const& space,
                   std::string const& path) -> SP::Expected<std::optional<T>> {
    auto value = space.read<T, std::string>(path);
    if (value) {
        return std::optional<T>{*value};
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound
        || error.code == SP::Error::Code::NoSuchPath) {
        return std::optional<T>{};
    }
    return std::unexpected(error);
}

auto ensure_runtime_roots(PathSpace& space,
                          IoPumpOptions const& options) -> SP::Expected<void> {
    auto state_path = options.state_path.empty()
        ? std::string{"/system/widgets/runtime/io/state/running"}
        : options.state_path;
    if (auto status = replace_single<bool>(space, state_path, false); !status) {
        return status;
    }

    auto metrics_root = options.metrics_root.empty()
        ? std::string{"/system/widgets/runtime/input/metrics"}
        : options.metrics_root;
    if (auto status = replace_single<std::uint64_t>(space,
                                                    metrics_root + "/pointer_events_total",
                                                    0);
        !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space,
                                                    metrics_root + "/button_events_total",
                                                    0);
        !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space,
                                                    metrics_root + "/text_events_total",
                                                    0);
        !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space,
                                                    metrics_root + "/events_dropped_total",
                                                    0);
        !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space,
                                                    metrics_root + "/last_pump_ns",
                                                    0);
        !status) {
        return status;
    }
    return {};
}

class IoPumpWorker : public std::enable_shared_from_this<IoPumpWorker> {
public:
    IoPumpWorker(PathSpace& space, IoPumpOptions options)
        : space_(space)
        , options_(options)
        , windows_root_(normalize_root(options.windows_root))
        , events_root_(normalize_root(options.events_root))
        , metrics_root_(options.metrics_root.empty()
              ? std::string{"/system/widgets/runtime/input/metrics"}
              : options.metrics_root)
        , state_path_(options.state_path.empty()
              ? std::string{"/system/widgets/runtime/io/state/running"}
              : options.state_path) {
        block_timeout_ = clamp_duration(options_.block_timeout,
                                        std::chrono::microseconds{200},
                                        std::chrono::microseconds{50});
        idle_sleep_ = clamp_duration(options_.idle_sleep,
                                     std::chrono::milliseconds{1},
                                     std::chrono::milliseconds{1});
        subscription_refresh_ = clamp_duration(options_.subscription_refresh,
                                               std::chrono::milliseconds{250},
                                               std::chrono::milliseconds{50});
        if (!options_.stop_flag) {
            stop_flag_ = std::make_shared<std::atomic<bool>>(false);
        } else {
            stop_flag_ = options_.stop_flag;
            stop_flag_->store(false, std::memory_order_release);
        }
        global_pointer_queue_ = events_root_ + "/global/pointer/queue";
        global_button_queue_ = events_root_ + "/global/button/queue";
        global_text_queue_ = events_root_ + "/global/text/queue";
    }

    ~IoPumpWorker() {
        stop();
    }

    void start() {
        worker_ = std::thread([self = shared_from_this()] { self->run(); });
    }

    void stop() {
        bool expected = false;
        if (stop_requested_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            stop_flag_->store(true, std::memory_order_release);
            if (worker_.joinable()) {
                worker_.join();
            }
        } else if (worker_.joinable()) {
            stop_flag_->store(true, std::memory_order_release);
            worker_.join();
        }
    }

private:
    void run() {
        (void)replace_single<bool>(space_, state_path_, true);
        refresh_bindings();
        auto next_refresh = std::chrono::steady_clock::now() + subscription_refresh_;

        while (!stop_flag_->load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= next_refresh) {
                refresh_bindings();
                next_refresh = std::chrono::steady_clock::now() + subscription_refresh_;
            }

            bool processed = false;
            processed |= pump_pointer();
            processed |= pump_button();
            processed |= pump_text();

            if (!processed) {
                std::this_thread::sleep_for(idle_sleep_);
            } else {
                publish_metrics();
            }
        }

        (void)replace_single<bool>(space_, state_path_, false);
        publish_metrics();
    }

    bool pump_pointer() {
        auto block_duration = std::chrono::duration_cast<std::chrono::milliseconds>(block_timeout_);
        if (block_duration <= std::chrono::milliseconds::zero()) {
            block_duration = std::chrono::milliseconds{1};
        }
        auto event = space_.take<SP::IO::PointerEvent, std::string>(
            std::string{SP::IO::IoEventPaths::kPointerQueue},
            SP::Out{} & SP::Block{block_duration});
        if (!event) {
            auto const& error = event.error();
            if (error.code == SP::Error::Code::Timeout
                || error.code == SP::Error::Code::NoObjectFound) {
                return false;
            }
            enqueue_error(space_, "IOPump pointer take failed: " + error.message.value_or("unknown error"));
            return false;
        }
        ++pointer_events_total_;
        bool delivered = dispatch_event(*event,
                                        pointer_routes_,
                                        [](WindowBinding const& binding) {
                                            return binding.pointer_queue;
                                        },
                                        global_pointer_queue_);
        if (!delivered) {
            ++dropped_events_total_;
        }
        last_pump_ns_ = now_ns();
        return true;
    }

    bool pump_button() {
        auto event = space_.take<SP::IO::ButtonEvent, std::string>(
            std::string{SP::IO::IoEventPaths::kButtonQueue});
        if (!event) {
            auto const& error = event.error();
            if (error.code == SP::Error::Code::NoObjectFound) {
                return false;
            }
            if (error.code == SP::Error::Code::Timeout) {
                return false;
            }
            enqueue_error(space_, "IOPump button take failed: " + error.message.value_or("unknown error"));
            return false;
        }
        ++button_events_total_;
        bool delivered = dispatch_event(*event,
                                        button_routes_,
                                        [](WindowBinding const& binding) {
                                            return binding.button_queue;
                                        },
                                        global_button_queue_);
        if (!delivered) {
            ++dropped_events_total_;
        }
        last_pump_ns_ = now_ns();
        return true;
    }

    bool pump_text() {
        auto event = space_.take<SP::IO::TextEvent, std::string>(
            std::string{SP::IO::IoEventPaths::kTextQueue});
        if (!event) {
            auto const& error = event.error();
            if (error.code == SP::Error::Code::NoObjectFound) {
                return false;
            }
            if (error.code == SP::Error::Code::Timeout) {
                return false;
            }
            enqueue_error(space_, "IOPump text take failed: " + error.message.value_or("unknown error"));
            return false;
        }
        ++text_events_total_;
        bool delivered = dispatch_event(*event,
                                        text_routes_,
                                        [](WindowBinding const& binding) {
                                            return binding.text_queue;
                                        },
                                        global_text_queue_);
        if (!delivered) {
            ++dropped_events_total_;
        }
        last_pump_ns_ = now_ns();
        return true;
    }

    template <typename Event, typename Map, typename QueueAccessor>
    bool dispatch_event(Event const& event,
                        Map const& map,
                        QueueAccessor accessor,
                        std::string const& global_queue) {
        bool delivered = false;
        auto it = map.find(event.device_path);
        if (it != map.end()) {
            for (auto index : it->second) {
                if (index >= bindings_.size()) {
                    continue;
                }
                auto const& binding = bindings_[index];
                if (enqueue_event(accessor(binding), event)) {
                    delivered = true;
                }
            }
        }

        if (!delivered && options_.fanout_unmatched_to_global) {
            delivered = enqueue_event(global_queue, event);
        }
        return delivered;
    }

    template <typename Event>
    bool enqueue_event(std::string const& queue_path, Event const& event) {
        auto inserted = space_.insert(queue_path, event);
        if (!inserted.errors.empty()) {
            const auto& error = inserted.errors.front();
            enqueue_error(space_, "IOPump enqueue failed at " + queue_path + ": "
                                   + error.message.value_or("unknown error"));
            return false;
        }
        return true;
    }

    void refresh_bindings() {
        auto entries = list_children(space_, windows_root_);
        std::vector<WindowBinding> updated;
        updated.reserve(entries.size());

        for (auto const& token : entries) {
            std::string base = windows_root_;
            base.push_back('/');
            base.append(token);

            auto window_path = read_optional<std::string>(space_, base + "/window");
            if (!window_path) {
                enqueue_error(space_, "IOPump failed to read window path for " + base);
                continue;
            }
            if (!window_path->has_value()) {
                continue;
            }

            WindowBinding binding{};
            binding.token = token;
            binding.window_path = **window_path;
            binding.pointer_queue = events_root_ + "/" + token + "/pointer/queue";
            binding.button_queue = events_root_ + "/" + token + "/button/queue";
            binding.text_queue = events_root_ + "/" + token + "/text/queue";

            auto pointer_devices = read_optional<std::vector<std::string>>(space_,
                                                                           base + "/subscriptions/pointer/devices");
            if (!pointer_devices) {
                enqueue_error(space_, "IOPump failed to read pointer subscriptions for " + base);
                continue;
            }
            if (pointer_devices->has_value()) {
                binding.pointer_devices = **pointer_devices;
            }

            auto button_devices = read_optional<std::vector<std::string>>(space_,
                                                                          base + "/subscriptions/button/devices");
            if (!button_devices) {
                enqueue_error(space_, "IOPump failed to read button subscriptions for " + base);
                continue;
            }
            if (button_devices->has_value()) {
                binding.button_devices = **button_devices;
            }

            auto text_devices = read_optional<std::vector<std::string>>(space_,
                                                                        base + "/subscriptions/text/devices");
            if (!text_devices) {
                enqueue_error(space_, "IOPump failed to read text subscriptions for " + base);
                continue;
            }
            if (text_devices->has_value()) {
                binding.text_devices = **text_devices;
            }

            updated.push_back(std::move(binding));
        }

        bindings_ = std::move(updated);
        rebuild_routes();
    }

    void rebuild_routes() {
        pointer_routes_.clear();
        button_routes_.clear();
        text_routes_.clear();

        for (std::size_t index = 0; index < bindings_.size(); ++index) {
            auto const& binding = bindings_[index];
            for (auto const& device : binding.pointer_devices) {
                pointer_routes_[device].push_back(index);
            }
            for (auto const& device : binding.button_devices) {
                button_routes_[device].push_back(index);
            }
            for (auto const& device : binding.text_devices) {
                text_routes_[device].push_back(index);
            }
        }
    }

    void publish_metrics() {
        (void)replace_single<std::uint64_t>(space_,
                                            metrics_root_ + "/pointer_events_total",
                                            pointer_events_total_);
        (void)replace_single<std::uint64_t>(space_,
                                            metrics_root_ + "/button_events_total",
                                            button_events_total_);
        (void)replace_single<std::uint64_t>(space_,
                                            metrics_root_ + "/text_events_total",
                                            text_events_total_);
        (void)replace_single<std::uint64_t>(space_,
                                            metrics_root_ + "/events_dropped_total",
                                            dropped_events_total_);
        (void)replace_single<std::uint64_t>(space_,
                                            metrics_root_ + "/last_pump_ns",
                                            last_pump_ns_);
    }

private:
    PathSpace& space_;
    IoPumpOptions options_;
    std::string windows_root_;
    std::string events_root_;
    std::string metrics_root_;
    std::string state_path_;
    std::string global_pointer_queue_;
    std::string global_button_queue_;
    std::string global_text_queue_;
    std::shared_ptr<std::atomic<bool>> stop_flag_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;

    std::chrono::microseconds block_timeout_{};
    std::chrono::milliseconds idle_sleep_{};
    std::chrono::milliseconds subscription_refresh_{};

    std::vector<WindowBinding> bindings_;
    std::unordered_map<std::string, std::vector<std::size_t>> pointer_routes_;
    std::unordered_map<std::string, std::vector<std::size_t>> button_routes_;
    std::unordered_map<std::string, std::vector<std::size_t>> text_routes_;

    std::uint64_t pointer_events_total_ = 0;
    std::uint64_t button_events_total_ = 0;
    std::uint64_t text_events_total_ = 0;
    std::uint64_t dropped_events_total_ = 0;
    std::uint64_t last_pump_ns_ = 0;
};

std::mutex g_worker_mutex;
std::unordered_map<PathSpace*, std::shared_ptr<IoPumpWorker>> g_workers;

} // namespace

auto MakeRuntimeWindowToken(std::string_view window_path) -> std::string {
    std::string token;
    token.reserve(window_path.size());
    for (char ch : window_path) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            token.push_back(static_cast<char>(ch));
        } else {
            token.push_back('_');
        }
    }
    while (!token.empty() && token.front() == '_') {
        token.erase(token.begin());
    }
    if (token.empty()) {
        token = "window";
    }
    return token;
}

auto CreateIOPump(PathSpace& space,
                  IoPumpOptions const& options) -> SP::Expected<bool> {
    if (auto ensured = ensure_runtime_roots(space, options); !ensured) {
        return std::unexpected(ensured.error());
    }

    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_workers.find(&space);
    if (it != g_workers.end() && it->second) {
        return false;
    }

    auto worker = std::make_shared<IoPumpWorker>(space, options);
    worker->start();
    g_workers[&space] = worker;
    return true;
}

auto ShutdownIOPump(PathSpace& space) -> void {
    std::shared_ptr<IoPumpWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_workers.find(&space);
        if (it != g_workers.end()) {
            worker = std::move(it->second);
            g_workers.erase(it);
        }
    }
    if (worker) {
        worker->stop();
    }
}

} // namespace SP::Runtime
