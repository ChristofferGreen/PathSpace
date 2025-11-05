#include "history/UndoableSpace.hpp"

#include "PathSpace.hpp"
#include "core/InsertReturn.hpp"
#include "core/Node.hpp"
#include "core/NodeData.hpp"
#include "log/TaggedLogger.hpp"
#include <cstring>
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <thread>
#include <utility>

namespace {

using SP::History::CowSubtreePrototype;

auto normalizePath(std::string path) -> std::string {
    if (path.empty())
        return "/";
    if (path.back() == '/' && path.size() > 1)
        path.pop_back();
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    return path;
}

auto splitComponents(std::string const& path) -> std::vector<std::string> {
    auto parsed = CowSubtreePrototype::parsePath(path);
    if (!parsed.has_value()) {
        return {};
    }
    return *parsed;
}

auto pathIsPrefix(std::string const& prefix, std::string const& path) -> bool {
    if (prefix == "/")
        return true;
    if (!path.starts_with(prefix))
        return false;
    if (path.size() == prefix.size())
        return true;
    return path[prefix.size()] == '/';
}

auto toMillis(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
    return static_cast<std::uint64_t>(duration.count());
}

} // namespace

namespace SP::History {

using SP::ConcretePathStringView;

struct UndoableSpace::RootState {
    struct Entry {
        CowSubtreePrototype::Snapshot             snapshot;
        std::size_t                               bytes     = 0;
        std::chrono::system_clock::time_point     timestamp = std::chrono::system_clock::now();
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
        std::size_t                                               undoBytes        = 0;
        std::size_t                                               redoBytes        = 0;
        std::size_t                                               trimOperations   = 0;
        std::size_t                                               trimmedEntries   = 0;
        std::size_t                                               trimmedBytes     = 0;
        std::optional<std::chrono::system_clock::time_point>      lastTrimTimestamp;
        std::optional<OperationRecord>                            lastOperation;
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
    Telemetry                                 telemetry;
    std::optional<TransactionState>           activeTransaction;
    mutable std::mutex                        mutex;
};

class UndoableSpace::OperationScope {
public:
    OperationScope(UndoableSpace& owner, RootState& state, std::string_view type)
        : owner_(owner)
        , state_(state)
        , type_(type)
        , startSteady_(std::chrono::steady_clock::now())
        , undoBefore_(state.undoStack.size())
        , redoBefore_(state.redoStack.size())
        , bytesBefore_(UndoableSpace::computeTotalBytesLocked(state)) {}

    void setResult(bool success, std::string message = {}) {
        success_ = success;
        message_ = std::move(message);
    }

    ~OperationScope() {
        owner_.recordOperation(state_, type_, std::chrono::steady_clock::now() - startSteady_,
                               success_, undoBefore_, redoBefore_, bytesBefore_, message_);
    }

private:
    UndoableSpace&                         owner_;
    RootState&                             state_;
    std::string                            type_;
    std::chrono::steady_clock::time_point  startSteady_;
    std::size_t                            undoBefore_;
    std::size_t                            redoBefore_;
    std::size_t                            bytesBefore_;
    bool                                   success_ = true;
    std::string                            message_;
};

UndoableSpace::TransactionGuard::TransactionGuard(UndoableSpace& owner,
                                                  std::shared_ptr<RootState> state,
                                                  bool active)
    : owner_(&owner)
    , state_(std::move(state))
    , active_(active) {}

UndoableSpace::TransactionGuard::TransactionGuard(TransactionGuard&& other) noexcept
    : owner_(other.owner_)
    , state_(std::move(other.state_))
    , active_(other.active_) {
    other.owner_  = nullptr;
    other.active_ = false;
}

UndoableSpace::TransactionGuard&
UndoableSpace::TransactionGuard::operator=(TransactionGuard&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    owner_        = other.owner_;
    state_        = std::move(other.state_);
    active_       = other.active_;
    other.owner_  = nullptr;
    other.active_ = false;
    return *this;
}

UndoableSpace::TransactionGuard::~TransactionGuard() {
    release();
}

void UndoableSpace::TransactionGuard::release() {
    if (active_ && owner_ && state_) {
        auto const result = owner_->commitTransaction(*state_);
        if (!result) {
            sp_log("UndoableSpace::TransactionGuard commit failed during destruction: "
                       + result.error().message.value_or("unknown"),
                   "UndoableSpace");
        }
    }
    active_ = false;
}

void UndoableSpace::TransactionGuard::markDirty() {
    if (!active_ || !owner_ || !state_)
        return;
    owner_->markTransactionDirty(*state_);
}

auto UndoableSpace::TransactionGuard::commit() -> Expected<void> {
    if (!active_ || !owner_ || !state_) {
        active_ = false;
        return {};
    }
    active_ = false;
    return owner_->commitTransaction(*state_);
}

void UndoableSpace::TransactionGuard::deactivate() {
    active_ = false;
}

UndoableSpace::HistoryTransaction::HistoryTransaction(UndoableSpace& owner,
                                                      std::shared_ptr<RootState> state)
    : owner_(&owner)
    , rootState_(std::move(state)) {}

UndoableSpace::HistoryTransaction::HistoryTransaction(HistoryTransaction&& other) noexcept
    : owner_(other.owner_)
    , rootState_(std::move(other.rootState_))
    , active_(other.active_) {
    other.owner_  = nullptr;
    other.active_ = false;
}

UndoableSpace::HistoryTransaction&
UndoableSpace::HistoryTransaction::operator=(HistoryTransaction&& other) noexcept {
    if (this == &other)
        return *this;
    commit();
    owner_        = other.owner_;
    rootState_    = std::move(other.rootState_);
    active_       = other.active_;
    other.owner_  = nullptr;
    other.active_ = false;
    return *this;
}

UndoableSpace::HistoryTransaction::~HistoryTransaction() {
    if (active_ && owner_ && rootState_) {
        auto const result = owner_->commitTransaction(*rootState_);
        if (!result) {
            sp_log("UndoableSpace::HistoryTransaction auto-commit failed: "
                       + result.error().message.value_or("unknown"),
                   "UndoableSpace");
        }
    }
}

auto UndoableSpace::HistoryTransaction::commit() -> Expected<void> {
    if (!active_ || !owner_ || !rootState_) {
        active_ = false;
        return {};
    }
    active_ = false;
    return owner_->commitTransaction(*rootState_);
}

UndoableSpace::UndoableSpace(std::unique_ptr<PathSpaceBase> inner, HistoryOptions defaults)
    : inner_(std::move(inner))
    , defaultOptions_(defaults) {}

auto UndoableSpace::resolveRootNode() -> Node* {
    if (!inner_)
        return nullptr;
    return inner_->getRootNode();
}

auto UndoableSpace::enableHistory(ConcretePathStringView root, HistoryOptions opts) -> Expected<void> {
    auto normalized = normalizePath(std::string(root.getPath()));
    auto components = splitComponents(normalized);
    if (components.empty() && normalized != "/") {
        return std::unexpected(Error{Error::Code::InvalidPath, "History root must be a concrete path"});
    }

    {
        std::scoped_lock lock(rootsMutex_);
        if (roots_.find(normalized) != roots_.end()) {
            return std::unexpected(Error{Error::Code::UnknownError, "History already enabled for path"});
        }
        if (!defaultOptions_.allowNestedUndo || !opts.allowNestedUndo) {
            for (auto const& [existing, _] : roots_) {
                if (pathIsPrefix(existing, normalized) || pathIsPrefix(normalized, existing)) {
                    return std::unexpected(Error{
                        Error::Code::InvalidPermissions,
                        "History roots may not be nested without allowNestedUndo"});
                }
            }
        }
    }

    if (auto* rootNode = resolveRootNode(); !rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "UndoableSpace requires PathSpace backend"});
    }

    auto state                = std::make_shared<RootState>();
    state->rootPath           = normalized;
    state->components         = std::move(components);
    state->options            = defaultOptions_;
    state->options.maxEntries = opts.maxEntries ? opts.maxEntries : state->options.maxEntries;
    state->options.maxBytesRetained =
        opts.maxBytesRetained ? opts.maxBytesRetained : state->options.maxBytesRetained;
    state->options.manualGarbageCollect = opts.manualGarbageCollect;
    state->options.allowNestedUndo      = opts.allowNestedUndo;
    state->undoStack.clear();
    state->redoStack.clear();
    state->telemetry = {};

    {
        std::scoped_lock rootLock(state->mutex);
        auto snapshot = captureSnapshotLocked(*state);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        state->liveSnapshot = snapshot.value();
        auto metrics        = state->prototype.analyze(state->liveSnapshot);
        state->liveBytes    = metrics.payloadBytes;
    }

    {
        std::scoped_lock lock(rootsMutex_);
        roots_.emplace(state->rootPath, std::move(state));
    }

    return {};
}

auto UndoableSpace::disableHistory(ConcretePathStringView root) -> Expected<void> {
    auto normalized = normalizePath(std::string(root.getPath()));
    std::scoped_lock lock(rootsMutex_);
    if (roots_.erase(normalized) == 0) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }
    return {};
}

auto UndoableSpace::findRoot(ConcretePathStringView root) const -> std::shared_ptr<RootState> {
    auto normalized = normalizePath(std::string(root.getPath()));
    std::scoped_lock lock(rootsMutex_);
    auto it = roots_.find(normalized);
    if (it == roots_.end())
        return {};
    return it->second;
}

auto UndoableSpace::findRootByPath(std::string const& path) const -> std::optional<MatchedRoot> {
    std::string bestKey;
    std::shared_ptr<RootState> bestState;

    {
        std::scoped_lock lock(rootsMutex_);
        for (auto const& [rootPath, state] : roots_) {
            if (pathIsPrefix(rootPath, path)) {
                if (rootPath.size() > bestKey.size()) {
                    bestKey   = rootPath;
                    bestState = state;
                }
            }
        }
    }

    if (!bestState)
        return std::nullopt;

    std::string relative;
    if (path.size() > bestKey.size()) {
        relative = path.substr(bestKey.size() + (bestKey == "/" ? 0 : 1));
    }
    return MatchedRoot{std::move(bestState), std::move(bestKey), std::move(relative)};
}

auto UndoableSpace::beginTransactionInternal(std::shared_ptr<RootState> const& state)
    -> Expected<TransactionGuard> {
    if (!state)
        return std::unexpected(Error{Error::Code::UnknownError, "History root missing"});

    std::unique_lock lock(state->mutex);
    auto const currentThread = std::this_thread::get_id();
    if (state->activeTransaction.has_value()) {
        auto& tx = *state->activeTransaction;
        if (tx.owner != currentThread) {
            return std::unexpected(Error{Error::Code::InvalidPermissions,
                                         "History transaction already active on another thread"});
        }
        tx.depth += 1;
    } else {
        state->activeTransaction = RootState::TransactionState{
            .owner          = currentThread,
            .depth          = 1,
            .dirty          = false,
            .snapshotBefore = state->liveSnapshot};
    }
    return TransactionGuard(*this, state, true);
}

void UndoableSpace::markTransactionDirty(RootState& state) {
    std::scoped_lock lock(state.mutex);
    if (state.activeTransaction)
        state.activeTransaction->dirty = true;
}

auto UndoableSpace::commitTransaction(RootState& state) -> Expected<void> {
    std::unique_lock lock(state.mutex);
    if (!state.activeTransaction) {
        return {};
    }
    auto currentThread = std::this_thread::get_id();
    if (state.activeTransaction->owner != currentThread) {
        return std::unexpected(Error{Error::Code::InvalidPermissions,
                                     "History transaction owned by another thread"});
    }

    auto before = state.activeTransaction->snapshotBefore;
    bool dirty  = state.activeTransaction->dirty;
    auto depth  = state.activeTransaction->depth;

    if (depth == 0) {
        state.activeTransaction.reset();
        return {};
    }

    state.activeTransaction->depth -= 1;
    if (state.activeTransaction->depth > 0) {
        return {};
    }

    state.activeTransaction.reset();

    OperationScope scope(*this, state, "commit");

    if (!dirty) {
        scope.setResult(true, "no_changes");
        return {};
    }

    auto snapshotExpected = captureSnapshotLocked(state);
    if (!snapshotExpected) {
        auto revert = applySnapshotLocked(state, before);
        if (!revert) {
            sp_log("UndoableSpace::commitTransaction rollback failed: "
                       + revert.error().message.value_or("unknown"),
                   "UndoableSpace");
        }
        state.liveSnapshot = before;
        auto beforeMetrics  = state.prototype.analyze(state.liveSnapshot);
        state.liveBytes     = beforeMetrics.payloadBytes;
        scope.setResult(false, snapshotExpected.error().message.value_or("capture_failed"));
        return std::unexpected(snapshotExpected.error());
    }

    auto latest     = snapshotExpected.value();
    auto now        = std::chrono::system_clock::now();
    auto undoBytes  = state.liveBytes;
    RootState::Entry undoEntry;
    undoEntry.snapshot  = before;
    undoEntry.bytes     = undoBytes;
    undoEntry.timestamp = now;
    state.undoStack.push_back(std::move(undoEntry));
    state.telemetry.undoBytes += undoBytes;

    state.liveSnapshot = latest;
    auto latestMetrics = state.prototype.analyze(latest);
    state.liveBytes    = latestMetrics.payloadBytes;
    state.redoStack.clear();
    state.telemetry.redoBytes = 0;

    TrimStats trimStats{};
    if (!state.options.manualGarbageCollect) {
        trimStats = applyRetentionLocked(state, "commit");
        if (trimStats.entriesRemoved > 0) {
            scope.setResult(true, "trimmed=" + std::to_string(trimStats.entriesRemoved));
        }
    }

    return {};
}

auto UndoableSpace::captureSnapshotLocked(RootState& state)
    -> Expected<CowSubtreePrototype::Snapshot> {
    auto* rootNode = resolveRootNode();
    if (!rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "PathSpace backend unavailable"});
    }

    Node* node = rootNode;
    for (auto const& component : state.components) {
        node = node->getChild(component);
        if (!node) {
            return state.prototype.emptySnapshot();
        }
    }

    std::vector<CowSubtreePrototype::Mutation> mutations;
    std::vector<std::string>                   pathComponents;
    std::optional<Error>                       failure;

    auto gather = [&](auto&& self, Node const& current, std::vector<std::string>& components)
                    -> void {
        std::shared_ptr<const std::vector<std::uint8_t>> payloadBytes;
        {
            std::scoped_lock payloadLock(current.payloadMutex);
            if (current.nested) {
                failure = Error{Error::Code::UnknownError,
                                "History does not yet support nested PathSpaces"};
                return;
            }
            if (current.data) {
                auto bytesOpt = current.data->serializeSnapshot();
                if (!bytesOpt.has_value()) {
                    failure = Error{Error::Code::UnknownError,
                                    "Unable to serialize node payload for history"};
                    return;
                }
                auto rawBytes = std::make_shared<std::vector<std::uint8_t>>(bytesOpt->size());
                std::memcpy(rawBytes->data(), bytesOpt->data(), bytesOpt->size());
                payloadBytes = std::move(rawBytes);
            }
        }

        if (payloadBytes) {
            CowSubtreePrototype::Mutation mutation;
            mutation.components = components;
            mutation.payload    = CowSubtreePrototype::Payload(std::move(*payloadBytes));
            mutations.push_back(std::move(mutation));
        }

        current.children.for_each([&](auto const& kv) {
            components.push_back(kv.first);
            self(self, *kv.second, components);
            components.pop_back();
        });
    };

    gather(gather, *node, pathComponents);
    if (failure)
        return std::unexpected(*failure);

    auto snapshot = state.prototype.emptySnapshot();
    for (auto const& mutation : mutations) {
        snapshot = state.prototype.apply(snapshot, mutation);
    }
    return snapshot;
}

auto UndoableSpace::clearSubtree(Node& node) -> void {
    {
        std::scoped_lock lock(node.payloadMutex);
        node.data.reset();
        node.nested.reset();
    }
    std::vector<std::string> eraseList;
    node.children.for_each([&](auto const& kv) { eraseList.push_back(kv.first); });
    for (auto const& key : eraseList) {
        if (auto* child = node.getChild(key)) {
            clearSubtree(*child);
        }
        node.eraseChild(key);
    }
}

auto UndoableSpace::computeTotalBytesLocked(RootState const& state) -> std::size_t {
    return state.liveBytes + state.telemetry.undoBytes + state.telemetry.redoBytes;
}

void UndoableSpace::recordOperation(RootState& state,
                                    std::string_view type,
                                    std::chrono::steady_clock::duration duration,
                                    bool success,
                                    std::size_t undoBefore,
                                    std::size_t redoBefore,
                                    std::size_t bytesBefore,
                                    std::string const& message) {
    RootState::OperationRecord record;
    record.type            = std::string(type);
    record.timestamp       = std::chrono::system_clock::now();
    record.duration        = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    record.success         = success;
    record.undoCountBefore = undoBefore;
    record.undoCountAfter  = state.undoStack.size();
    record.redoCountBefore = redoBefore;
    record.redoCountAfter  = state.redoStack.size();
    record.bytesBefore     = bytesBefore;
    record.bytesAfter      = computeTotalBytesLocked(state);
    record.message         = message;
    state.telemetry.lastOperation = std::move(record);
}

auto UndoableSpace::applyRetentionLocked(RootState& state, std::string_view origin) -> TrimStats {
    (void)origin;
    TrimStats stats{};
    bool      trimmed    = false;
    std::size_t totalBytes = computeTotalBytesLocked(state);

    auto removeOldestUndo = [&]() -> bool {
        if (state.undoStack.empty())
            return false;
        auto entry = state.undoStack.front();
        state.undoStack.erase(state.undoStack.begin());
        if (state.telemetry.undoBytes >= entry.bytes)
            state.telemetry.undoBytes -= entry.bytes;
        else
            state.telemetry.undoBytes = 0;
        totalBytes = totalBytes >= entry.bytes ? totalBytes - entry.bytes : 0;
        stats.entriesRemoved += 1;
        stats.bytesRemoved += entry.bytes;
        trimmed = true;
        return true;
    };

    auto removeOldestRedo = [&]() -> bool {
        if (state.redoStack.empty())
            return false;
        auto entry = state.redoStack.front();
        state.redoStack.erase(state.redoStack.begin());
        if (state.telemetry.redoBytes >= entry.bytes)
            state.telemetry.redoBytes -= entry.bytes;
        else
            state.telemetry.redoBytes = 0;
        totalBytes = totalBytes >= entry.bytes ? totalBytes - entry.bytes : 0;
        stats.entriesRemoved += 1;
        stats.bytesRemoved += entry.bytes;
        trimmed = true;
        return true;
    };

    if (state.options.maxEntries > 0) {
        while (state.undoStack.size() > state.options.maxEntries) {
            if (!removeOldestUndo())
                break;
        }
        while (state.redoStack.size() > state.options.maxEntries) {
            if (!removeOldestRedo())
                break;
        }
    }

    if (state.options.maxBytesRetained > 0) {
        while (totalBytes > state.options.maxBytesRetained) {
            if (!state.undoStack.empty()) {
                if (!removeOldestUndo())
                    break;
                continue;
            }
            if (!state.redoStack.empty()) {
                if (!removeOldestRedo())
                    break;
                continue;
            }
            break;
        }
    }

    if (trimmed) {
        state.telemetry.trimOperations += 1;
        state.telemetry.trimmedEntries += stats.entriesRemoved;
        state.telemetry.trimmedBytes   += stats.bytesRemoved;
        state.telemetry.lastTrimTimestamp = std::chrono::system_clock::now();
    }

    return stats;
}

auto UndoableSpace::gatherStatsLocked(RootState const& state) const -> HistoryStats {
    HistoryStats stats;
    stats.counts.undo          = state.undoStack.size();
    stats.counts.redo          = state.redoStack.size();
    stats.bytes.total                 = computeTotalBytesLocked(state);
    stats.bytes.undo                  = state.telemetry.undoBytes;
    stats.bytes.redo                  = state.telemetry.redoBytes;
    stats.bytes.live                  = state.liveBytes;
    stats.counts.manualGarbageCollect = state.options.manualGarbageCollect;
    stats.trim.operationCount = state.telemetry.trimOperations;
    stats.trim.entries        = state.telemetry.trimmedEntries;
    stats.trim.bytes          = state.telemetry.trimmedBytes;
    if (state.telemetry.lastTrimTimestamp) {
        stats.trim.lastTimestampMs = toMillis(*state.telemetry.lastTrimTimestamp);
    }
    if (state.telemetry.lastOperation) {
        HistoryLastOperation op;
        op.type            = state.telemetry.lastOperation->type;
        op.timestampMs     = toMillis(state.telemetry.lastOperation->timestamp);
        op.durationMs      = static_cast<std::uint64_t>(state.telemetry.lastOperation->duration.count());
        op.success         = state.telemetry.lastOperation->success;
        op.undoCountBefore = state.telemetry.lastOperation->undoCountBefore;
        op.undoCountAfter  = state.telemetry.lastOperation->undoCountAfter;
        op.redoCountBefore = state.telemetry.lastOperation->redoCountBefore;
        op.redoCountAfter  = state.telemetry.lastOperation->redoCountAfter;
        op.bytesBefore     = state.telemetry.lastOperation->bytesBefore;
        op.bytesAfter      = state.telemetry.lastOperation->bytesAfter;
        op.message         = state.telemetry.lastOperation->message;
        stats.lastOperation = std::move(op);
    }
    return stats;
}

auto UndoableSpace::readHistoryValue(MatchedRoot const& matchedRoot,
                                     std::string const& relativePath,
                                     InputMetadata const& metadata,
                                     void* obj) -> std::optional<Error> {
    auto state = matchedRoot.state;
    if (!state) {
        return Error{Error::Code::UnknownError, "History root missing"};
    }

    std::unique_lock lock(state->mutex);
    auto stats = gatherStatsLocked(*state);

    auto assign = [&](auto value, std::string_view descriptor) -> std::optional<Error> {
        using ValueT = std::decay_t<decltype(value)>;
        if (!metadata.typeInfo || *metadata.typeInfo != typeid(ValueT)) {
            return Error{Error::Code::InvalidType,
                         std::string("History telemetry path ") + std::string(descriptor)
                             + " expects type " + typeid(ValueT).name()};
        }
        if (obj == nullptr) {
            return Error{Error::Code::MalformedInput, "Output pointer is null"};
        }
        *static_cast<ValueT*>(obj) = value;
        return std::nullopt;
    };

    if (relativePath == "_history/stats") {
        return assign(stats, relativePath);
    }
    if (relativePath == "_history/stats/undoCount") {
        return assign(stats.counts.undo, relativePath);
    }
    if (relativePath == "_history/stats/redoCount") {
        return assign(stats.counts.redo, relativePath);
    }
    if (relativePath == "_history/stats/undoBytes") {
        return assign(stats.bytes.undo, relativePath);
    }
    if (relativePath == "_history/stats/redoBytes") {
        return assign(stats.bytes.redo, relativePath);
    }
    if (relativePath == "_history/stats/liveBytes") {
        return assign(stats.bytes.live, relativePath);
    }
    if (relativePath == "_history/stats/bytesRetained") {
        return assign(stats.bytes.total, relativePath);
    }
    if (relativePath == "_history/stats/manualGcEnabled") {
        return assign(stats.counts.manualGarbageCollect, relativePath);
    }
    if (relativePath == "_history/stats/trimOperationCount") {
        return assign(stats.trim.operationCount, relativePath);
    }
    if (relativePath == "_history/stats/trimmedEntries") {
        return assign(stats.trim.entries, relativePath);
    }
    if (relativePath == "_history/stats/trimmedBytes") {
        return assign(stats.trim.bytes, relativePath);
    }
    if (relativePath == "_history/stats/lastTrimTimestampMs") {
        return assign(stats.trim.lastTimestampMs, relativePath);
    }
    if (relativePath == "_history/head/generation") {
        return assign(state->liveSnapshot.generation, relativePath);
    }

    if (relativePath.starts_with("_history/lastOperation")) {
        if (!stats.lastOperation) {
            return Error{Error::Code::NoObjectFound, "No history operation recorded"};
        }
        auto const& op = *stats.lastOperation;
        if (relativePath == "_history/lastOperation/type") {
            return assign(op.type, relativePath);
        }
        if (relativePath == "_history/lastOperation/timestampMs") {
            return assign(op.timestampMs, relativePath);
        }
        if (relativePath == "_history/lastOperation/durationMs") {
            return assign(op.durationMs, relativePath);
        }
        if (relativePath == "_history/lastOperation/success") {
            return assign(op.success, relativePath);
        }
        if (relativePath == "_history/lastOperation/undoCountBefore") {
            return assign(op.undoCountBefore, relativePath);
        }
        if (relativePath == "_history/lastOperation/undoCountAfter") {
            return assign(op.undoCountAfter, relativePath);
        }
        if (relativePath == "_history/lastOperation/redoCountBefore") {
            return assign(op.redoCountBefore, relativePath);
        }
        if (relativePath == "_history/lastOperation/redoCountAfter") {
            return assign(op.redoCountAfter, relativePath);
        }
        if (relativePath == "_history/lastOperation/bytesBefore") {
            return assign(op.bytesBefore, relativePath);
        }
        if (relativePath == "_history/lastOperation/bytesAfter") {
            return assign(op.bytesAfter, relativePath);
        }
        if (relativePath == "_history/lastOperation/message") {
            return assign(op.message, relativePath);
        }
    }

    return Error{Error::Code::NotFound, std::string("Unsupported history telemetry path: ") + relativePath};
}

auto UndoableSpace::applySnapshotLocked(RootState& state,
                                        CowSubtreePrototype::Snapshot const& snapshot)
    -> Expected<void> {
    auto* rootNode = resolveRootNode();
    if (!rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "PathSpace backend unavailable"});
    }

    Node* node = rootNode;
    for (auto const& component : state.components) {
        node = &node->getOrCreateChild(component);
    }

    if (!snapshot.valid()) {
        clearSubtree(*node);
        return {};
    }

    auto applyNode = [&](auto&& self, Node& target, CowSubtreePrototype::Node const& source)
        -> Expected<void> {
        {
            std::scoped_lock lock(target.payloadMutex);
            target.nested.reset();
            if (source.payload.bytes) {
                auto nodeDataOpt =
                    NodeData::deserializeSnapshot(std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(source.payload.bytes->data()),
                        source.payload.bytes->size()});
                if (!nodeDataOpt.has_value()) {
                    return std::unexpected(
                        Error{Error::Code::UnknownError, "Failed to restore node payload"});
                }
                target.data = std::make_unique<NodeData>(std::move(*nodeDataOpt));
            } else {
                target.data.reset();
            }
        }

        std::unordered_map<std::string, bool> keep;
        for (auto const& [childName, childNode] : source.children) {
            keep.emplace(childName, true);
            Node& childTarget = target.getOrCreateChild(childName);
            auto  result      = self(self, childTarget, *childNode);
            if (!result)
                return result;
        }

        std::vector<std::string> toErase;
        target.children.for_each([&](auto const& kv) {
            if (!keep.contains(kv.first)) {
                toErase.push_back(kv.first);
            }
        });
        for (auto const& key : toErase) {
            if (auto* child = target.getChild(key)) {
                clearSubtree(*child);
            }
            target.eraseChild(key);
        }
        return Expected<void>{};
    };

    return applyNode(applyNode, *node, *snapshot.root);
}

auto UndoableSpace::interpretSteps(InputData const& data) const -> std::size_t {
    if (!data.metadata.typeInfo || data.obj == nullptr)
        return 1;

    auto interpretUnsigned = [&](auto ptr) -> std::size_t {
        using T = std::remove_pointer_t<decltype(ptr)>;
        if (*ptr <= 0)
            return 1;
        return static_cast<std::size_t>(*ptr);
    };

    if (*data.metadata.typeInfo == typeid(int)) {
        return interpretUnsigned(static_cast<int const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(unsigned int)) {
        return interpretUnsigned(static_cast<unsigned int const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(std::size_t)) {
        return interpretUnsigned(static_cast<std::size_t const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(std::uint64_t)) {
        return interpretUnsigned(static_cast<std::uint64_t const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(std::int64_t)) {
        return interpretUnsigned(static_cast<std::int64_t const*>(data.obj));
    }

    return 1;
}

auto UndoableSpace::handleControlInsert(MatchedRoot const& matchedRoot,
                                        std::string const& command,
                                         InputData const& data) -> InsertReturn {
    InsertReturn ret;
    if (command == "_history/undo") {
        auto steps = interpretSteps(data);
        ConcretePathStringView rootView{matchedRoot.key};
        if (auto result = undo(rootView, steps); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == "_history/redo") {
        auto steps = interpretSteps(data);
        ConcretePathStringView rootView{matchedRoot.key};
        if (auto result = redo(rootView, steps); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == "_history/garbage_collect") {
        auto state = matchedRoot.state;
        std::unique_lock lock(state->mutex);
        OperationScope scope(*this, *state, "garbage_collect");
        auto stats = applyRetentionLocked(*state, "manual");
        if (stats.entriesRemoved == 0) {
            scope.setResult(true, "no_trim");
        } else {
            scope.setResult(true, "trimmed=" + std::to_string(stats.entriesRemoved));
        }
        return ret;
    }
    if (command == "_history/set_manual_garbage_collect") {
        bool manual = false;
        if (data.obj && data.metadata.typeInfo) {
            if (*data.metadata.typeInfo == typeid(bool)) {
                manual = *static_cast<bool const*>(data.obj);
            }
        }
        auto state = matchedRoot.state;
        std::scoped_lock lock(state->mutex);
        state->options.manualGarbageCollect = manual;
        return ret;
    }
    ret.errors.push_back(
        Error{Error::Code::UnknownError, "Unsupported history control command"});
    return ret;
}

auto UndoableSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    auto fullPath = path.toString();
    auto matched  = findRootByPath(fullPath);
    if (!matched.has_value()) {
        return inner_->in(path, data);
    }

    if (!matched->relativePath.empty() && matched->relativePath.starts_with("_history")) {
        return handleControlInsert(*matched, matched->relativePath, data);
    }

    auto guardExpected = beginTransactionInternal(matched->state);
    if (!guardExpected) {
        InsertReturn ret;
        ret.errors.push_back(guardExpected.error());
        return ret;
    }

    auto guard  = std::move(guardExpected.value());
    auto result = inner_->in(path, data);
    if (result.errors.empty()) {
        guard.markDirty();
    }
    if (auto commit = guard.commit(); !commit) {
        result.errors.push_back(commit.error());
    }
    return result;
}

auto UndoableSpace::out(Iterator const& path,
                        InputMetadata const& inputMetadata,
                        Out const& options,
                        void* obj) -> std::optional<Error> {
    auto fullPath = path.toString();
    auto matched  = findRootByPath(fullPath);

    if (!options.doPop) {
        if (matched.has_value() && !matched->relativePath.empty()
            && matched->relativePath.starts_with("_history")) {
            return readHistoryValue(*matched, matched->relativePath, inputMetadata, obj);
        }
        return inner_->out(path, inputMetadata, options, obj);
    }

    if (!matched.has_value()) {
        return inner_->out(path, inputMetadata, options, obj);
    }

    auto guardExpected = beginTransactionInternal(matched->state);
    if (!guardExpected) {
        return guardExpected.error();
    }

    auto guard = std::move(guardExpected.value());
    auto error = inner_->out(path, inputMetadata, options, obj);
    if (!error.has_value()) {
        guard.markDirty();
    }
    if (auto commit = guard.commit(); !commit) {
        return commit.error();
    }
    return error;
}

auto UndoableSpace::undo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::unique_lock lock(state->mutex);
    if (state->activeTransaction) {
        return std::unexpected(
            Error{Error::Code::InvalidPermissions, "Cannot undo while transaction open"});
    }
    if (steps == 0)
        steps = 1;

    while (steps-- > 0) {
        OperationScope scope(*this, *state, "undo");
        if (state->undoStack.empty()) {
            scope.setResult(false, "empty");
            return std::unexpected(Error{Error::Code::NoObjectFound, "Nothing to undo"});
        }

        auto entry = state->undoStack.back();
        state->undoStack.pop_back();
        if (state->telemetry.undoBytes >= entry.bytes) {
            state->telemetry.undoBytes -= entry.bytes;
        } else {
            state->telemetry.undoBytes = 0;
        }

        auto currentSnapshot = state->liveSnapshot;
        auto currentBytes    = state->liveBytes;

        auto applyResult = applySnapshotLocked(*state, entry.snapshot);
        if (!applyResult) {
            auto revert = applySnapshotLocked(*state, currentSnapshot);
            (void)revert;
            state->liveSnapshot = currentSnapshot;
            state->liveBytes    = currentBytes;
            state->undoStack.push_back(std::move(entry));
            state->telemetry.undoBytes += entry.bytes;
            scope.setResult(false, applyResult.error().message.value_or("apply_failed"));
            return std::unexpected(applyResult.error());
        }

        RootState::Entry redoEntry;
        redoEntry.snapshot  = currentSnapshot;
        redoEntry.bytes     = currentBytes;
        redoEntry.timestamp = std::chrono::system_clock::now();
        state->redoStack.push_back(std::move(redoEntry));
        state->telemetry.redoBytes += currentBytes;

        state->liveSnapshot = entry.snapshot;
        state->liveBytes    = entry.bytes;

        if (!state->options.manualGarbageCollect) {
            applyRetentionLocked(*state, "undo");
        }
    }

    return {};
}

auto UndoableSpace::redo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::unique_lock lock(state->mutex);
    if (state->activeTransaction) {
        return std::unexpected(
            Error{Error::Code::InvalidPermissions, "Cannot redo while transaction open"});
    }
    if (steps == 0)
        steps = 1;

    while (steps-- > 0) {
        OperationScope scope(*this, *state, "redo");
        if (state->redoStack.empty()) {
            scope.setResult(false, "empty");
            return std::unexpected(Error{Error::Code::NoObjectFound, "Nothing to redo"});
        }

        auto entry = state->redoStack.back();
        state->redoStack.pop_back();
        if (state->telemetry.redoBytes >= entry.bytes) {
            state->telemetry.redoBytes -= entry.bytes;
        } else {
            state->telemetry.redoBytes = 0;
        }

        auto currentSnapshot = state->liveSnapshot;
        auto currentBytes    = state->liveBytes;

        auto applyResult = applySnapshotLocked(*state, entry.snapshot);
        if (!applyResult) {
            auto revert = applySnapshotLocked(*state, currentSnapshot);
            (void)revert;
            state->liveSnapshot = currentSnapshot;
            state->liveBytes    = currentBytes;
            state->redoStack.push_back(std::move(entry));
            state->telemetry.redoBytes += entry.bytes;
            scope.setResult(false, applyResult.error().message.value_or("apply_failed"));
            return std::unexpected(applyResult.error());
        }

        RootState::Entry undoEntry;
        undoEntry.snapshot  = currentSnapshot;
        undoEntry.bytes     = currentBytes;
        undoEntry.timestamp = std::chrono::system_clock::now();
        state->undoStack.push_back(std::move(undoEntry));
        state->telemetry.undoBytes += currentBytes;

        state->liveSnapshot = entry.snapshot;
        state->liveBytes    = entry.bytes;

        if (!state->options.manualGarbageCollect) {
            applyRetentionLocked(*state, "redo");
        }
    }

    return {};
}

auto UndoableSpace::trimHistory(ConcretePathStringView root, TrimPredicate predicate)
    -> Expected<TrimStats> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::unique_lock lock(state->mutex);
    OperationScope scope(*this, *state, "trim");

    TrimStats stats{};
    if (!predicate) {
        scope.setResult(true, "no_predicate");
        return stats;
    }

    std::vector<RootState::Entry> kept;
    kept.reserve(state->undoStack.size());
    std::size_t bytesRemoved = 0;
    for (std::size_t i = 0; i < state->undoStack.size(); ++i) {
        auto& entry = state->undoStack[i];
        if (predicate(i)) {
            stats.entriesRemoved += 1;
            bytesRemoved += entry.bytes;
        } else {
            kept.push_back(entry);
        }
    }

    if (stats.entriesRemoved == 0) {
        scope.setResult(true, "no_trim");
        return stats;
    }

    stats.bytesRemoved = bytesRemoved;
    state->undoStack    = std::move(kept);
    if (state->telemetry.undoBytes >= bytesRemoved) {
        state->telemetry.undoBytes -= bytesRemoved;
    } else {
        state->telemetry.undoBytes = 0;
    }

    state->telemetry.trimOperations += 1;
    state->telemetry.trimmedEntries += stats.entriesRemoved;
    state->telemetry.trimmedBytes   += stats.bytesRemoved;
    state->telemetry.lastTrimTimestamp = std::chrono::system_clock::now();

    scope.setResult(true, "trimmed=" + std::to_string(stats.entriesRemoved));
    return stats;
}

auto UndoableSpace::getHistoryStats(ConcretePathStringView root) const -> Expected<HistoryStats> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::scoped_lock lock(state->mutex);
    return gatherStatsLocked(*state);
}

auto UndoableSpace::beginTransaction(ConcretePathStringView root) -> Expected<HistoryTransaction> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    auto guardExpected = beginTransactionInternal(state);
    if (!guardExpected)
        return std::unexpected(guardExpected.error());

    auto guard = std::move(guardExpected.value());
    guard.deactivate();
    return HistoryTransaction(*this, std::move(state));
}

auto UndoableSpace::shutdown() -> void {
    if (inner_)
        inner_->shutdown();
}

auto UndoableSpace::notify(std::string const& notificationPath) -> void {
    if (inner_)
        inner_->notify(notificationPath);
}

} // namespace SP::History
