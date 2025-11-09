#include "history/UndoableSpace.hpp"

#include "history/UndoSnapshotCodec.hpp"
#include "history/UndoHistoryMetadata.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "history/UndoableSpaceState.hpp"

#include "core/InsertReturn.hpp"
#include "core/Node.hpp"
#include "core/NodeData.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <limits>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using SP::Error;
using SP::Expected;
using SP::History::CowSubtreePrototype;
using SP::History::Detail::forEachHistoryStack;
namespace UndoUtilsAlias    = SP::History::UndoUtils;
namespace UndoPaths         = SP::History::UndoUtils::Paths;
namespace UndoMetadata      = SP::History::UndoMetadata;
namespace UndoSnapshotCodec = SP::History::UndoSnapshotCodec;
namespace UndoJournal       = SP::History::UndoJournal;

inline auto nodeDataBytes(SP::NodeData const& data) -> std::size_t {
    auto raw   = data.rawBuffer();
    auto front = data.rawBufferFrontOffset();
    if (front >= raw.size()) {
        return 0;
    }
    return raw.size() - front;
}

inline auto nodeDataBytes(std::optional<SP::NodeData> const& data) -> std::size_t {
    if (!data.has_value()) {
        return 0;
    }
    return nodeDataBytes(data.value());
}

auto subtreePayloadBytes(SP::Node const& node) -> std::size_t {
    std::size_t total = 0;
    {
        std::scoped_lock lock(node.payloadMutex);
        if (node.data) {
            total += nodeDataBytes(*node.data);
        }
    }
    node.forEachChild([&](std::string_view, SP::Node const& child) {
        total += subtreePayloadBytes(child);
    });
    return total;
}

} // namespace

namespace SP::History {

using SP::ConcretePathStringView;

auto UndoableSpace::payloadBytes(NodeData const& data) -> std::size_t {
    return nodeDataBytes(data);
}

auto UndoableSpace::payloadBytes(std::optional<NodeData> const& data) -> std::size_t {
    return nodeDataBytes(data);
}

void UndoableSpace::adjustLiveBytes(std::size_t& liveBytes,
                                    std::size_t beforeBytes,
                                    std::size_t afterBytes) {
    if (afterBytes >= beforeBytes) {
        liveBytes += afterBytes - beforeBytes;
        return;
    }
    auto const delta = beforeBytes - afterBytes;
    liveBytes        = delta > liveBytes ? 0 : liveBytes - delta;
}

auto UndoableSpace::computeJournalLiveBytes(UndoJournalRootState const& state) const -> std::size_t {
    auto* rootNode = const_cast<UndoableSpace*>(this)->resolveRootNode();
    if (!rootNode) {
        return 0;
    }

    Node const* node = rootNode;
    for (auto const& component : state.components) {
        node = node->getChild(component);
        if (!node) {
            return 0;
        }
    }

    if (!node) {
        return 0;
    }
    return subtreePayloadBytes(*node);
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
    std::optional<std::string>                 failurePath;
    std::optional<std::string>                 failureReason;

    auto makeFailurePath = [&](std::vector<std::string> const& components) -> std::string {
        std::filesystem::path path(state.rootPath.empty() ? std::filesystem::path{"/"}
                                                          : std::filesystem::path{state.rootPath});
        for (auto const& component : components) {
            path /= component;
        }
        auto str = path.generic_string();
        if (str.empty()) {
            return "/";
        }
        return str;
    };

    auto gather = [&](auto&& self, Node const& current, std::vector<std::string>& components)
                    -> void {
        std::shared_ptr<const std::vector<std::byte>> payloadBytes;
        {
            std::scoped_lock payloadLock(current.payloadMutex);
            if (current.nested) {
                failure       = Error{Error::Code::UnknownError,
                                      std::string(UndoUtilsAlias::UnsupportedNestedMessage)};
                failurePath   = makeFailurePath(components);
                failureReason = failure->message;
                return;
            }
            if (current.data) {
                if (current.data->hasExecutionPayload()) {
                    failure       = Error{Error::Code::UnknownError,
                                          std::string(UndoUtilsAlias::UnsupportedExecutionMessage)};
                    failurePath   = makeFailurePath(components);
                    failureReason = failure->message;
                    return;
                }
                auto bytesOpt = current.data->serializeSnapshot();
                if (!bytesOpt.has_value()) {
                    failure       = Error{Error::Code::UnknownError,
                                          std::string(UndoUtilsAlias::UnsupportedSerializationMessage)};
                    failurePath   = makeFailurePath(components);
                    failureReason = failure->message;
                    return;
                }
                auto rawBytes = std::make_shared<std::vector<std::byte>>(std::move(*bytesOpt));
                payloadBytes  = std::move(rawBytes);
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
    if (failure) {
        if (failurePath && failureReason) {
            recordUnsupportedPayloadLocked(state, *failurePath, *failureReason);
            std::string message = *failureReason;
            message.append(" at ");
            message.append(*failurePath);
            failure->message = std::move(message);
        }
        return std::unexpected(*failure);
    }

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

    auto removeOldest = [&](auto& stack, std::size_t& telemetryBytes) -> bool {
        if (stack.empty())
            return false;
        auto entry = stack.front();
        stack.erase(stack.begin());
        if (state.persistenceEnabled && entry.persisted) {
            removeEntryFiles(state, entry.snapshot.generation);
        }
        if (telemetryBytes >= entry.bytes)
            telemetryBytes -= entry.bytes;
        else
            telemetryBytes = 0;
        totalBytes = totalBytes >= entry.bytes ? totalBytes - entry.bytes : 0;
        stats.entriesRemoved += 1;
        stats.bytesRemoved += entry.bytes;
        trimmed = true;
        return true;
    };

    auto removeOldestUndo = [&]() -> bool {
        return removeOldest(state.undoStack, state.telemetry.undoBytes);
    };
    auto removeOldestRedo = [&]() -> bool {
        return removeOldest(state.redoStack, state.telemetry.redoBytes);
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
        updateTrimTelemetryLocked(state, stats);
    }

    return stats;
}

void UndoableSpace::updateTrimTelemetryLocked(RootState& state, TrimStats const& stats) {
    if (stats.entriesRemoved == 0) {
        return;
    }
    state.telemetry.trimOperations += 1;
    state.telemetry.trimmedEntries += stats.entriesRemoved;
    state.telemetry.trimmedBytes += stats.bytesRemoved;
    state.telemetry.lastTrimTimestamp = std::chrono::system_clock::now();
}

auto UndoableSpace::gatherStatsLocked(RootState const& state) const -> HistoryStats {
    HistoryStats stats;
    stats.counts.undo          = state.undoStack.size();
    stats.counts.redo          = state.redoStack.size();
    stats.bytes.total          = computeTotalBytesLocked(state);
    stats.bytes.undo           = state.telemetry.undoBytes;
    stats.bytes.redo           = state.telemetry.redoBytes;
    stats.bytes.live           = state.liveBytes;
    stats.bytes.disk           = state.telemetry.diskBytes;
    stats.counts.manualGarbageCollect = state.options.manualGarbageCollect;
    stats.counts.diskEntries          = state.telemetry.diskEntries;
    stats.counts.cachedUndo           = state.telemetry.cachedUndo;
    stats.counts.cachedRedo           = state.telemetry.cachedRedo;
    stats.trim.operationCount = state.telemetry.trimOperations;
    stats.trim.entries        = state.telemetry.trimmedEntries;
    stats.trim.bytes          = state.telemetry.trimmedBytes;
    if (state.telemetry.lastTrimTimestamp) {
        stats.trim.lastTimestampMs = UndoUtilsAlias::toMillis(*state.telemetry.lastTrimTimestamp);
    }
    if (state.telemetry.lastOperation) {
        HistoryLastOperation op;
        op.type            = state.telemetry.lastOperation->type;
        op.timestampMs     = UndoUtilsAlias::toMillis(state.telemetry.lastOperation->timestamp);
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
    stats.unsupported.total = state.telemetry.unsupportedTotal;
    stats.unsupported.recent.reserve(state.telemetry.unsupportedLog.size());
    for (auto const& entry : state.telemetry.unsupportedLog) {
        HistoryUnsupportedRecord record;
        record.path            = entry.path;
        record.reason          = entry.reason;
        record.occurrences     = entry.occurrences;
        record.lastTimestampMs = UndoUtilsAlias::toMillis(entry.timestamp);
        stats.unsupported.recent.push_back(std::move(record));
    }
    return stats;
}

auto UndoableSpace::gatherJournalStatsLocked(UndoJournalRootState const& state) const -> HistoryStats {
    HistoryStats stats;
    auto         journalStats = state.journal.stats();

    stats.counts.undo = journalStats.undoCount;
    stats.counts.redo = journalStats.redoCount;

    stats.bytes.undo  = journalStats.undoBytes;
    stats.bytes.redo  = journalStats.redoBytes;
    stats.bytes.live  = state.liveBytes;
    stats.bytes.total = stats.bytes.undo + stats.bytes.redo + stats.bytes.live;
    stats.bytes.disk  = state.telemetry.diskBytes;

    stats.counts.manualGarbageCollect = state.options.manualGarbageCollect;
    stats.counts.diskEntries          = state.telemetry.diskEntries;
    stats.counts.cachedUndo           = state.telemetry.cachedUndo;
    stats.counts.cachedRedo           = state.telemetry.cachedRedo;

    stats.trim.operationCount = state.telemetry.trimOperations;
    stats.trim.entries        = state.telemetry.trimmedEntries;
    stats.trim.bytes          = state.telemetry.trimmedBytes;
    if (state.telemetry.lastTrimTimestamp) {
        stats.trim.lastTimestampMs = UndoUtilsAlias::toMillis(*state.telemetry.lastTrimTimestamp);
    }

    if (state.telemetry.lastOperation) {
        HistoryLastOperation op;
        op.type            = state.telemetry.lastOperation->type;
        op.timestampMs     = UndoUtilsAlias::toMillis(state.telemetry.lastOperation->timestamp);
        op.durationMs      =
            static_cast<std::uint64_t>(state.telemetry.lastOperation->duration.count());
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

    stats.unsupported.total = state.telemetry.unsupportedTotal;
    stats.unsupported.recent.reserve(state.telemetry.unsupportedLog.size());
    for (auto const& entry : state.telemetry.unsupportedLog) {
        HistoryUnsupportedRecord record;
        record.path            = entry.path;
        record.reason          = entry.reason;
        record.occurrences     = entry.occurrences;
        record.lastTimestampMs = UndoUtilsAlias::toMillis(entry.timestamp);
        stats.unsupported.recent.push_back(std::move(record));
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
    return readHistoryStatsValue(
        stats,
        std::optional<std::size_t>{static_cast<std::size_t>(state->liveSnapshot.generation)},
        relativePath,
        metadata,
        obj);
}

auto UndoableSpace::readJournalHistoryValue(MatchedJournalRoot const& matchedRoot,
                                            std::string const& relativePath,
                                            InputMetadata const& metadata,
                                            void* obj) -> std::optional<Error> {
    auto state = matchedRoot.state;
    if (!state) {
        return Error{Error::Code::UnknownError, "History root missing"};
    }

    std::unique_lock lock(state->mutex);
    auto stats = gatherJournalStatsLocked(*state);
    auto head  = std::optional<std::size_t>{
        static_cast<std::size_t>(std::min<std::uint64_t>(
            state->nextSequence, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())))};
    return readHistoryStatsValue(stats, head, relativePath, metadata, obj);
}

void UndoableSpace::recordUnsupportedPayloadLocked(RootState& state,
                                                   std::string const& path,
                                                   std::string const& reason) {
    auto now = std::chrono::system_clock::now();
    state.telemetry.unsupportedTotal++;

    auto it = std::find_if(state.telemetry.unsupportedLog.begin(),
                           state.telemetry.unsupportedLog.end(),
                           [&](auto const& entry) {
                               return entry.path == path && entry.reason == reason;
                           });
    if (it != state.telemetry.unsupportedLog.end()) {
        it->occurrences += 1;
        it->timestamp = now;
        if (std::next(it) != state.telemetry.unsupportedLog.end()) {
            auto updated = std::move(*it);
            state.telemetry.unsupportedLog.erase(it);
            state.telemetry.unsupportedLog.push_back(std::move(updated));
        }
        return;
    }

    RootState::Telemetry::UnsupportedRecord record;
    record.path        = path;
    record.reason      = reason;
    record.timestamp   = now;
    record.occurrences = 1;
    state.telemetry.unsupportedLog.push_back(std::move(record));
    if (state.telemetry.unsupportedLog.size() > UndoUtilsAlias::MaxUnsupportedLogEntries) {
        state.telemetry.unsupportedLog.erase(state.telemetry.unsupportedLog.begin());
    }
}

void UndoableSpace::recordJournalUnsupportedPayload(UndoJournalRootState& state,
                                                    std::string const& path,
                                                    std::string const& reason) {
    auto now = std::chrono::system_clock::now();
    state.telemetry.unsupportedTotal++;

    auto it = std::find_if(state.telemetry.unsupportedLog.begin(),
                           state.telemetry.unsupportedLog.end(),
                           [&](auto const& entry) {
                               return entry.path == path && entry.reason == reason;
                           });
    if (it != state.telemetry.unsupportedLog.end()) {
        it->occurrences += 1;
        it->timestamp = now;
        if (std::next(it) != state.telemetry.unsupportedLog.end()) {
            auto updated = std::move(*it);
            state.telemetry.unsupportedLog.erase(it);
            state.telemetry.unsupportedLog.push_back(std::move(updated));
        }
        return;
    }

    RootState::Telemetry::UnsupportedRecord record;
    record.path        = path;
    record.reason      = reason;
    record.timestamp   = now;
    record.occurrences = 1;
    state.telemetry.unsupportedLog.push_back(std::move(record));
    if (state.telemetry.unsupportedLog.size() > UndoUtilsAlias::MaxUnsupportedLogEntries) {
        state.telemetry.unsupportedLog.erase(state.telemetry.unsupportedLog.begin());
    }
}

auto UndoableSpace::parseJournalRelativeComponents(UndoJournalRootState const& state,
                                                   std::string const& path) const
    -> Expected<std::vector<std::string>> {
    ConcretePathStringView pathView{path};
    auto canonical = pathView.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    auto componentsExpected = canonical->components();
    if (!componentsExpected) {
        return std::unexpected(componentsExpected.error());
    }
    auto components = std::move(componentsExpected.value());
    if (components.size() < state.components.size()) {
        return std::unexpected(
            Error{Error::Code::InvalidPermissions, "Journal entry path outside history root"});
    }
    for (std::size_t i = 0; i < state.components.size(); ++i) {
        if (components[i] != state.components[i]) {
            return std::unexpected(
                Error{Error::Code::InvalidPermissions, "Journal entry path outside history root"});
        }
    }
    std::vector<std::string> relative;
    relative.reserve(components.size() - state.components.size());
    std::copy(std::next(components.begin(), static_cast<std::ptrdiff_t>(state.components.size())),
              components.end(),
              std::back_inserter(relative));
    return relative;
}

auto UndoableSpace::captureJournalNodeData(UndoJournalRootState const& state,
                                           std::vector<std::string> const& relativeComponents) const
    -> Expected<std::optional<NodeData>> {
    auto* rootNode = const_cast<UndoableSpace*>(this)->resolveRootNode();
    if (!rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "PathSpace backend unavailable"});
    }

    Node const* node = rootNode;
    for (auto const& component : state.components) {
        node = node->getChild(component);
        if (!node) {
            return std::optional<NodeData>{};
        }
    }

    for (auto const& component : relativeComponents) {
        if (!node) {
            return std::optional<NodeData>{};
        }
        node = node->getChild(component);
        if (!node) {
            return std::optional<NodeData>{};
        }
    }

    if (!node) {
        return std::optional<NodeData>{};
    }

    std::scoped_lock payloadLock(node->payloadMutex);
    if (node->nested) {
        return std::unexpected(Error{Error::Code::UnknownError,
                                     std::string(UndoUtilsAlias::UnsupportedNestedMessage)});
    }
    if (!node->data) {
        return std::optional<NodeData>{};
    }
    if (node->data->hasExecutionPayload()) {
        return std::unexpected(Error{Error::Code::UnknownError,
                                     std::string(UndoUtilsAlias::UnsupportedExecutionMessage)});
    }
    return std::optional<NodeData>{*node->data};
}

auto UndoableSpace::applyJournalNodeData(UndoJournalRootState& state,
                                         std::vector<std::string> const& relativeComponents,
                                         std::optional<NodeData> const& data) -> Expected<void> {
    auto* rootNode = resolveRootNode();
    if (!rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "PathSpace backend unavailable"});
    }

    Node* node = rootNode;
    for (auto const& component : state.components) {
        node = &node->getOrCreateChild(component);
    }

    for (auto const& component : relativeComponents) {
        if (data.has_value()) {
            node = &node->getOrCreateChild(component);
        } else {
            auto* existing = node->getChild(component);
            if (!existing) {
                return {};
            }
            node = existing;
        }
    }

    if (!node) {
        return {};
    }

    auto beforeBytes = std::size_t{0};
    auto afterBytes  = payloadBytes(data);

    {
        std::scoped_lock payloadLock(node->payloadMutex);
        if (node->data) {
            beforeBytes = payloadBytes(*node->data);
        }
        if (data.has_value()) {
            node->data = std::make_unique<NodeData>(data.value());
        } else {
            node->data.reset();
        }
    }

    adjustLiveBytes(state.liveBytes, beforeBytes, afterBytes);
    return {};
}

auto UndoableSpace::applyHistorySteps(ConcretePathStringView root,
                                      std::size_t            steps,
                                      bool                   isUndo) -> Expected<void> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::unique_lock lock(state->mutex);
    auto const busyMessage = isUndo ? "Cannot undo while transaction open"
                                    : "Cannot redo while transaction open";
    if (state->activeTransaction) {
        return std::unexpected(Error{Error::Code::InvalidPermissions, busyMessage});
    }
    if (steps == 0)
        steps = 1;

    auto const operationName = isUndo ? std::string_view{"undo"} : std::string_view{"redo"};
    auto const emptyMessage  = isUndo ? std::string_view{"Nothing to undo"}
                                      : std::string_view{"Nothing to redo"};

    while (steps-- > 0) {
        auto step = performHistoryStep(*state, isUndo, operationName, emptyMessage, operationName);
        if (!step)
            return step;
    }

    return finalizeHistoryMutation(*state);
}

auto UndoableSpace::applyJournalSteps(std::shared_ptr<UndoJournalRootState> const& statePtr,
                                      std::size_t steps,
                                      bool        isUndo) -> Expected<void> {
    if (!statePtr)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    auto state = statePtr;
    std::unique_lock lock(state->mutex);
    auto const busyMessage = isUndo ? "Cannot undo while transaction open"
                                    : "Cannot redo while transaction open";
    if (state->activeTransaction) {
        return std::unexpected(Error{Error::Code::InvalidPermissions, busyMessage});
    }

    if (steps == 0)
        steps = 1;

    auto const operationName = isUndo ? std::string_view{"undo"} : std::string_view{"redo"};
    auto const emptyMessage  = isUndo ? std::string_view{"Nothing to undo"}
                                      : std::string_view{"Nothing to redo"};

    while (steps-- > 0) {
        auto step = performJournalStep(*state, isUndo, operationName, emptyMessage);
        if (!step)
            return step;
    }

    state->stateDirty       = true;
    state->persistenceDirty = state->persistenceDirty || state->persistenceEnabled;
    return {};
}

auto UndoableSpace::performHistoryStep(RootState& state,
                                       bool       sourceIsUndo,
                                       std::string_view operationName,
                                       std::string_view emptyMessage,
                                       std::string_view retentionOrigin) -> Expected<void> {
    auto& sourceStack          = sourceIsUndo ? state.undoStack : state.redoStack;
    auto& targetStack          = sourceIsUndo ? state.redoStack : state.undoStack;
    auto& sourceTelemetryBytes = sourceIsUndo ? state.telemetry.undoBytes : state.telemetry.redoBytes;
    auto& targetTelemetryBytes = sourceIsUndo ? state.telemetry.redoBytes : state.telemetry.undoBytes;

    OperationScope scope(*this, state, operationName);
    if (sourceStack.empty()) {
        scope.setResult(false, "empty");
        return std::unexpected(
            Error{Error::Code::NoObjectFound, std::string(emptyMessage)});
    }

    auto index = sourceStack.size() - 1;
    if (!sourceStack[index].cached && sourceStack[index].persisted) {
        auto load = loadEntrySnapshotLocked(state, index, sourceIsUndo);
        if (!load) {
            scope.setResult(false, "load_failed");
            return std::unexpected(load.error());
        }
    }

    auto entry       = sourceStack.back();
    auto entryBytes  = entry.bytes;
    sourceStack.pop_back();
    if (sourceTelemetryBytes >= entryBytes) {
        sourceTelemetryBytes -= entryBytes;
    } else {
        sourceTelemetryBytes = 0;
    }

    auto currentSnapshot = state.liveSnapshot;
    auto currentBytes    = state.liveBytes;

    auto applyResult = applySnapshotLocked(state, entry.snapshot);
    if (!applyResult) {
        auto revert = applySnapshotLocked(state, currentSnapshot);
        (void)revert;
        state.liveSnapshot = currentSnapshot;
        state.liveBytes    = currentBytes;
        sourceStack.push_back(std::move(entry));
        sourceTelemetryBytes += entryBytes;
        scope.setResult(false, applyResult.error().message.value_or("apply_failed"));
        return std::unexpected(applyResult.error());
    }

    RootState::Entry inverseEntry;
    inverseEntry.snapshot  = currentSnapshot;
    inverseEntry.bytes     = currentBytes;
    inverseEntry.timestamp = std::chrono::system_clock::now();
    inverseEntry.persisted = state.persistenceEnabled;
    inverseEntry.cached    = true;
    targetStack.push_back(std::move(inverseEntry));
    targetTelemetryBytes += currentBytes;

    state.liveSnapshot = entry.snapshot;
    state.liveBytes    = entry.bytes;

    if (!state.options.manualGarbageCollect) {
        applyRetentionLocked(state, retentionOrigin);
    }

    scope.setResult(true);
    return {};
}

auto UndoableSpace::performJournalStep(UndoJournalRootState& state,
                                       bool                  sourceIsUndo,
                                       std::string_view      operationName,
                                       std::string_view      emptyMessage) -> Expected<void> {
    JournalOperationScope scope(*this, state, operationName);

    auto entryOpt =
        sourceIsUndo ? state.journal.undo() : state.journal.redo();
    if (!entryOpt.has_value()) {
        scope.setResult(false, "empty");
        return std::unexpected(
            Error{Error::Code::NoObjectFound, std::string(emptyMessage)});
    }
    auto const& entry = entryOpt->get();

    auto componentsExpected = parseJournalRelativeComponents(state, entry.path);
    if (!componentsExpected) {
        scope.setResult(false, "path_invalid");
        return std::unexpected(componentsExpected.error());
    }
    auto relativeComponents = std::move(componentsExpected.value());

    std::optional<NodeData> payload;

    auto decodePayload = [&](UndoJournal::SerializedPayload const& serialized,
                             std::string_view context) -> Expected<std::optional<NodeData>> {
        if (!serialized.present)
            return std::optional<NodeData>{};
        auto decoded = UndoJournal::decodeNodeDataPayload(serialized);
        if (!decoded) {
            auto error = decoded.error();
            auto message = error.message.value_or(std::string(context));
            scope.setResult(false, std::string(context));
            return std::unexpected(Error{error.code, std::move(message)});
        }
        return std::optional<NodeData>{decoded.value()};
    };

    auto payloadExpected = sourceIsUndo ? decodePayload(entry.inverseValue, "decode_inverse_failed")
                                        : decodePayload(entry.value, "decode_value_failed");
    if (!payloadExpected) {
        return std::unexpected(payloadExpected.error());
    }
    payload = std::move(payloadExpected.value());

    auto applyResult = applyJournalNodeData(state, relativeComponents, payload);
    if (!applyResult) {
        scope.setResult(false, applyResult.error().message.value_or("apply_failed"));
        return applyResult;
    }

    scope.setResult(true);
    return {};
}

auto UndoableSpace::finalizeHistoryMutation(RootState& state, bool forceFsync) -> Expected<void> {
    state.stateDirty = true;
    applyRamCachePolicyLocked(state);
    updateCacheTelemetryLocked(state);
    auto persistResult = persistStacksLocked(state, forceFsync);
    if (!persistResult)
        return persistResult;
    return {};
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
    if (command == UndoPaths::CommandUndo) {
        auto steps = interpretSteps(data);
        ConcretePathStringView rootView{matchedRoot.key};
        if (auto result = undo(rootView, steps); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == UndoPaths::CommandRedo) {
        auto steps = interpretSteps(data);
        ConcretePathStringView rootView{matchedRoot.key};
        if (auto result = redo(rootView, steps); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == UndoPaths::CommandGarbageCollect) {
        auto state = matchedRoot.state;
        std::unique_lock lock(state->mutex);
        OperationScope scope(*this, *state, "garbage_collect");
        auto stats = applyRetentionLocked(*state, "manual");
        if (stats.entriesRemoved == 0) {
            scope.setResult(true, "no_trim");
        } else {
            scope.setResult(true, "trimmed=" + std::to_string(stats.entriesRemoved));
        }
        auto persist = finalizeHistoryMutation(*state, true);
        if (!persist) {
            ret.errors.push_back(persist.error());
        }
        return ret;
    }
    if (command == UndoPaths::CommandSetManualGc) {
        bool manual = false;
        if (data.obj && data.metadata.typeInfo) {
            if (*data.metadata.typeInfo == typeid(bool)) {
                manual = *static_cast<bool const*>(data.obj);
            }
        }
        auto state = matchedRoot.state;
        std::scoped_lock lock(state->mutex);
        state->options.manualGarbageCollect = manual;
        state->stateDirty                   = true;
        auto persist = persistStacksLocked(*state, !manual);
        if (!persist) {
            ret.errors.push_back(persist.error());
        }
        return ret;
    }
    ret.errors.push_back(
        Error{Error::Code::UnknownError, "Unsupported history control command"});
    return ret;
}

auto UndoableSpace::handleJournalControlInsert(MatchedJournalRoot const& matchedRoot,
                                               std::string const& command,
                                               InputData const& data) -> InsertReturn {
    InsertReturn ret;
    auto         state = matchedRoot.state;
    if (!state) {
        ret.errors.push_back(Error{Error::Code::UnknownError, "History root missing"});
        return ret;
    }

    if (command == UndoPaths::CommandUndo) {
        auto steps = interpretSteps(data);
        if (auto result = applyJournalSteps(state, steps, true); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == UndoPaths::CommandRedo) {
        auto steps = interpretSteps(data);
        if (auto result = applyJournalSteps(state, steps, false); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == UndoPaths::CommandGarbageCollect) {
        std::unique_lock lock(state->mutex);
        JournalOperationScope scope(*this, *state, "garbage_collect");
        if (state->activeTransaction) {
            scope.setResult(false, "transaction_active");
            ret.errors.push_back(Error{Error::Code::InvalidPermissions,
                                       "Cannot garbage collect while transaction open"});
            return ret;
        }

        auto beforeStats = state->journal.stats();
        auto policy      = state->journal.policy();
        state->journal.setRetentionPolicy(policy);
        auto afterStats   = state->journal.stats();
        auto trimmedEntries =
            afterStats.trimmedEntries >= beforeStats.trimmedEntries
                ? afterStats.trimmedEntries - beforeStats.trimmedEntries
                : std::size_t{0};
        auto trimmedBytes =
            afterStats.trimmedBytes >= beforeStats.trimmedBytes
                ? afterStats.trimmedBytes - beforeStats.trimmedBytes
                : std::size_t{0};

        state->telemetry.cachedUndo = afterStats.undoCount;
        state->telemetry.cachedRedo = afterStats.redoCount;
        state->telemetry.undoBytes  = afterStats.undoBytes;
        state->telemetry.redoBytes  = afterStats.redoBytes;

        if (trimmedEntries == 0) {
            scope.setResult(true, "no_trim");
        } else {
            scope.setResult(true, "trimmed=" + std::to_string(trimmedEntries));
            auto now = std::chrono::system_clock::now();
            state->telemetry.trimOperations += 1;
            state->telemetry.trimmedEntries += trimmedEntries;
            state->telemetry.trimmedBytes += trimmedBytes;
            state->telemetry.lastTrimTimestamp = now;
        }

        bool compactFailed = false;
        if (state->persistenceEnabled) {
            if (trimmedEntries > 0) {
                auto compact = compactJournalPersistence(
                    *state, true);
                if (!compact) {
                    ret.errors.push_back(compact.error());
                    compactFailed = true;
                } else {
                    state->persistenceDirty           = false;
                    state->telemetry.persistenceDirty = false;
                }
            } else {
                updateJournalDiskTelemetry(*state);
                state->persistenceDirty           = false;
                state->telemetry.persistenceDirty = false;
            }
        }
        if (state->persistenceEnabled) {
            updateJournalDiskTelemetry(*state);
        }

        state->stateDirty       = true;
        if (state->persistenceEnabled && compactFailed) {
            state->persistenceDirty           = true;
            state->telemetry.persistenceDirty = true;
        }
        return ret;
    }
    if (command == UndoPaths::CommandSetManualGc) {
        bool manual = false;
        if (data.obj && data.metadata.typeInfo) {
            if (*data.metadata.typeInfo == typeid(bool)) {
                manual = *static_cast<bool const*>(data.obj);
            }
        }
        std::scoped_lock lock(state->mutex);
        JournalOperationScope scope(*this, *state, "set_manual_gc");
        state->options.manualGarbageCollect = manual;
        state->stateDirty                   = true;
        state->persistenceDirty             = state->persistenceDirty || state->persistenceEnabled;
        scope.setResult(true, manual ? "enabled" : "disabled");
        return ret;
    }

    ret.errors.push_back(
        Error{Error::Code::UnknownError, "Unsupported history control command"});
    return ret;
}

auto UndoableSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    auto fullPath       = path.toString();
    auto matched        = findRootByPath(fullPath);
    auto journalMatched = findJournalRootByPath(fullPath);
    if (!matched.has_value()) {
        if (journalMatched.has_value()) {
            if (!journalMatched->relativePath.empty()
                && journalMatched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
                return handleJournalControlInsert(*journalMatched,
                                                  journalMatched->relativePath,
                                                  data);
            }

            auto componentsExpected =
                parseJournalRelativeComponents(*journalMatched->state, fullPath);
            if (!componentsExpected) {
                InsertReturn ret;
                ret.errors.push_back(componentsExpected.error());
                return ret;
            }
            auto relativeComponents = std::move(componentsExpected.value());

            auto beforeExpected =
                captureJournalNodeData(*journalMatched->state, relativeComponents);
            if (!beforeExpected) {
                InsertReturn ret;
                ret.errors.push_back(beforeExpected.error());
                return ret;
            }
            auto beforeNode = beforeExpected.value();

            auto guardExpected = beginJournalTransactionInternal(journalMatched->state);
            if (!guardExpected) {
                InsertReturn ret;
                ret.errors.push_back(guardExpected.error());
                return ret;
            }

            auto guard  = std::move(guardExpected.value());
            auto result = inner->in(path, data);
            if (result.errors.empty()) {
                auto afterExpected =
                    captureJournalNodeData(*journalMatched->state, relativeComponents);
                if (!afterExpected) {
                    result.errors.push_back(afterExpected.error());
                } else {
                    auto afterNode = afterExpected.value();
                    auto record    = recordJournalMutation(*journalMatched->state,
                                                        UndoJournal::OperationKind::Insert,
                                                        fullPath,
                                                        afterNode,
                                                        beforeNode);
                    if (!record) {
                        result.errors.push_back(record.error());
                    }
                }
            }
            if (auto commit = guard.commit(); !commit) {
                result.errors.push_back(commit.error());
            }
            return result;
        }
        return inner->in(path, data);
    }

    if (!matched->relativePath.empty() && matched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
        return handleControlInsert(*matched, matched->relativePath, data);
    }

    auto guardExpected = beginTransactionInternal(matched->state);
    if (!guardExpected) {
        InsertReturn ret;
        ret.errors.push_back(guardExpected.error());
        return ret;
    }

    auto guard  = std::move(guardExpected.value());
    auto result = inner->in(path, data);
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
    auto fullPath       = path.toString();
    auto matched        = findRootByPath(fullPath);
    auto journalMatched = findJournalRootByPath(fullPath);

    if (!options.doPop) {
        if (matched.has_value() && !matched->relativePath.empty()
            && matched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
            return readHistoryValue(*matched, matched->relativePath, inputMetadata, obj);
        }
        if (journalMatched.has_value()) {
            if (!journalMatched->relativePath.empty()
                && journalMatched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
                return readJournalHistoryValue(
                    *journalMatched, journalMatched->relativePath, inputMetadata, obj);
            }
            return inner->out(path, inputMetadata, options, obj);
        }
        return inner->out(path, inputMetadata, options, obj);
    }

    if (!matched.has_value()) {
        if (journalMatched.has_value()) {
            if (!journalMatched->relativePath.empty()
                && journalMatched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
                return journalNotReadyError(journalMatched->key);
            }

            auto componentsExpected =
                parseJournalRelativeComponents(*journalMatched->state, fullPath);
            if (!componentsExpected) {
                return componentsExpected.error();
            }
            auto relativeComponents = std::move(componentsExpected.value());

            auto beforeExpected =
                captureJournalNodeData(*journalMatched->state, relativeComponents);
            if (!beforeExpected) {
                return beforeExpected.error();
            }
            auto beforeNode = beforeExpected.value();

            auto guardExpected = beginJournalTransactionInternal(journalMatched->state);
            if (!guardExpected)
                return guardExpected.error();

            auto guard = std::move(guardExpected.value());
            auto error = inner->out(path, inputMetadata, options, obj);
            if (!error.has_value()) {
                auto afterExpected =
                    captureJournalNodeData(*journalMatched->state, relativeComponents);
                if (!afterExpected) {
                    return afterExpected.error();
                }
                auto afterNode = afterExpected.value();
                auto record    = recordJournalMutation(*journalMatched->state,
                                                    UndoJournal::OperationKind::Take,
                                                    fullPath,
                                                    afterNode,
                                                    beforeNode);
                if (!record) {
                    return record.error();
                }
            }
            if (auto commit = guard.commit(); !commit) {
                return commit.error();
            }
            return error;
        }
        return inner->out(path, inputMetadata, options, obj);
    }

    auto guardExpected = beginTransactionInternal(matched->state);
    if (!guardExpected) {
        return guardExpected.error();
    }

    auto guard = std::move(guardExpected.value());
    auto error = inner->out(path, inputMetadata, options, obj);
    if (!error.has_value()) {
        guard.markDirty();
    }
    if (auto commit = guard.commit(); !commit) {
        return commit.error();
    }
    return error;
}

auto UndoableSpace::undo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    if (auto journal = findJournalRoot(root); journal) {
        return applyJournalSteps(journal, steps, true);
    }
    return applyHistorySteps(root, steps, true);
}

auto UndoableSpace::redo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    if (auto journal = findJournalRoot(root); journal) {
        return applyJournalSteps(journal, steps, false);
    }
    return applyHistorySteps(root, steps, false);
}

auto UndoableSpace::trimHistory(ConcretePathStringView root, TrimPredicate predicate)
    -> Expected<TrimStats> {
    if (auto journal = findJournalRoot(root); journal) {
        return std::unexpected(journalNotReadyError(journal->rootPath));
    }
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
            if (state->persistenceEnabled && entry.persisted) {
                removeEntryFiles(*state, entry.snapshot.generation);
            }
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

    updateTrimTelemetryLocked(*state, stats);

    scope.setResult(true, "trimmed=" + std::to_string(stats.entriesRemoved));
    auto finalizeResult = finalizeHistoryMutation(*state);
    if (!finalizeResult)
        return std::unexpected(finalizeResult.error());
    return stats;
}

auto UndoableSpace::getHistoryStats(ConcretePathStringView root) const -> Expected<HistoryStats> {
    if (auto journal = findJournalRoot(root); journal) {
        std::unique_lock lock(journal->mutex);
        return gatherJournalStatsLocked(*journal);
    }
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::scoped_lock lock(state->mutex);
    return gatherStatsLocked(*state);
}
auto UndoableSpace::readHistoryStatsValue(HistoryStats const& stats,
                                          std::optional<std::size_t> headGeneration,
                                          std::string const& relativePath,
                                          InputMetadata const& metadata,
                                          void* obj) const -> std::optional<Error> {
    auto assign = [&](auto const& value, std::string_view descriptor) -> std::optional<Error> {
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

    struct FieldHandler {
        std::string_view                      path;
        std::function<std::optional<Error>()> apply;
    };

    const std::array<FieldHandler, 16> simpleHandlers{{
        {UndoPaths::HistoryStats, [&] { return assign(stats, UndoPaths::HistoryStats); }},
        {UndoPaths::HistoryStatsUndoCount, [&] { return assign(stats.counts.undo, UndoPaths::HistoryStatsUndoCount); }},
        {UndoPaths::HistoryStatsRedoCount, [&] { return assign(stats.counts.redo, UndoPaths::HistoryStatsRedoCount); }},
        {UndoPaths::HistoryStatsUndoBytes, [&] { return assign(stats.bytes.undo, UndoPaths::HistoryStatsUndoBytes); }},
        {UndoPaths::HistoryStatsRedoBytes, [&] { return assign(stats.bytes.redo, UndoPaths::HistoryStatsRedoBytes); }},
        {UndoPaths::HistoryStatsLiveBytes, [&] { return assign(stats.bytes.live, UndoPaths::HistoryStatsLiveBytes); }},
        {UndoPaths::HistoryStatsBytesRetained, [&] { return assign(stats.bytes.total, UndoPaths::HistoryStatsBytesRetained); }},
        {UndoPaths::HistoryStatsManualGcEnabled, [&] { return assign(stats.counts.manualGarbageCollect, UndoPaths::HistoryStatsManualGcEnabled); }},
        {UndoPaths::HistoryStatsTrimOperationCount, [&] { return assign(stats.trim.operationCount, UndoPaths::HistoryStatsTrimOperationCount); }},
        {UndoPaths::HistoryStatsTrimmedEntries, [&] { return assign(stats.trim.entries, UndoPaths::HistoryStatsTrimmedEntries); }},
        {UndoPaths::HistoryStatsTrimmedBytes, [&] { return assign(stats.trim.bytes, UndoPaths::HistoryStatsTrimmedBytes); }},
        {UndoPaths::HistoryStatsLastTrimTimestamp, [&] { return assign(stats.trim.lastTimestampMs, UndoPaths::HistoryStatsLastTrimTimestamp); }},
        {UndoPaths::HistoryUnsupported, [&] { return assign(stats.unsupported, UndoPaths::HistoryUnsupported); }},
        {UndoPaths::HistoryUnsupportedTotalCount, [&] { return assign(stats.unsupported.total, UndoPaths::HistoryUnsupportedTotalCount); }},
        {UndoPaths::HistoryUnsupportedRecentCount, [&] {
             return assign(stats.unsupported.recent.size(), UndoPaths::HistoryUnsupportedRecentCount);
         }},
        {UndoPaths::HistoryHeadGeneration,
         [&] {
             if (!headGeneration) {
                 return std::optional<Error>{
                     Error{Error::Code::NoObjectFound, "History head generation unavailable"}};
             }
             return assign(*headGeneration, UndoPaths::HistoryHeadGeneration);
         }},
    }};

    for (auto const& handler : simpleHandlers) {
        if (relativePath == handler.path) {
            return handler.apply();
        }
    }

    if (relativePath.starts_with(UndoPaths::HistoryLastOperationPrefix)) {
        if (!stats.lastOperation) {
            return Error{Error::Code::NoObjectFound, "No history operation recorded"};
        }
        auto const& op = *stats.lastOperation;
        const std::array<FieldHandler, 11> operationHandlers{{
            {UndoPaths::HistoryLastOperationType, [&] { return assign(op.type, UndoPaths::HistoryLastOperationType); }},
            {UndoPaths::HistoryLastOperationTimestamp, [&] { return assign(op.timestampMs, UndoPaths::HistoryLastOperationTimestamp); }},
            {UndoPaths::HistoryLastOperationDuration, [&] { return assign(op.durationMs, UndoPaths::HistoryLastOperationDuration); }},
            {UndoPaths::HistoryLastOperationSuccess, [&] { return assign(op.success, UndoPaths::HistoryLastOperationSuccess); }},
            {UndoPaths::HistoryLastOperationUndoBefore, [&] { return assign(op.undoCountBefore, UndoPaths::HistoryLastOperationUndoBefore); }},
            {UndoPaths::HistoryLastOperationUndoAfter, [&] { return assign(op.undoCountAfter, UndoPaths::HistoryLastOperationUndoAfter); }},
            {UndoPaths::HistoryLastOperationRedoBefore, [&] { return assign(op.redoCountBefore, UndoPaths::HistoryLastOperationRedoBefore); }},
            {UndoPaths::HistoryLastOperationRedoAfter, [&] { return assign(op.redoCountAfter, UndoPaths::HistoryLastOperationRedoAfter); }},
            {UndoPaths::HistoryLastOperationBytesBefore, [&] { return assign(op.bytesBefore, UndoPaths::HistoryLastOperationBytesBefore); }},
            {UndoPaths::HistoryLastOperationBytesAfter, [&] { return assign(op.bytesAfter, UndoPaths::HistoryLastOperationBytesAfter); }},
            {UndoPaths::HistoryLastOperationMessage, [&] { return assign(op.message, UndoPaths::HistoryLastOperationMessage); }},
        }};
        for (auto const& handler : operationHandlers) {
            if (relativePath == handler.path) {
                return handler.apply();
            }
        }
    }

    if (relativePath.starts_with(UndoPaths::HistoryUnsupportedRecentPrefix)) {
        auto parseIndex = [](std::string_view value) -> std::optional<std::size_t> {
            std::size_t index = 0;
            auto        begin = value.data();
            auto        end   = value.data() + value.size();
            auto        res   = std::from_chars(begin, end, index);
            if (res.ec != std::errc{} || res.ptr != end) {
                return std::nullopt;
            }
            return index;
        };

        std::string_view suffix =
            std::string_view(relativePath).substr(UndoPaths::HistoryUnsupportedRecentPrefix.size());
        auto slash = suffix.find('/');
        auto indexView = suffix.substr(0, slash);
        auto indexOpt  = parseIndex(indexView);
        if (!indexOpt) {
            return Error{Error::Code::InvalidPath, "Unsupported history record index"};
        }
        auto index = *indexOpt;
        if (index >= stats.unsupported.recent.size()) {
            return Error{Error::Code::NoObjectFound, "Unsupported history record not found"};
        }
        auto const& record = stats.unsupported.recent[index];
        if (slash == std::string_view::npos) {
            return assign(record, relativePath);
        }
        auto field = suffix.substr(slash + 1);
        if (field == "path") {
            return assign(record.path, relativePath);
        }
        if (field == "reason") {
            return assign(record.reason, relativePath);
        }
        if (field == "occurrences") {
            return assign(record.occurrences, relativePath);
        }
        if (field == "timestampMs") {
            return assign(record.lastTimestampMs, relativePath);
        }
    }

    return Error{Error::Code::NotFound,
                 std::string("Unsupported history telemetry path: ") + relativePath};
}

} // namespace SP::History
