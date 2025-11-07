#pragma once

#include "core/Error.hpp"
#include "history/CowSubtreePrototype.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace SP {
class PathSpace;
struct Node;
} // namespace SP

namespace SP::History {

class UndoableSpace;
struct HistoryOptions;

struct UndoableSpace::RootState {
    struct Entry {
        CowSubtreePrototype::Snapshot             snapshot;
        std::size_t                               bytes     = 0;
        std::chrono::system_clock::time_point     timestamp = std::chrono::system_clock::now();
        bool                                      persisted = false;
        bool                                      cached    = true;
    };

    struct OperationRecord {
        std::string                               type;
        std::chrono::system_clock::time_point     timestamp;
        std::chrono::milliseconds                 duration{0};
        bool                                      success          = true;
        std::size_t                               undoCountBefore  = 0;
        std::size_t                               undoCountAfter   = 0;
        std::size_t                               redoCountBefore  = 0;
        std::size_t                               redoCountAfter   = 0;
        std::size_t                               bytesBefore      = 0;
        std::size_t                               bytesAfter       = 0;
        std::string                               message;
    };

    struct Telemetry {
        struct UnsupportedRecord {
            std::string                               path;
            std::string                               reason;
            std::chrono::system_clock::time_point     timestamp;
            std::size_t                               occurrences = 0;
        };
        struct ExecutionOptOutRecord {
            std::string                               path;
            std::chrono::system_clock::time_point     timestamp;
            std::size_t                               occurrences = 0;
        };

        std::size_t                                               undoBytes        = 0;
        std::size_t                                               redoBytes        = 0;
        std::size_t                                               trimOperations   = 0;
        std::size_t                                               trimmedEntries   = 0;
        std::size_t                                               trimmedBytes     = 0;
        std::optional<std::chrono::system_clock::time_point>      lastTrimTimestamp;
        std::optional<OperationRecord>                            lastOperation;
        std::size_t                                               diskBytes        = 0;
        std::size_t                                               diskEntries      = 0;
        std::size_t                                               cachedUndo       = 0;
        std::size_t                                               cachedRedo       = 0;
        bool                                                      persistenceDirty = false;
        std::size_t                                               unsupportedTotal = 0;
        std::vector<UnsupportedRecord>                            unsupportedLog;
        std::size_t                                               executionOptOutTotal = 0;
        std::vector<ExecutionOptOutRecord>                        executionOptOutLog;
    };

    struct TransactionState {
        std::thread::id                    owner;
        std::size_t                        depth          = 0;
        bool                               dirty          = false;
        CowSubtreePrototype::Snapshot      snapshotBefore;
    };

    std::string                               rootPath;
    std::vector<std::string>                  components;
    HistoryOptions                            options;
    CowSubtreePrototype                       prototype;
    CowSubtreePrototype::Snapshot             liveSnapshot;
    std::vector<Entry>                        undoStack;
    std::vector<Entry>                        redoStack;
    std::size_t                               liveBytes = 0;
    std::vector<std::string>                  executionOptOutPrefixes;
    Telemetry                                 telemetry;
    std::optional<TransactionState>           activeTransaction;
    mutable std::mutex                        mutex;
    bool                                      persistenceEnabled = false;
    std::filesystem::path                     persistencePath;
    std::filesystem::path                     entriesPath;
    std::string                               encodedRoot;
    bool                                      stateDirty        = false;
    bool                                      hasPersistentState = false;
};

class UndoableSpace::OperationScope {
public:
    OperationScope(UndoableSpace& owner, RootState& state, std::string_view type);
    void setResult(bool success, std::string message = {});
    ~OperationScope();

private:
    UndoableSpace&                         owner;
    RootState&                             state;
    std::string                            type;
    std::chrono::steady_clock::time_point  startSteady;
    std::size_t                            undoBefore;
    std::size_t                            redoBefore;
    std::size_t                            bytesBefore;
    bool                                   succeeded = true;
    std::string                            messageText;
};

namespace Detail {
template <typename State, typename Fn>
inline void forEachHistoryStack(State& state, Fn&& fn) {
    fn(state.undoStack, true);
    fn(state.redoStack, false);
}
} // namespace Detail

} // namespace SP::History
