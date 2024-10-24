#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <source_location>
#include <string>
#include <thread>
#include <unordered_map>

namespace SP {

class TaggedLogger {
public:
    struct LogMessage {
        std::chrono::system_clock::time_point timestamp;
        std::set<std::string> tags;
        std::string message;
        std::string threadName;
        std::source_location location;
    };

    TaggedLogger();
    ~TaggedLogger();

    TaggedLogger(const TaggedLogger&) = delete;
    TaggedLogger& operator=(const TaggedLogger&) = delete;
    TaggedLogger(TaggedLogger&&) = delete;
    TaggedLogger& operator=(TaggedLogger&&) = delete;

    template <typename... Tags>
    auto log_impl(const std::string& message, const std::source_location& location, Tags&&... tags) -> void;

    auto setThreadName(const std::string& name) -> void;
    auto setLoggingEnabled(bool enabled) -> void;

    static std::mutex coutMutex;

private:
    std::queue<LogMessage> messageQueue;
    mutable std::mutex queueMutex;
    std::condition_variable cv;
    std::thread workerThread;
    std::atomic<bool> running;
    std::atomic<bool> loggingEnabled;
    std::set<std::string> skipTags{"Function Called", "INFO"};

    std::unordered_map<std::thread::id, std::string> threadNames;
    mutable std::mutex threadNamesMutex;
    std::atomic<int> nextThreadNumber;

    auto processQueue() -> void;
    auto writeToStderr(const LogMessage& msg) const -> void;
    auto getThreadName(const std::thread::id& id) -> std::string;
    static auto getShortPath(const char* filepath) -> std::string;
};

inline TaggedLogger& logger() {
    static TaggedLogger instance;
    return instance;
}

template <typename... Tags>
auto TaggedLogger::log_impl(const std::string& message, const std::source_location& location, Tags&&... tags) -> void {
    if (!loggingEnabled)
        return;

    const auto logMessage = LogMessage{.timestamp = std::chrono::system_clock::now(),
                                       .tags = {std::forward<Tags>(tags)...},
                                       .message = message,
                                       .threadName = getThreadName(std::this_thread::get_id()),
                                       .location = location};

    {
        std::unique_lock<std::mutex> lock(this->queueMutex);
        this->messageQueue.push(std::move(logMessage));
        this->cv.notify_one();
    }
}

#define sp_log(message, ...) logger().log_impl(message, std::source_location::current(), ##__VA_ARGS__)

inline void set_thread_name(const std::string& name) {
    logger().setThreadName(name);
}

inline void set_logging_enabled(bool enabled) {
    logger().setLoggingEnabled(enabled);
}

} // namespace SP