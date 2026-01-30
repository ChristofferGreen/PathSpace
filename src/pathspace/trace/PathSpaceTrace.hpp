#pragma once

#include "task/TaskPool.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace SP::PathSpaceTrace {

enum class Op {
    Read,
    Insert,
    Take,
};

struct ThreadTotals {
    uint64_t groupId = 0;
    uint64_t readUs = 0;
    uint64_t insertUs = 0;
    uint64_t takeUs = 0;
};

inline std::mutex gMutex;
inline std::unordered_map<uint64_t, ThreadTotals> gTotals;
inline std::atomic<uint64_t> gGroupId{0};
inline thread_local int gDepth = 0;

inline auto current_thread_id() -> uint64_t {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid = 0;
    if (pthread_threadid_np(nullptr, &tid) == 0 && tid != 0) {
        return tid;
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#elif defined(__linux__)
    return static_cast<uint64_t>(syscall(SYS_gettid));
#else
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

struct ScopedOp {
    explicit ScopedOp(Op opIn) : op(opIn) {
        auto &pool = TaskPool::Instance();
        startUs = pool.traceNowUs();
        if (startUs == 0) return;
        groupId = gGroupId.load(std::memory_order_acquire);
        if (groupId == 0) {
            startUs = 0;
            return;
        }
        depthPtr = &gDepth;
        ++(*depthPtr);
        if (*depthPtr != 1) {
            startUs = 0;
            return;
        }
        threadId = current_thread_id();
    }

    ~ScopedOp() {
        if (depthPtr) {
            --(*depthPtr);
        }
        if (startUs == 0) return;
        auto &pool = TaskPool::Instance();
        uint64_t endUs = pool.traceNowUs();
        if (endUs <= startUs) return;
        uint64_t durUs = endUs - startUs;
        std::lock_guard<std::mutex> lock(gMutex);
        auto &stats = gTotals[threadId];
        if (stats.groupId != groupId) {
            stats = {};
            stats.groupId = groupId;
        }
        switch (op) {
            case Op::Read:
                stats.readUs += durUs;
                break;
            case Op::Insert:
                stats.insertUs += durUs;
                break;
            case Op::Take:
                stats.takeUs += durUs;
                break;
        }
    }

private:
    Op op;
    uint64_t startUs = 0;
    uint64_t groupId = 0;
    uint64_t threadId = 0;
    int* depthPtr = nullptr;
};

inline void BeginGroup(uint64_t groupId) {
    gGroupId.store(groupId, std::memory_order_release);
}

inline void EndGroup(TaskPool& pool, uint64_t groupId, uint64_t groupStartUs, uint64_t groupEndUs) {
    gGroupId.store(0, std::memory_order_release);
    if (groupStartUs == 0 || groupEndUs <= groupStartUs) return;

    std::vector<std::pair<uint64_t, ThreadTotals>> snapshot;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto &entry : gTotals) {
            if (entry.second.groupId != groupId) continue;
            snapshot.emplace_back(entry.first, entry.second);
            entry.second = {};
        }
    }

    if (snapshot.empty()) return;
    uint64_t frameDur = groupEndUs - groupStartUs;
    for (auto const& [threadId, stats] : snapshot) {
        uint64_t total = stats.readUs + stats.insertUs + stats.takeUs;
        if (total == 0) continue;
        double scale = 1.0;
        if (frameDur > 0 && total > frameDur) {
            scale = static_cast<double>(frameDur) / static_cast<double>(total);
        }
        auto scaled = [&](uint64_t value) -> uint64_t {
            if (value == 0) return 0;
            double v = static_cast<double>(value) * scale;
            if (v <= 0.0) return 0;
            return static_cast<uint64_t>(v);
        };

        uint64_t totalScaled = scaled(total);
        if (totalScaled == 0) continue;
        uint64_t cursor = groupStartUs;
        uint64_t remaining = totalScaled;
        pool.traceSpan("PathSpace", "pathspace", {}, cursor, totalScaled, threadId);
        auto emit_child = [&](char const* name, char const* category, uint64_t valueUs) {
            if (valueUs == 0 || remaining == 0) return;
            uint64_t useDur = std::min<uint64_t>(valueUs, remaining);
            pool.traceSpan(std::string(name), std::string(category), {}, cursor, useDur, threadId);
            cursor += useDur;
            remaining -= useDur;
        };
        emit_child("read", "pathspace.read", scaled(stats.readUs));
        emit_child("insert", "pathspace.insert", scaled(stats.insertUs));
        emit_child("take", "pathspace.take", scaled(stats.takeUs));
    }
}

} // namespace SP::PathSpaceTrace
