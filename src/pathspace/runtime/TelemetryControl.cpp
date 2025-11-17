#include <pathspace/runtime/TelemetryControl.hpp>

#include <pathspace/core/Out.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP::Runtime {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kConfigPushEnabled = "/config/push/enabled";
constexpr std::string_view kConfigPushTelemetry = "/config/push/telemetry_enabled";
constexpr std::string_view kConfigPushRateLimit = "/config/push/rate_limit_hz";
constexpr std::string_view kConfigPushMaxQueue = "/config/push/max_queue";

class TelemetryControlWorker : public std::enable_shared_from_this<TelemetryControlWorker> {
public:
    TelemetryControlWorker(PathSpace& space, TelemetryControlOptions options)
        : space_(space), options_(std::move(options)) {}

    auto start() -> SP::Expected<void> {
        if (running_) {
            return {};
        }
        if (auto ensured = ensure_roots(); !ensured) {
            return ensured;
        }
        stop_flag_ = std::make_shared<std::atomic<bool>>(false);
        running_ = true;
        worker_ = std::thread([self = shared_from_this()] { self->run(); });
        if (!options_.state_path.empty()) {
            set_bool(options_.state_path, true);
        }
        return {};
    }

    void stop() {
        if (!running_) {
            return;
        }
        stop_flag_->store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (!options_.state_path.empty()) {
            set_bool(options_.state_path, false);
        }
        running_ = false;
    }

private:
    using CommandFn = void (TelemetryControlWorker::*)(TelemetryToggleCommand const&);

    void run() {
        while (!stop_flag_->load(std::memory_order_acquire)) {
            bool processed = false;
            processed |= take_command<TelemetryToggleCommand>(options_.telemetry_start_queue,
                                                              [this](TelemetryToggleCommand const& cmd) {
                                                                  apply_telemetry(cmd.enable);
                                                              });
            processed |= take_command<TelemetryToggleCommand>(options_.telemetry_stop_queue,
                                                              [this](TelemetryToggleCommand const& cmd) {
                                                                  apply_telemetry(false);
                                                              });
            processed |= take_command<DevicePushCommand>(options_.push_command_queue,
                                                         [this](DevicePushCommand const& cmd) {
                                                             apply_push_command(cmd);
                                                         });
            processed |= take_command<DeviceThrottleCommand>(options_.throttle_command_queue,
                                                             [this](DeviceThrottleCommand const& cmd) {
                                                                 apply_throttle_command(cmd);
                                                             });
            if (!processed) {
                std::this_thread::sleep_for(options_.idle_sleep);
            }
        }
    }

    template <typename Command, typename Handler>
    bool take_command(std::string const& path, Handler&& handler) {
        auto block = SP::Out{} & SP::Block{options_.block_timeout};
        auto value = space_.take<Command, std::string>(path, block);
        if (value) {
            handler(*value);
            return true;
        }
        auto const& error = value.error();
        if (error.code == SP::Error::Code::Timeout ||
            error.code == SP::Error::Code::NoObjectFound) {
            return false;
        }
        log_error("TelemetryControl take(" + path + ") failed: "
                  + error.message.value_or("unknown error"));
        return false;
    }

    auto ensure_roots() -> SP::Expected<void> {
        auto current = space_.read<bool, std::string>(options_.telemetry_toggle_path);
        if (!current) {
            auto inserted = space_.insert(options_.telemetry_toggle_path, false);
            if (!inserted.errors.empty()) {
                return std::unexpected(inserted.errors.front());
            }
        }
        if (!options_.state_path.empty()) {
            set_bool(options_.state_path, false);
        }
        return {};
    }

    void apply_telemetry(bool enable) {
        set_bool(options_.telemetry_toggle_path, enable);
        auto devices = resolve_devices("*");
        for (auto const& device : devices) {
            set_bool(device + std::string{kConfigPushTelemetry}, enable);
        }
    }

    void apply_push_command(DevicePushCommand const& command) {
        auto devices = resolve_devices(command.device);
        if (devices.empty()) {
            log_error("TelemetryControl: no devices matched " + command.device);
            return;
        }
        for (auto const& device : devices) {
            if (command.touch_push_enabled) {
                set_bool(device + std::string{kConfigPushEnabled}, command.enable);
            }
            auto subscriber = command.subscriber.empty() ? options_.default_subscriber
                                                         : command.subscriber;
            if (!subscriber.empty()) {
                auto subscriber_path = device + "/config/push/subscribers/" + subscriber;
                set_bool(subscriber_path, command.enable);
            } else {
                log_error("TelemetryControl push command missing subscriber for device " + device);
            }
            if (command.set_telemetry) {
                set_bool(device + std::string{kConfigPushTelemetry}, command.telemetry_enabled);
            }
        }
    }

    void apply_throttle_command(DeviceThrottleCommand const& command) {
        if (!command.set_rate_limit && !command.set_max_queue) {
            log_error("TelemetryControl throttle command missing fields");
            return;
        }
        auto devices = resolve_devices(command.device);
        if (devices.empty()) {
            log_error("TelemetryControl: no devices matched " + command.device);
            return;
        }
        for (auto const& device : devices) {
            if (command.set_rate_limit) {
                set_uint(device + std::string{kConfigPushRateLimit}, command.rate_limit_hz);
            }
            if (command.set_max_queue) {
                set_uint(device + std::string{kConfigPushMaxQueue}, command.max_queue);
            }
        }
    }

    std::vector<std::string> resolve_devices(std::string pattern) {
        if (pattern.empty() || pattern == "*") {
            return enumerate_devices();
        }
        bool prefix_match = false;
        if (!pattern.empty() && pattern.back() == '*') {
            prefix_match = true;
            pattern.pop_back();
            while (!pattern.empty() && pattern.back() == '/') {
                pattern.pop_back();
            }
        }

        auto devices = enumerate_devices();
        std::vector<std::string> matches;
        matches.reserve(devices.size());
        for (auto const& device : devices) {
            if (prefix_match) {
                if (device.rfind(pattern, 0) == 0) {
                    matches.push_back(device);
                }
                continue;
            }
            if (device == pattern) {
                matches.push_back(device);
            }
        }
        return matches;
    }

    std::vector<std::string> enumerate_devices() {
        std::vector<std::string> devices;
        auto classes = space_.listChildren(SP::ConcretePathStringView{options_.devices_root});
        for (auto const& cls : classes) {
            std::string class_root = options_.devices_root;
            if (!class_root.empty() && class_root.back() != '/') {
                class_root.push_back('/');
            }
            class_root.append(cls);
            auto ids = space_.listChildren(SP::ConcretePathStringView{class_root});
            for (auto const& id : ids) {
                std::string device = class_root;
                device.push_back('/');
                device.append(id);
                devices.push_back(std::move(device));
            }
        }
        return devices;
    }

    template <typename T>
    void replace_value(std::string const& path, T const& value) {
        while (true) {
            auto taken = space_.take<T, std::string>(path);
            if (taken) {
                continue;
            }
            auto const& error = taken.error();
            if (error.code == SP::Error::Code::NoObjectFound ||
                error.code == SP::Error::Code::NoSuchPath) {
                break;
            }
            log_error("TelemetryControl failed to clear " + path + ": "
                      + error.message.value_or("unknown error"));
            return;
        }

        auto inserted = space_.insert(path, value);
        if (!inserted.errors.empty()) {
            log_error("TelemetryControl failed to set " + path + ": "
                      + inserted.errors.front().message.value_or("unknown error"));
        }
    }

    void set_bool(std::string const& path, bool value) {
        replace_value(path, value);
    }

    void set_uint(std::string const& path, std::uint32_t value) {
        replace_value(path, value);
    }

    void log_error(std::string message) {
        if (options_.log_path.empty()) {
            return;
        }
        space_.insert(options_.log_path, std::move(message));
    }

    PathSpace& space_;
    TelemetryControlOptions options_;
    std::shared_ptr<std::atomic<bool>> stop_flag_;
    std::thread worker_;
    bool running_ = false;
};

std::mutex g_worker_mutex;
std::unordered_map<PathSpace*, std::shared_ptr<TelemetryControlWorker>> g_workers;

} // namespace

auto CreateTelemetryControl(PathSpace& space,
                            TelemetryControlOptions const& options) -> SP::Expected<bool> {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_workers.find(&space);
    if (it != g_workers.end() && it->second) {
        return false;
    }
    auto worker = std::make_shared<TelemetryControlWorker>(space, options);
    if (auto started = worker->start(); !started) {
        return std::unexpected(started.error());
    }
    g_workers[&space] = worker;
    return true;
}

auto ShutdownTelemetryControl(PathSpace& space) -> void {
    std::shared_ptr<TelemetryControlWorker> worker;
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
