#pragma once

/**
 * @file TaggedLogger.hpp
 * @brief A header-only, thread-safe logging library with tag-based filtering and test discovery compatibility.
 *
 * Design Overview:
 * ----------------
 * This logging library provides a flexible, efficient, and thread-safe logging solution.
 * Key features include:
 *
 * 1. Tag-based logging for flexible categorization of log messages.
 * 2. Thread-safe operation using a background thread and message queue.
 * 3. Millisecond precision timestamps for each log entry.
 * 4. Conditional compilation for zero overhead in release builds.
 * 5. Header-only design for easy integration into existing projects.
 * 6. Thread naming support for better multi-threading debugging.
 * 7. Ability to temporarily disable logging for test discovery compatibility.
 *
 * Usage:
 * ------
 * 1. Include this header in your source code.
 *
 * 2. To enable logging, define SP_LOG_DEBUG before including this header:
 *    #define SP_LOG_DEBUG
 *    #include "TaggedLogger.hpp"
 *
 * 3. Use SP::log() for logging:
 *    SP::log("Your message here", "TAG1", "TAG2");
 *
 * 4. For formatted logging, use your preferred formatting method (e.g., std::format):
 *    SP::log(std::format("User {} logged in", username), "INFO", "AUTH");
 *
 * 5. To name a thread, use SP::setThreadName():
 *    SP::setThreadName("WorkerThread");
 *
 * 6. To disable logging (e.g., in release builds), simply don't define SP_LOG_DEBUG.
 *
 * 7. To temporarily disable logging (e.g., during test discovery):
 *    SP::setLoggingEnabled(false);
 *    // ... test discovery or other operations ...
 *    SP::setLoggingEnabled(true);
 *
 * Example:
 * --------
 * #define SP_LOG_DEBUG
 * #include "TaggedLogger.hpp"
 * #include <format>
 *
 * int main() {
 *     SP::setThreadName("MainThread");
 *     SP::log("Application started", "INFO", "STARTUP");
 *
 *     int user_count = 42;
 *     SP::log(std::format("Current user count: {}", user_count), "INFO", "USERS");
 *
 *     std::thread worker([]() {
 *         SP::setThreadName("Worker");
 *         SP::log("Worker thread started", "INFO", "WORKER");
 *     });
 *
 *     worker.join();
 *     SP::log("Application shutting down", "INFO", "SHUTDOWN");
 *     return 0;
 * }
 */

#ifdef SP_LOG_DEBUG

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

    TaggedLogger() : running(true), nextThreadNumber(0), loggingEnabled(true) {
        this->workerThread = std::thread(&TaggedLogger::processQueue, this);
    }

    ~TaggedLogger() {
        this->running = false;
        this->cv.notify_one();
        if (this->workerThread.joinable()) {
            this->workerThread.join();
        }
    }

    template <typename... Tags>
    auto log(const std::string_view message, Tags&&... tags) -> void {
        if (!loggingEnabled)
            return;

        const auto logMessage = LogMessage{.timestamp = std::chrono::system_clock::now(),
                                           .tags = {std::forward<Tags>(tags)...},
                                           .message = std::string(message),
                                           .threadName = getThreadName(std::this_thread::get_id())};

        {
            const std::lock_guard<std::mutex> lock(this->queueMutex);
            this->messageQueue.push(std::move(logMessage));
        }
        this->cv.notify_one();
    }

    auto setThreadName(const std::string& name) -> void {
        const auto threadId = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(threadNamesMutex);
        threadNames[threadId] = name;
    }

    auto setLoggingEnabled(bool enabled) -> void {
        loggingEnabled.store(enabled, std::memory_order_relaxed);
    }

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

    auto processQueue() -> void {
        while (this->running) {
            std::unique_lock<std::mutex> lock(this->queueMutex);
            this->cv.wait(lock, [this] { return !this->messageQueue.empty() || !this->running; });

            while (!this->messageQueue.empty()) {
                const auto& msg = this->messageQueue.front();
                this->writeToStderr(msg);
                this->messageQueue.pop();
            }
        }
    }

    auto writeToStderr(const LogMessage& msg) const -> void {
        const auto now = msg.timestamp;
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        const auto nowTimeT = std::chrono::system_clock::to_time_t(now);
        const auto* nowTm = std::localtime(&nowTimeT);

        std::ostringstream oss;
        oss << std::put_time(nowTm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << nowMs.count() << ' ';

        oss << '[';
        for (size_t i = 0; i < msg.tags.size(); ++i) {
            if (i > 0)
                oss << "][";
            oss << msg.tags[i];
        }
        oss << "] ";

        oss << "[" << msg.threadName << "] ";
        oss << msg.message << '\n';

        std::cerr << oss.str();
    }

    auto getThreadName(const std::thread::id& id) -> std::string {
        std::lock_guard<std::mutex> lock(threadNamesMutex);
        auto it = threadNames.find(id);
        if (it != threadNames.end()) {
            return it->second;
        } else {
            std::string name = "Thread " + std::to_string(nextThreadNumber++);
            threadNames[id] = name;
            return name;
        }
    }
};

inline auto logger() -> TaggedLogger& {
    static TaggedLogger logger;
    return logger;
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

#else // SP_LOG_DEBUG not defined

namespace SP {

template <typename... Args>
inline void log(Args&&...) {
}

inline void setThreadName(const std::string&) {
}

inline void setLoggingEnabled(bool) {
}

} // namespace SP

#endif // SP_LOG_DEBUG