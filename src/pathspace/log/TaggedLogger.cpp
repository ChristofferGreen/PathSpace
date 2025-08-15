#ifdef SP_LOG_DEBUG
#include "TaggedLogger.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <sstream>

using namespace std::string_view_literals;

namespace SP {

template <typename Range, typename Delimiter>
std::string join_with_impl(const Range& range, const Delimiter& delim) {
    std::ostringstream oss;
    bool               first = true;
    for (const auto& item : range) {
        if (!first)
            oss << delim;
        oss << item;
        first = false;
    }
    return oss.str();
}

std::mutex TaggedLogger::coutMutex;

TaggedLogger& logger() {
    static TaggedLogger instance;
    return instance;
}

TaggedLogger::TaggedLogger() : running(true), nextThreadNumber(0), loggingEnabled(false) {
    this->workerThread = std::thread(&TaggedLogger::processQueue, this);
}

TaggedLogger::~TaggedLogger() {
    {
        std::unique_lock<std::mutex> lock(this->queueMutex);
        this->running = false;
        this->cv.notify_one();
    }
    if (this->workerThread.joinable()) {
        this->workerThread.join();
    }
}

auto TaggedLogger::setThreadName(const std::string& name) -> void {
    const auto                  threadId = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(threadNamesMutex);
    threadNames[threadId] = name;
}

auto TaggedLogger::setLoggingEnabled(bool enabled) -> void {
    loggingEnabled.store(enabled, std::memory_order_relaxed);
}

auto TaggedLogger::processQueue() -> void {
    while (true) {
        std::unique_lock<std::mutex> lock(this->queueMutex);
        this->cv.wait(lock, [this] { return !this->messageQueue.empty() || !this->running; });

        if (!this->running && this->messageQueue.empty()) {
            return;
        }

        while (!this->messageQueue.empty()) {
            const auto msg = std::move(this->messageQueue.front());
            this->messageQueue.pop();
            lock.unlock();
            this->writeToStderr(msg);
            lock.lock();
        }
    }
}

inline auto SP::TaggedLogger::getShortPath(const char* filepath) -> std::string {
    namespace fs = std::filesystem;
    fs::path p{filepath};
    if (p.has_parent_path()) {
        auto parent = p.parent_path().filename();
        return (parent / p.filename()).string();
    }
    return p.filename().string();
}

inline auto SP::TaggedLogger::writeToStderr(const LogMessage& msg) const -> void {
    if (this->enabledTags.size())
        for (auto const& tag : msg.tags)
            if (!this->enabledTags.contains(tag))
                return;
    for (auto const& skipTag : this->skipTags)
        if (msg.tags.contains(skipTag))
            return;
    const auto  now      = msg.timestamp;
    const auto  nowMs    = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const auto  nowTimeT = std::chrono::system_clock::to_time_t(now);
    const auto* nowTm    = std::localtime(&nowTimeT);

    std::ostringstream oss;
    oss << std::put_time(nowTm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << nowMs.count() << ' ';

    oss << '[' << (join_with_impl(msg.tags, std::string("][")) | std::ranges::to<std::string>()) << ']' << ' ';

    oss << "[" << msg.threadName << "] ";
    oss << "[" << getShortPath(msg.location.file_name()) << ":" << msg.location.line() << "] ";
    oss << msg.message << '\n';

    std::lock_guard<std::mutex> lock(coutMutex);
    std::cerr << oss.str() << std::flush;
}

auto TaggedLogger::getThreadName(const std::thread::id& id) -> std::string {
    std::lock_guard<std::mutex> lock(threadNamesMutex);
    auto                        it = threadNames.find(id);
    if (it != threadNames.end()) {
        return it->second;
    } else {
        std::string name = "Thread " + std::to_string(nextThreadNumber++);
        threadNames[id]  = name;
        return name;
    }
}

void set_thread_name(const std::string& name) {
    logger().setThreadName(name);
}

void set_logging_enabled(bool enabled) {
    logger().setLoggingEnabled(enabled);
}

} // namespace SP
#endif // SP_LOG_DEBUG