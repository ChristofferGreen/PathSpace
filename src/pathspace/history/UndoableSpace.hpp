#pragma once

#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "history/CowSubtreePrototype.hpp"
#include "path/ConcretePath.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SP {
class PathSpace;
struct Node;
} // namespace SP

namespace SP::History {

struct HistoryOptions {
    std::size_t maxEntries           = 128;
    std::size_t maxBytesRetained     = 0;
    bool        manualGarbageCollect = false;
    bool        allowNestedUndo      = false;
    bool        persistHistory       = false;
    std::string persistenceRoot;
    std::string persistenceNamespace;
    std::size_t ramCacheEntries      = 8;
    std::size_t maxDiskBytes         = 0;
    std::chrono::milliseconds keepLatestFor{0};
    bool        restoreFromPersistence = true;
};

struct HistoryLastOperation {
    std::string type;
    std::uint64_t timestampMs      = 0;
    std::uint64_t durationMs       = 0;
    bool         success           = true;
    std::size_t  undoCountBefore   = 0;
    std::size_t  undoCountAfter    = 0;
    std::size_t  redoCountBefore   = 0;
    std::size_t  redoCountAfter    = 0;
    std::size_t  bytesBefore       = 0;
    std::size_t  bytesAfter        = 0;
    std::string  message;
};

struct HistoryTrimMetrics {
    std::size_t operationCount   = 0;
    std::size_t entries          = 0;
    std::size_t bytes            = 0;
    std::uint64_t lastTimestampMs = 0;
};

struct HistoryBytes {
    std::size_t total = 0;
    std::size_t undo  = 0;
    std::size_t redo  = 0;
    std::size_t live  = 0;
    std::size_t disk  = 0;
};

struct HistoryCounts {
    std::size_t undo                 = 0;
    std::size_t redo                 = 0;
    bool        manualGarbageCollect = false;
    std::size_t diskEntries          = 0;
    std::size_t cachedUndo           = 0;
    std::size_t cachedRedo           = 0;
};

struct HistoryUnsupportedRecord {
    std::string path;
    std::string reason;
    std::size_t occurrences      = 0;
    std::uint64_t lastTimestampMs = 0;
};

struct HistoryUnsupportedStats {
    std::size_t                           total    = 0;
    std::vector<HistoryUnsupportedRecord> recent;
};

struct HistoryStats {
    HistoryCounts                      counts;
    HistoryBytes                       bytes;
    HistoryTrimMetrics                 trim;
    std::optional<HistoryLastOperation> lastOperation;
    HistoryUnsupportedStats             unsupported;
};

struct TrimStats {
    std::size_t entriesRemoved = 0;
    std::size_t bytesRemoved   = 0;
};

using TrimPredicate = std::function<bool(std::size_t generationIndex)>;

class UndoableSpace : public PathSpaceBase {
public:
    explicit UndoableSpace(std::unique_ptr<PathSpaceBase> inner, HistoryOptions defaults = {});
    ~UndoableSpace() override = default;

    auto enableHistory(SP::ConcretePathStringView root, HistoryOptions opts = {}) -> Expected<void>;
    auto disableHistory(SP::ConcretePathStringView root) -> Expected<void>;
    auto undo(SP::ConcretePathStringView root, std::size_t steps = 1) -> Expected<void>;
    auto redo(SP::ConcretePathStringView root, std::size_t steps = 1) -> Expected<void>;
    auto trimHistory(SP::ConcretePathStringView root, TrimPredicate predicate) -> Expected<TrimStats>;
    auto getHistoryStats(SP::ConcretePathStringView root) const -> Expected<HistoryStats>;

    struct RootState;

    class HistoryTransaction {
    public:
        HistoryTransaction(HistoryTransaction&&) noexcept;
        HistoryTransaction& operator=(HistoryTransaction&&) noexcept;
        ~HistoryTransaction();

        HistoryTransaction(HistoryTransaction const&)            = delete;
        HistoryTransaction& operator=(HistoryTransaction const&) = delete;

        auto commit() -> Expected<void>;

    private:
        friend class UndoableSpace;
        HistoryTransaction(UndoableSpace& owner,
                           std::shared_ptr<RootState> rootState);

        UndoableSpace*                    owner;
        std::shared_ptr<RootState>        rootState;
        bool                              active = true;
    };

    auto beginTransaction(SP::ConcretePathStringView root) -> Expected<HistoryTransaction>;

protected:
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;

private:
    struct MatchedRoot {
        std::shared_ptr<RootState> state;
        std::string                key;
        std::string                relativePath;
    };

    class OperationScope;
    class TransactionGuard {
    public:
        TransactionGuard() = default;
        TransactionGuard(UndoableSpace& owner, std::shared_ptr<RootState> state, bool active);
        TransactionGuard(TransactionGuard&& other) noexcept;
        TransactionGuard& operator=(TransactionGuard&& other) noexcept;
        ~TransactionGuard();

        TransactionGuard(TransactionGuard const&)            = delete;
        TransactionGuard& operator=(TransactionGuard const&) = delete;

        void markDirty();
        auto commit() -> Expected<void>;
        void deactivate();
        explicit operator bool() const { return active; }

    private:
        void release();

        UndoableSpace*              owner  = nullptr;
        std::shared_ptr<RootState>  state;
        bool                        active = false;
    };

    auto findRoot(SP::ConcretePathStringView root) const -> std::shared_ptr<RootState>;
    auto findRootByPath(std::string const& path) const -> std::optional<MatchedRoot>;
    auto beginTransactionInternal(std::shared_ptr<RootState> const& state) -> Expected<TransactionGuard>;
    auto commitTransaction(RootState& state) -> Expected<void>;
    auto markTransactionDirty(RootState& state) -> void;

    auto captureSnapshotLocked(RootState& state) -> Expected<CowSubtreePrototype::Snapshot>;
    auto applySnapshotLocked(RootState& state, CowSubtreePrototype::Snapshot const& snapshot) -> Expected<void>;
    auto clearSubtree(Node& node) -> void;

    auto handleControlInsert(MatchedRoot const& matchedRoot, std::string const& command, InputData const& data) -> InsertReturn;
    auto interpretSteps(InputData const& data) const -> std::size_t;

    auto resolveRootNode() -> Node*;
    static auto computeTotalBytesLocked(RootState const& state) -> std::size_t;
    auto gatherStatsLocked(RootState const& state) const -> HistoryStats;
    void recordOperation(RootState& state,
                         std::string_view type,
                         std::chrono::steady_clock::duration duration,
                         bool success,
                         std::size_t undoBefore,
                         std::size_t redoBefore,
                         std::size_t bytesBefore,
                         std::string const& message);
    auto applyRetentionLocked(RootState& state, std::string_view origin) -> TrimStats;
    auto readHistoryValue(MatchedRoot const& matchedRoot,
                          std::string const& relativePath,
                          InputMetadata const& metadata,
                          void* obj) -> std::optional<Error>;
    void recordUnsupportedPayloadLocked(RootState& state,
                                        std::string const& path,
                                        std::string const& reason);
    auto ensurePersistenceSetup(RootState& state) -> Expected<void>;
    auto loadPersistentState(RootState& state) -> Expected<void>;
    auto restoreRootFromPersistence(RootState& state) -> Expected<void>;
    auto persistStacksLocked(RootState& state, bool forceFsync) -> Expected<void>;
    auto loadEntrySnapshotLocked(RootState& state, std::size_t stackIndex, bool undoStack) -> Expected<void>;
    auto applyRamCachePolicyLocked(RootState& state) -> void;
    auto updateCacheTelemetryLocked(RootState& state) -> void;
    auto updateDiskTelemetryLocked(RootState& state) -> void;
    auto encodeRootForPersistence(std::string const& rootPath) const -> std::string;
    auto persistenceRootPath(HistoryOptions const& opts) const -> std::filesystem::path;
    auto defaultPersistenceRoot() const -> std::filesystem::path;
    auto entrySnapshotPath(RootState const& state, std::size_t generation) const -> std::filesystem::path;
    auto entryMetaPath(RootState const& state, std::size_t generation) const -> std::filesystem::path;
    auto stateMetaPath(RootState const& state) const -> std::filesystem::path;
    auto removeEntryFiles(RootState& state, std::size_t generation) -> void;

    std::unique_ptr<PathSpaceBase>                     inner;
    HistoryOptions                                     defaultOptions;
    mutable std::mutex                                 rootsMutex;
    std::string                                        spaceUuid;
    std::unordered_map<std::string, std::shared_ptr<RootState>> roots;
};

} // namespace SP::History
