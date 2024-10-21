#include "TaggedLogger.hpp"

namespace SP {

std::mutex TaggedLogger::coutMutex;

TaggedLogger::TaggedLogger() : running(true), nextThreadNumber(0), loggingEnabled(true) {
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
    const auto threadId = std::this_thread::get_id();
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

auto TaggedLogger::writeToStderr(const LogMessage& msg) const -> void {
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

    std::lock_guard<std::mutex> lock(coutMutex);
    std::cerr << oss.str() << std::flush;
}

auto TaggedLogger::getThreadName(const std::thread::id& id) -> std::string {
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

} // namespace SP