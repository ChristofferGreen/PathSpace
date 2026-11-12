#ifdef SP_LOG_DEBUG
#include "TaggedLogger.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <sstream>
#include <cstdlib>

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

auto logger() -> TaggedLogger& { static TaggedLogger instance; return instance; }

TaggedLogger::TaggedLogger() : running(true), loggingEnabled(false), nextThreadNumber(0) {
    // Configure defaults from environment
    auto truthy = [](std::string_view v) {
        return v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON" || v == "yes" || v == "YES";
    };

    if (const char* env = std::getenv("PATHSPACE_LOG_ENABLED")) {
        if (truthy(env)) {
            loggingEnabled.store(true, std::memory_order_relaxed);
        }
    } else if (const char* env2 = std::getenv("PATHSPACE_LOG")) {
        if (truthy(env2)) {
            loggingEnabled.store(true, std::memory_order_relaxed);
        }
    }

    // Optionally clear the default skip list first
    if (const char* clearSkips = std::getenv("PATHSPACE_LOG_CLEAR_DEFAULT_SKIPS")) {
        if (truthy(clearSkips)) {
            skipTags.clear();
        }
    }

    // Helper to iterate CSV tokens and apply a functor
    auto foreach_token = [](const char* s, auto&& fn) {
        std::string current;
        for (const char* p = s; *p; ++p) {
            if (*p == ',') {
                size_t start = current.find_first_not_of(" \t");
                size_t end   = current.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    fn(current.substr(start, end - start + 1));
                }
                current.clear();
            } else {
                current.push_back(*p);
            }
        }
        if (!current.empty()) {
            size_t start = current.find_first_not_of(" \t");
            size_t end   = current.find_last_not_of(" \t");
            if (start != std::string::npos) {
                fn(current.substr(start, end - start + 1));
            }
        }
    };

    // Comma-separated list of tags to enable (restrict output to these tags)
    if (const char* en = std::getenv("PATHSPACE_LOG_ENABLE_TAGS"); en && *en) {
        enabledTags.clear();
        foreach_token(en, [this](std::string t) {
            if (!t.empty()) enabledTags.insert(std::move(t));
        });
    }

    // Comma-separated list of tags to add to the skip set
    if (const char* sk = std::getenv("PATHSPACE_LOG_SKIP_TAGS"); sk && *sk) {
        foreach_token(sk, [this](std::string t) {
            if (!t.empty()) skipTags.insert(std::move(t));
        });
    }

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

auto TaggedLogger::setEnabledTags(std::set<std::string> tags) -> void {
    std::lock_guard<std::mutex> lock(tagFilterMutex);
    enabledTags = std::move(tags);
}

auto TaggedLogger::enableTag(const std::string& tag) -> void {
    std::lock_guard<std::mutex> lock(tagFilterMutex);
    enabledTags.insert(tag);
}

auto TaggedLogger::disableTag(const std::string& tag) -> void {
    std::lock_guard<std::mutex> lock(tagFilterMutex);
    enabledTags.erase(tag);
}

auto TaggedLogger::clearEnabledTags() -> void {
    std::lock_guard<std::mutex> lock(tagFilterMutex);
    enabledTags.clear();
}

auto TaggedLogger::skipTag(const std::string& tag) -> void {
    std::lock_guard<std::mutex> lock(tagFilterMutex);
    skipTags.insert(tag);
}

auto TaggedLogger::unskipTag(const std::string& tag) -> void {
    std::lock_guard<std::mutex> lock(tagFilterMutex);
    skipTags.erase(tag);
}

auto TaggedLogger::shouldLog(const std::set<std::string>& tags) const -> bool {
    std::lock_guard<std::mutex> lock(tagFilterMutex);
    if (!enabledTags.empty()) {
        for (auto const& tag : tags)
            if (!enabledTags.contains(tag))
                return false;
    }
    for (auto const& skipTag : skipTags)
        if (tags.contains(skipTag))
            return false;
    return true;
}

auto TaggedLogger::flush() -> void {
    std::unique_lock<std::mutex> lock(queueMutex);
    idleCv.wait(lock, [this] { return messageQueue.empty() && !writing; });
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
            this->writing = true;
            lock.unlock();
            this->writeToStderr(msg);
            lock.lock();
            this->writing = false;
        }
        this->idleCv.notify_all();
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

void enable_log_tag(const std::string& tag) {
    logger().enableTag(tag);
}

void disable_log_tag(const std::string& tag) {
    logger().disableTag(tag);
}

void clear_enabled_log_tags() {
    logger().clearEnabledTags();
}

void flush_logs() {
    logger().flush();
}

} // namespace SP
#endif // SP_LOG_DEBUG
