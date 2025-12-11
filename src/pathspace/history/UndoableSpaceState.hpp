#pragma once

#include "history/UndoJournalState.hpp"
#include "history/UndoJournalPersistence.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace SP::History {

struct HistoryOptions {
    std::size_t maxEntries           = 128;
    std::size_t maxBytesRetained     = 0;
    bool        manualGarbageCollect = false;
    bool        allowNestedUndo      = false;
    bool        useMutationJournal   = false;
    bool        persistHistory       = false;
    std::string persistenceRoot;
    std::string persistenceNamespace;
    std::size_t ramCacheEntries      = 8;
    std::size_t maxDiskBytes         = 0;
    std::chrono::milliseconds keepLatestFor{0};
    bool        restoreFromPersistence = true;
    std::optional<std::string> sharedStackKey;
};

namespace UndoJournal {
class JournalFileWriter;
}

struct HistoryOperationRecord {
    std::string                           type;
    std::chrono::system_clock::time_point timestamp;
    std::chrono::milliseconds             duration{0};
    bool                                  success          = true;
    std::size_t                           undoCountBefore  = 0;
    std::size_t                           undoCountAfter   = 0;
    std::size_t                           redoCountBefore  = 0;
    std::size_t                           redoCountAfter   = 0;
    std::size_t                           bytesBefore      = 0;
    std::size_t                           bytesAfter       = 0;
    std::string                           tag;
    std::string                           message;
};

struct HistoryTelemetry {
    struct UnsupportedRecord {
        std::string                               path;
        std::string                               reason;
        std::chrono::system_clock::time_point     timestamp;
        std::size_t                               occurrences = 0;
    };

    std::size_t                                          undoBytes        = 0;
    std::size_t                                          redoBytes        = 0;
    std::size_t                                          trimOperations   = 0;
    std::size_t                                          trimmedEntries   = 0;
    std::size_t                                          trimmedBytes     = 0;
    std::optional<std::chrono::system_clock::time_point> lastTrimTimestamp;
    std::optional<HistoryOperationRecord>                lastOperation;
    std::size_t                                          diskBytes        = 0;
    std::size_t                                          diskEntries      = 0;
    std::size_t                                          cachedUndo       = 0;
    std::size_t                                          cachedRedo       = 0;
    bool                                                 persistenceDirty = false;
    std::size_t                                          unsupportedTotal = 0;
    std::vector<UnsupportedRecord>                       unsupportedLog;
};

struct UndoJournalRootState {
    std::string                               rootPath;
    std::vector<std::string>                  components;
    HistoryOptions                            options;
    UndoJournal::JournalState                 journal;
    HistoryTelemetry                          telemetry;
    std::size_t                               liveBytes = 0;
    std::uint64_t                             nextSequence = 0;
    std::string                               currentTag;
    bool                                      persistenceEnabled = false;
    std::filesystem::path                     persistencePath;
    std::filesystem::path                     journalPath;
    std::string                               encodedRoot;
    bool                                      persistenceDirty = false;
    bool                                      stateDirty       = false;
    std::unique_ptr<UndoJournal::JournalFileWriter> persistenceWriter;
    mutable std::mutex                        mutex;
    std::condition_variable                   transactionCv;
    struct TransactionState {
        std::thread::id                       owner;
        std::size_t                           depth          = 0;
        bool                                  dirty          = false;
        std::vector<UndoJournal::JournalEntry> pendingEntries;
    };
    std::optional<TransactionState>           activeTransaction;
};

} // namespace SP::History
