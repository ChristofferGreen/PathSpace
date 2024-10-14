#pragma once

/**
 * @file TaggedLogger.hpp
 * @brief A header-only, thread-safe logging library with tag-based filtering.
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
 *
 * Usage:
 * ------
 * 1. Include this header in your source code.
 * 2. Use SP::log() for simple logging:
 *    SP::log("Your message here", "TAG1", "TAG2");
 *
 * 3. Use SP::logf() for formatted logging:
 *    SP::logf("Formatted message: ", some_variable, " ", another_variable, "TAG1", "TAG2");
 *
 * 4. To enable logging, define SP_LOG_DEBUG before including this header:
 *    #define SP_LOG_DEBUG
 *    #include "TaggedLogger.hpp"
 *
 * 5. To disable logging (e.g., in release builds), simply don't define SP_LOG_DEBUG.
 *
 * Example:
 * --------
 * #define SP_LOG_DEBUG
 * #include "TaggedLogger.hpp"
 *
 * int main() {
 *     SP::log("Application started", "INFO", "STARTUP");
 *
 *     int user_count = 42;
 *     SP::logf("Current user count: ", user_count, "INFO", "USERS");
 *
 *     try {
 *         // Some operation that might throw
 *     } catch (const std::exception& e) {
 *         SP::logf("Error occurred: ", e.what(), "ERROR", "EXCEPTION");
 *     }
 *
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
#include <vector>
#endif // SP_LOG_DEBUG
namespace SP {
#ifdef SP_LOG_DEBUG

class TaggedLogger {
public:
    struct LogMessage {
        std::chrono::system_clock::time_point timestamp;
        std::vector<std::string> tags;
        std::string message;
    };

    TaggedLogger() : running(true) {
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
        const auto logMessage = LogMessage{.timestamp = std::chrono::system_clock::now(),
                                           .tags = {std::forward<Tags>(tags)...},
                                           .message = std::string(message)};

        {
            const std::lock_guard<std::mutex> lock(this->queueMutex);
            this->messageQueue.push(std::move(logMessage));
        }
        this->cv.notify_one();
    }

    template <typename... Args>
    auto logf(Args&&... args) -> void {
        std::ostringstream oss;
        (oss << ... << std::forward<Args>(args));
        this->log(oss.str());
    }

private:
    std::queue<LogMessage> messageQueue;
    mutable std::mutex queueMutex;
    std::condition_variable cv;
    std::thread workerThread;
    std::atomic<bool> running;

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
        oss << "] " << msg.message << '\n';

        std::cerr << oss.str();
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

template <typename... Args>
inline void logf(Args&&... args) {
    logger().logf(std::forward<Args>(args)...);
}
#else
template <typename... Args>
inline void log(Args&&...) {
}

template <typename... Args>
inline void logf(Args&&...) {
}
#endif // SP_LOG_DEBUG

} // namespace SP