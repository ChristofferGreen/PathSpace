#pragma once

#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "history/UndoJournalEntry.hpp"
#include "history/UndoJournalState.hpp"
#include "history/UndoableSpaceState.hpp"
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

struct HistoryOptions;

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
    std::string  tag;
    std::string  message;
};

struct HistoryTrimMetrics {
    std::size_t operationCount   = 0;
    std::size_t entries          = 0;
    std::size_t bytes            = 0;
    std::uint64_t lastTimestampMs = 0;
};

struct HistoryLimitMetrics {
    std::size_t  maxEntries              = 0;
    std::size_t  maxBytesRetained        = 0;
    std::uint64_t keepLatestForMs        = 0;
    std::size_t  ramCacheEntries         = 0;
    std::size_t  maxDiskBytes            = 0;
    bool         persistHistory          = false;
    bool         restoreFromPersistence  = true;
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
    HistoryLimitMetrics                limits;
    std::optional<HistoryLastOperation> lastOperation;
    HistoryUnsupportedStats             unsupported;
};

struct TrimStats {
    std::size_t entriesRemoved = 0;
    std::size_t bytesRemoved   = 0;
};

using TrimPredicate = std::function<bool(std::size_t generationIndex)>;

class UndoableSpace : public PathSpaceBase {
private:

    class JournalTransactionGuard {
    public:
        JournalTransactionGuard() = default;
        JournalTransactionGuard(UndoableSpace& owner,
                                std::shared_ptr<UndoJournalRootState> state,
                                bool active);
        JournalTransactionGuard(JournalTransactionGuard&& other) noexcept = default;
        JournalTransactionGuard& operator=(JournalTransactionGuard&& other) noexcept = default;
        ~JournalTransactionGuard() = default;

        JournalTransactionGuard(JournalTransactionGuard const&)            = delete;
        JournalTransactionGuard& operator=(JournalTransactionGuard const&) = delete;

        void markDirty();
        auto commit() -> Expected<void>;
        void deactivate();
        explicit operator bool() const noexcept { return active_; }

        [[nodiscard]] auto state() const noexcept -> std::shared_ptr<UndoJournalRootState> const& {
            return state_;
        }

    private:
        UndoableSpace*                        owner_  = nullptr;
        std::shared_ptr<UndoJournalRootState> state_;
        bool                                  active_ = false;
        bool                                  dirty_  = false;
    };

    class HistoryTransaction {
    public:
        HistoryTransaction() = default;
        HistoryTransaction(HistoryTransaction&&) noexcept = default;
        HistoryTransaction& operator=(HistoryTransaction&&) noexcept = default;
        ~HistoryTransaction() = default;

        HistoryTransaction(HistoryTransaction const&)            = delete;
        HistoryTransaction& operator=(HistoryTransaction const&) = delete;

        auto commit() -> Expected<void>;
        explicit operator bool() const noexcept { return guard_.has_value(); }

    private:
        friend class UndoableSpace;
        explicit HistoryTransaction(JournalTransactionGuard&& journalGuard);

        std::optional<JournalTransactionGuard> guard_;
    };

    class JournalOperationScope {
    public:
        JournalOperationScope(UndoableSpace& owner,
                              UndoJournalRootState& state,
                              std::string_view type,
                              std::string_view tag = {})
            : owner(owner)
            , state(state)
            , type(type)
            , tag(tag)
            , startSteady(std::chrono::steady_clock::now())
            , beforeStats(state.journal.stats())
            , beforeLiveBytes(state.liveBytes)
            , beforeTelemetry(state.telemetry) {}

        void setResult(bool success, std::string message = {}) {
            succeeded   = success;
            messageText = std::move(message);
        }

        void setTag(std::string_view newTag) { tag.assign(newTag.begin(), newTag.end()); }

        ~JournalOperationScope() {
            owner.recordJournalOperation(state,
                                         type,
                                         std::chrono::steady_clock::now() - startSteady,
                                         succeeded,
                                         beforeStats,
                                         beforeLiveBytes,
                                         beforeTelemetry,
                                         state.telemetry,
                                         tag,
                                         messageText);
        }

    private:
        UndoableSpace&                  owner;
        UndoJournalRootState&           state;
        std::string                     type;
        std::string                     tag;
        std::chrono::steady_clock::time_point startSteady;
        UndoJournal::JournalState::Stats beforeStats;
        std::size_t                      beforeLiveBytes = 0;
        HistoryTelemetry                 beforeTelemetry;
        bool                             succeeded = true;
        std::string                      messageText;
    };

    struct JournalByteMetrics {
        std::size_t undoBytes = 0;
        std::size_t redoBytes = 0;
        std::size_t liveBytes = 0;
    };

public:
    explicit UndoableSpace(std::unique_ptr<PathSpaceBase> inner, HistoryOptions defaults = {});
    ~UndoableSpace() override = default;

    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override;

    auto enableHistory(SP::ConcretePathStringView root, HistoryOptions opts = {}) -> Expected<void>;
    auto disableHistory(SP::ConcretePathStringView root) -> Expected<void>;
    auto undo(SP::ConcretePathStringView root, std::size_t steps = 1) -> Expected<void>;
    auto redo(SP::ConcretePathStringView root, std::size_t steps = 1) -> Expected<void>;
    auto trimHistory(SP::ConcretePathStringView root, TrimPredicate predicate) -> Expected<TrimStats>;
    auto getHistoryStats(SP::ConcretePathStringView root) const -> Expected<HistoryStats>;
    auto exportHistorySavefile(SP::ConcretePathStringView root,
                               std::filesystem::path const& file,
                               bool fsyncData = true) -> Expected<void>;
    auto importHistorySavefile(SP::ConcretePathStringView root,
                               std::filesystem::path const& file,
                               bool applyOptions = true) -> Expected<void>;

    auto beginTransaction(SP::ConcretePathStringView root) -> Expected<HistoryTransaction>;

protected:
    // Guardrail: all mutating operations must pass through these overrides so that
    // `UndoableSpace` can journal before/after payloads. New mutators should reuse
    // the transaction helpers in UndoableSpaceHistory.cpp instead of touching the
    // inner PathSpace directly.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path,
             InputMetadata const& inputMetadata,
             Out const& options,
             void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;
    auto listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> override;

private:
    struct MatchedJournalRoot {
        std::shared_ptr<UndoJournalRootState> state;
        std::string                           key;
        std::string                           relativePath;
        bool                                  diagnostics      = false;
        bool                                  diagnosticsCompat = false;
    };

    auto findJournalRoot(SP::ConcretePathStringView root) const
        -> std::shared_ptr<UndoJournalRootState>;
    auto findJournalRootByPath(std::string const& path) const
        -> std::optional<MatchedJournalRoot>;

    auto beginJournalTransactionInternal(std::shared_ptr<UndoJournalRootState> const& state)
        -> Expected<JournalTransactionGuard>;
    auto commitJournalTransaction(UndoJournalRootState& state) -> Expected<void>;
    void markJournalTransactionDirty(UndoJournalRootState& state);

    auto recordJournalMutation(UndoJournalRootState& state,
                               UndoJournal::OperationKind operation,
                               std::string_view fullPath,
                               std::optional<NodeData> const& valueAfter,
                               std::optional<NodeData> const& inverseValue,
                               bool barrier = false) -> Expected<void>;

    auto parseJournalRelativeComponents(UndoJournalRootState const& state,
                                        std::string_view fullPath) const
        -> Expected<std::vector<std::string>>;
    auto captureJournalNodeData(UndoJournalRootState const& state,
                                std::vector<std::string> const& relativeComponents) const
        -> Expected<std::optional<NodeData>>;
    auto applyJournalNodeData(UndoJournalRootState& state,
                              std::vector<std::string> const& relativeComponents,
                              std::optional<NodeData> const& payload) -> Expected<void>;
    auto applyJournalSteps(std::shared_ptr<UndoJournalRootState> const& state,
                           std::size_t steps,
                           bool undo) -> Expected<void>;
    auto performJournalStep(UndoJournalRootState& state,
                            bool sourceIsUndo,
                            std::string_view operationName,
                            std::string_view emptyMessage) -> Expected<void>;
    auto handleJournalControlInsert(MatchedJournalRoot const& matchedRoot,
                                    std::string const& command,
                                    InputData const& data) -> InsertReturn;

    void recordJournalOperation(UndoJournalRootState& state,
                               std::string_view type,
                               std::chrono::steady_clock::duration duration,
                               bool success,
                               UndoJournal::JournalState::Stats const& beforeStats,
                               std::size_t beforeLiveBytes,
                               HistoryTelemetry const& beforeTelemetry,
                               HistoryTelemetry const& afterTelemetry,
                               std::string_view tag,
                               std::string message);
    void recordJournalUnsupportedPayload(UndoJournalRootState& state,
                                         std::string_view path,
                                         std::string_view reason);

    auto readHistoryStatsValue(HistoryStats const& stats,
                               std::optional<std::size_t> headGeneration,
                               std::string const& relativePath,
                               InputMetadata const& metadata,
                               void* obj) const -> std::optional<Error>;
    auto readDiagnosticsHistoryValue(MatchedJournalRoot const& matchedRoot,
                                     std::string const& relativePath,
                                     InputMetadata const& metadata,
                                     void* obj) -> std::optional<Error>;
    auto readJournalHistoryValue(MatchedJournalRoot const& matchedRoot,
                                 std::string const& relativePath,
                                 InputMetadata const& metadata,
                                 void* obj) -> std::optional<Error>;
    auto interpretSteps(InputData const& data) const -> std::size_t;

    auto resolveRootNode() -> Node*;

    static auto payloadBytes(NodeData const& data) -> std::size_t;
    static auto payloadBytes(std::optional<NodeData> const& data) -> std::size_t;
    void adjustLiveBytes(std::size_t& liveBytes,
                         std::size_t beforeBytes,
                         std::size_t afterBytes);

    auto computeJournalLiveBytes(UndoJournalRootState const& state) const -> std::size_t;
    auto computeJournalByteMetrics(UndoJournalRootState const& state) const
        -> Expected<JournalByteMetrics>;
    auto gatherJournalStatsLocked(UndoJournalRootState const& state) const -> HistoryStats;

    auto ensureJournalPersistenceSetup(UndoJournalRootState& state) -> Expected<void>;
    auto loadJournalPersistence(UndoJournalRootState& state) -> Expected<void>;
    auto compactJournalPersistence(UndoJournalRootState& state, bool fsync) -> Expected<void>;
    void updateJournalDiskTelemetry(UndoJournalRootState& state);
    auto encodeRootForPersistence(std::string const& rootPath) const -> std::string;
    auto persistenceRootPath(HistoryOptions const& opts) const -> std::filesystem::path;
    auto defaultPersistenceRoot() const -> std::filesystem::path;

    HistoryOptions defaultOptions;
    std::unique_ptr<PathSpaceBase> inner;
    mutable std::mutex rootsMutex;
    std::unordered_map<std::string, std::shared_ptr<UndoJournalRootState>> journalRoots;
    std::unordered_map<std::string, std::shared_ptr<UndoJournalRootState>> diagnosticsRoots;
    std::string spaceUuid;
};

} // namespace SP::History
