#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SP {

class TaggedLogger {
public:
    struct LogMessage {
        std::chrono::system_clock::time_point timestamp;
        std::vector<std::string> tags;
        std::string message;
        std::string threadName;
    };

    TaggedLogger();
    ~TaggedLogger();

    TaggedLogger(const TaggedLogger&) = delete;
    TaggedLogger& operator=(const TaggedLogger&) = delete;
    TaggedLogger(TaggedLogger&&) = delete;
    TaggedLogger& operator=(TaggedLogger&&) = delete;

    template <typename... Tags>
    auto log(const std::string& message, Tags&&... tags) -> void;

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

    std::unordered_map<std::thread::id, std::string> threadNames;
    mutable std::mutex threadNamesMutex;
    std::atomic<int> nextThreadNumber;

    auto processQueue() -> void;
    auto writeToStderr(const LogMessage& msg) const -> void;
    auto getThreadName(const std::thread::id& id) -> std::string;
};

// Inline function definitions

inline TaggedLogger& logger() {
    static TaggedLogger instance;
    return instance;
}

template <typename... Tags>
auto TaggedLogger::log(const std::string& message, Tags&&... tags) -> void {
    if (!loggingEnabled)
        return;

    const auto logMessage = LogMessage{.timestamp = std::chrono::system_clock::now(),
                                       .tags = {std::forward<Tags>(tags)...},
                                       .message = message,
                                       .threadName = getThreadName(std::this_thread::get_id())};

    {
        std::unique_lock<std::mutex> lock(this->queueMutex);
        this->messageQueue.push(std::move(logMessage));
        this->cv.notify_one();
    }
}

template <typename... Args>
inline void log(Args&&... args) {
    logger().log(std::forward<Args>(args)...);
}

inline void set_thread_name(const std::string& name) {
    logger().setThreadName(name);
}

inline void set_logging_enabled(bool enabled) {
    logger().setLoggingEnabled(enabled);
}

} // namespace SP