#include "history/UndoableSpace.hpp"

#include "history/UndoHistoryUtils.hpp"
#include "history/UndoableSpaceState.hpp"

#include "core/InsertReturn.hpp"
#include "core/Node.hpp"
#include "core/NodeData.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <typeinfo>

namespace {

using SP::Error;
using SP::Expected;
namespace UndoUtilsAlias = SP::History::UndoUtils;
namespace UndoPaths      = SP::History::UndoUtils::Paths;
namespace UndoJournal    = SP::History::UndoJournal;

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
        std::scoped_lock payloadLock(node.payloadMutex);
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

auto UndoableSpace::computeJournalLiveBytes(UndoJournalRootState const& state) const
    -> std::size_t {
    auto* rootNode = const_cast<UndoableSpace*>(this)->resolveRootNode();
    if (!rootNode) {
        return 0;
    }

    SP::Node const* node = rootNode;
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

auto UndoableSpace::computeJournalByteMetrics(UndoJournalRootState const& state) const
    -> Expected<JournalByteMetrics> {
    auto stats = state.journal.stats();
    JournalByteMetrics metrics{};
    metrics.undoBytes = stats.undoBytes;
    metrics.redoBytes = stats.redoBytes;
    metrics.liveBytes = state.liveBytes;
    return metrics;
}

void UndoableSpace::recordJournalUnsupportedPayload(UndoJournalRootState& state,
                                                    std::string_view path,
                                                    std::string_view reason) {
    auto now = std::chrono::system_clock::now();
    state.telemetry.unsupportedTotal += 1;

    auto& log = state.telemetry.unsupportedLog;
    auto  it  = std::find_if(log.begin(), log.end(), [&](auto const& entry) {
        return entry.path == path && entry.reason == reason;
    });
    if (it != log.end()) {
        it->occurrences += 1;
        it->timestamp    = now;
        if (std::next(it) != log.end()) {
            auto updated = std::move(*it);
            log.erase(it);
            log.push_back(std::move(updated));
        }
        return;
    }

    HistoryTelemetry::UnsupportedRecord record;
    record.path        = std::string(path);
    record.reason      = std::string(reason);
    record.timestamp   = now;
    record.occurrences = 1;
    log.push_back(std::move(record));
    if (log.size() > UndoUtilsAlias::MaxUnsupportedLogEntries) {
        log.erase(log.begin());
    }
}

auto UndoableSpace::parseJournalRelativeComponents(UndoJournalRootState const& state,
                                                   std::string_view fullPath) const
    -> Expected<std::vector<std::string>> {
    ConcretePathStringView pathView{fullPath};
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
        return std::unexpected(Error{Error::Code::InvalidPermissions,
                                     "Journal entry path outside history root"});
    }
    for (std::size_t i = 0; i < state.components.size(); ++i) {
        if (components[i] != state.components[i]) {
            return std::unexpected(Error{Error::Code::InvalidPermissions,
                                         "Journal entry path outside history root"});
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

    SP::Node const* node = rootNode;
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
    if (node->data && node->data->hasNestedSpaces()) {
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

    SP::Node* node = rootNode;
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

auto UndoableSpace::interpretSteps(InputData const& data) const -> std::size_t {
    if (!data.metadata.typeInfo || data.obj == nullptr) {
        return 1;
    }

    auto interpretUnsigned = [&](auto ptr) -> std::size_t {
        using T = std::remove_pointer_t<decltype(ptr)>;
        if (*ptr <= 0) {
            return 1;
        }
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

auto UndoableSpace::applyJournalSteps(std::shared_ptr<UndoJournalRootState> const& statePtr,
                                      std::size_t steps,
                                      bool        isUndo) -> Expected<void> {
    if (!statePtr) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }

    auto state = statePtr;
    std::unique_lock lock(state->mutex);
    auto const currentThread = std::this_thread::get_id();
    state->transactionCv.wait(lock, [&] {
        return !state->activeTransaction
               || state->activeTransaction->owner == currentThread;
    });
    if (state->activeTransaction && state->activeTransaction->owner == currentThread) {
        auto const busyMessage = isUndo ? "Cannot undo while transaction open"
                                        : "Cannot redo while transaction open";
        return std::unexpected(Error{Error::Code::InvalidPermissions, busyMessage});
    }

    if (steps == 0) {
        steps = 1;
    }

    auto const operationName = isUndo ? std::string_view{"undo"}
                                      : std::string_view{"redo"};
    auto const emptyMessage  = isUndo ? std::string_view{"Nothing to undo"}
                                      : std::string_view{"Nothing to redo"};

    while (steps-- > 0) {
        auto step = performJournalStep(*state, isUndo, operationName, emptyMessage);
        if (!step) {
            return step;
        }
    }

    state->stateDirty       = true;
    state->persistenceDirty = state->persistenceDirty || state->persistenceEnabled;
    return {};
}

auto UndoableSpace::performJournalStep(UndoJournalRootState& state,
                                       bool                  sourceIsUndo,
                                       std::string_view      operationName,
                                       std::string_view      emptyMessage) -> Expected<void> {
    JournalOperationScope scope(*this, state, operationName);

    auto entryOpt = sourceIsUndo ? state.journal.undo() : state.journal.redo();
    if (!entryOpt.has_value()) {
        scope.setResult(false, "empty");
        return std::unexpected(Error{Error::Code::NoObjectFound,
                                     std::string(emptyMessage)});
    }
    auto const& entry = entryOpt->get();
    scope.setTag(entry.tag);

    auto componentsExpected = parseJournalRelativeComponents(state, entry.path);
    if (!componentsExpected) {
        scope.setResult(false, "path_invalid");
        return std::unexpected(componentsExpected.error());
    }
    auto relativeComponents = std::move(componentsExpected.value());

    auto decodePayload = [&](UndoJournal::SerializedPayload const& payload,
                             char const*                          context)
        -> Expected<std::optional<NodeData>> {
        if (!payload.present) {
            return std::optional<NodeData>{};
        }
        auto decoded = UndoJournal::decodeNodeDataPayload(payload);
        if (!decoded) {
            scope.setResult(false, context);
            return std::unexpected(decoded.error());
        }
        return std::optional<NodeData>{decoded.value()};
    };

    auto payloadExpected = sourceIsUndo ? decodePayload(entry.inverseValue, "decode_inverse_failed")
                                        : decodePayload(entry.value, "decode_value_failed");
    if (!payloadExpected) {
        return std::unexpected(payloadExpected.error());
    }

    auto applyResult = applyJournalNodeData(state, relativeComponents, payloadExpected.value());
    if (!applyResult) {
        scope.setResult(false, applyResult.error().message.value_or("apply_failed"));
        return applyResult;
    }

    scope.setResult(true);
    return {};
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
        JournalOperationScope scope(*this, *state, "garbage_collect", state->currentTag);
        auto const currentThread = std::this_thread::get_id();
        state->transactionCv.wait(lock, [&] {
            return !state->activeTransaction
                   || state->activeTransaction->owner == currentThread;
        });
        if (state->activeTransaction && state->activeTransaction->owner == currentThread) {
            scope.setResult(false, "transaction_active");
            ret.errors.push_back(Error{Error::Code::InvalidPermissions,
                                       "Cannot garbage collect while transaction open"});
            return ret;
        }

        auto beforeStats = state->journal.stats();
        auto policy      = state->journal.policy();
        state->journal.setRetentionPolicy(policy);
        auto afterStats   = state->journal.stats();
        auto trimmedEntries = afterStats.trimmedEntries >= beforeStats.trimmedEntries
                                   ? afterStats.trimmedEntries - beforeStats.trimmedEntries
                                   : std::size_t{0};
        auto trimmedBytes = afterStats.trimmedBytes >= beforeStats.trimmedBytes
                                  ? afterStats.trimmedBytes - beforeStats.trimmedBytes
                                  : std::size_t{0};

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
                auto compact = compactJournalPersistence(*state, true);
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

        state->stateDirty = true;
        if (state->persistenceEnabled && compactFailed) {
            state->persistenceDirty           = true;
            state->telemetry.persistenceDirty = true;
        }
        return ret;
    }
    if (command == UndoPaths::CommandSetTag) {
        if (!data.obj || !data.metadata.typeInfo
            || *data.metadata.typeInfo != typeid(std::string)
            || data.metadata.deserialize == nullptr) {
            ret.errors.push_back(
                Error{Error::Code::InvalidType, "History tag expects std::string payload"});
            return ret;
        }

        auto const& tagValue = *static_cast<std::string const*>(data.obj);
        std::scoped_lock lock(state->mutex);
        JournalOperationScope scope(*this, *state, "set_tag", tagValue);
        state->currentTag = tagValue;
        state->stateDirty = true;
        scope.setResult(true);
        return ret;
    }
    if (command == UndoPaths::CommandSetManualGc) {
        bool manual = false;
        if (data.obj && data.metadata.typeInfo && *data.metadata.typeInfo == typeid(bool)) {
            manual = *static_cast<bool const*>(data.obj);
        }
        std::scoped_lock lock(state->mutex);
        JournalOperationScope scope(*this, *state, "set_manual_gc", state->currentTag);
        state->options.manualGarbageCollect = manual;
        state->stateDirty                   = true;
        state->persistenceDirty             = state->persistenceEnabled
                                               && (state->persistenceDirty || !manual);
        scope.setResult(true, manual ? "enabled" : "disabled");
        return ret;
    }

    ret.errors.push_back(
        Error{Error::Code::UnknownError, "Unsupported history control command"});
    return ret;
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
        std::string                           path;
        std::function<std::optional<Error>()> apply;
    };

    const std::array<FieldHandler, 28> simpleHandlers{ {
        {std::string(UndoPaths::HistoryStats), [&] { return assign(stats, UndoPaths::HistoryStats); }},
        {std::string(UndoPaths::HistoryStatsUndoCount),
         [&] { return assign(stats.counts.undo, UndoPaths::HistoryStatsUndoCount); }},
        {std::string(UndoPaths::HistoryStatsRedoCount),
         [&] { return assign(stats.counts.redo, UndoPaths::HistoryStatsRedoCount); }},
        {std::string(UndoPaths::HistoryStatsUndoBytes),
         [&] { return assign(stats.bytes.undo, UndoPaths::HistoryStatsUndoBytes); }},
        {std::string(UndoPaths::HistoryStatsRedoBytes),
         [&] { return assign(stats.bytes.redo, UndoPaths::HistoryStatsRedoBytes); }},
        {std::string(UndoPaths::HistoryStatsLiveBytes),
         [&] { return assign(stats.bytes.live, UndoPaths::HistoryStatsLiveBytes); }},
        {std::string(UndoPaths::HistoryStatsBytesRetained),
         [&] { return assign(stats.bytes.total, UndoPaths::HistoryStatsBytesRetained); }},
        {std::string(UndoPaths::HistoryStatsManualGcEnabled),
         [&] { return assign(stats.counts.manualGarbageCollect, UndoPaths::HistoryStatsManualGcEnabled); }},
        {std::string(UndoPaths::HistoryStatsLimits),
         [&] { return assign(stats.limits, UndoPaths::HistoryStatsLimits); }},
        {std::string(UndoPaths::HistoryStatsLimitsMaxEntries),
         [&] { return assign(stats.limits.maxEntries, UndoPaths::HistoryStatsLimitsMaxEntries); }},
        {std::string(UndoPaths::HistoryStatsLimitsMaxBytesRetained),
         [&] {
             return assign(stats.limits.maxBytesRetained,
                           UndoPaths::HistoryStatsLimitsMaxBytesRetained);
         }},
        {std::string(UndoPaths::HistoryStatsLimitsKeepLatestForMs),
         [&] {
             return assign(stats.limits.keepLatestForMs,
                           UndoPaths::HistoryStatsLimitsKeepLatestForMs);
         }},
        {std::string(UndoPaths::HistoryStatsLimitsRamCacheEntries),
         [&] {
             return assign(stats.limits.ramCacheEntries,
                           UndoPaths::HistoryStatsLimitsRamCacheEntries);
         }},
        {std::string(UndoPaths::HistoryStatsLimitsMaxDiskBytes),
         [&] {
             return assign(stats.limits.maxDiskBytes, UndoPaths::HistoryStatsLimitsMaxDiskBytes);
         }},
        {std::string(UndoPaths::HistoryStatsLimitsPersistHistory),
         [&] {
             return assign(stats.limits.persistHistory,
                           UndoPaths::HistoryStatsLimitsPersistHistory);
         }},
        {std::string(UndoPaths::HistoryStatsLimitsRestoreFromPersistence),
         [&] {
             return assign(stats.limits.restoreFromPersistence,
                           UndoPaths::HistoryStatsLimitsRestoreFromPersistence);
         }},
        {std::string(UndoPaths::HistoryStatsTrimOperationCount),
         [&] { return assign(stats.trim.operationCount, UndoPaths::HistoryStatsTrimOperationCount); }},
        {std::string(UndoPaths::HistoryStatsTrimmedEntries),
         [&] { return assign(stats.trim.entries, UndoPaths::HistoryStatsTrimmedEntries); }},
        {std::string(UndoPaths::HistoryStatsTrimmedBytes),
         [&] { return assign(stats.trim.bytes, UndoPaths::HistoryStatsTrimmedBytes); }},
        {std::string(UndoPaths::HistoryStatsLastTrimTimestamp),
         [&] { return assign(stats.trim.lastTimestampMs, UndoPaths::HistoryStatsLastTrimTimestamp); }},
        {std::string(UndoPaths::HistoryStatsCompactionRuns),
         [&] { return assign(stats.trim.operationCount, UndoPaths::HistoryStatsCompactionRuns); }},
        {std::string(UndoPaths::HistoryStatsCompactionEntries),
         [&] { return assign(stats.trim.entries, UndoPaths::HistoryStatsCompactionEntries); }},
        {std::string(UndoPaths::HistoryStatsCompactionBytes),
         [&] { return assign(stats.trim.bytes, UndoPaths::HistoryStatsCompactionBytes); }},
        {std::string(UndoPaths::HistoryStatsCompactionLastTimestamp),
         [&] {
             return assign(stats.trim.lastTimestampMs, UndoPaths::HistoryStatsCompactionLastTimestamp);
         }},
        {std::string(UndoPaths::HistoryUnsupported),
         [&] { return assign(stats.unsupported, UndoPaths::HistoryUnsupported); }},
        {std::string(UndoPaths::HistoryUnsupportedTotalCount),
         [&] { return assign(stats.unsupported.total, UndoPaths::HistoryUnsupportedTotalCount); }},
        {std::string(UndoPaths::HistoryUnsupportedRecentCount),
         [&] {
             return assign(stats.unsupported.recent.size(), UndoPaths::HistoryUnsupportedRecentCount);
         }},
        {std::string(UndoPaths::HistoryHeadGeneration),
         [&] {
             if (!headGeneration) {
                 return std::optional<Error>{
                     Error{Error::Code::NoObjectFound, "History head generation unavailable"}};
             }
             return assign(*headGeneration, UndoPaths::HistoryHeadGeneration);
         }},
    } };

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
        const std::array<FieldHandler, 12> operationHandlers{ {
            {std::string(UndoPaths::HistoryLastOperationType),
             [&] { return assign(op.type, UndoPaths::HistoryLastOperationType); }},
            {std::string(UndoPaths::HistoryLastOperationTimestamp),
             [&] { return assign(op.timestampMs, UndoPaths::HistoryLastOperationTimestamp); }},
            {std::string(UndoPaths::HistoryLastOperationDuration),
             [&] { return assign(op.durationMs, UndoPaths::HistoryLastOperationDuration); }},
            {std::string(UndoPaths::HistoryLastOperationSuccess),
             [&] { return assign(op.success, UndoPaths::HistoryLastOperationSuccess); }},
            {std::string(UndoPaths::HistoryLastOperationUndoBefore),
             [&] { return assign(op.undoCountBefore, UndoPaths::HistoryLastOperationUndoBefore); }},
            {std::string(UndoPaths::HistoryLastOperationUndoAfter),
             [&] { return assign(op.undoCountAfter, UndoPaths::HistoryLastOperationUndoAfter); }},
            {std::string(UndoPaths::HistoryLastOperationRedoBefore),
             [&] { return assign(op.redoCountBefore, UndoPaths::HistoryLastOperationRedoBefore); }},
            {std::string(UndoPaths::HistoryLastOperationRedoAfter),
             [&] { return assign(op.redoCountAfter, UndoPaths::HistoryLastOperationRedoAfter); }},
            {std::string(UndoPaths::HistoryLastOperationBytesBefore),
             [&] { return assign(op.bytesBefore, UndoPaths::HistoryLastOperationBytesBefore); }},
            {std::string(UndoPaths::HistoryLastOperationBytesAfter),
             [&] { return assign(op.bytesAfter, UndoPaths::HistoryLastOperationBytesAfter); }},
            {std::string(UndoPaths::HistoryLastOperationMessage),
             [&] { return assign(op.message, UndoPaths::HistoryLastOperationMessage); }},
            {std::string(UndoPaths::HistoryLastOperationTag),
             [&] { return assign(op.tag, UndoPaths::HistoryLastOperationTag); }},
        } };
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

        std::string_view suffix{relativePath};
        suffix.remove_prefix(UndoPaths::HistoryUnsupportedRecentPrefix.size());
        auto index = parseIndex(suffix);
        if (!index || *index >= stats.unsupported.recent.size()) {
            return Error{Error::Code::NoObjectFound, "Unsupported payload index out of range"};
        }
        auto const& entry = stats.unsupported.recent[*index];
        auto base = std::string(UndoPaths::HistoryUnsupportedRecentPrefix) + std::to_string(*index);
        const std::array<FieldHandler, 4> unsupportedHandlers{ {
            {base + "/path", [&] { return assign(entry.path, "unsupported/path"); }},
            {base + "/reason", [&] { return assign(entry.reason, "unsupported/reason"); }},
            {base + "/occurrences", [&] { return assign(entry.occurrences, "unsupported/occurrences"); }},
            {base + "/timestampMs", [&] { return assign(entry.lastTimestampMs, "unsupported/timestamp"); }},
        } };
        for (auto const& handler : unsupportedHandlers) {
            if (relativePath == handler.path) {
                return handler.apply();
            }
        }
    }

    return Error{Error::Code::NoObjectFound, "History telemetry path not found"};
}

auto UndoableSpace::readDiagnosticsHistoryValue(MatchedJournalRoot const& matchedRoot,
                                                std::string const& relativePath,
                                                InputMetadata const& metadata,
                                                void* obj) -> std::optional<Error> {
    auto state = matchedRoot.state;
    if (!state) {
        return Error{Error::Code::UnknownError, "History root missing"};
    }

    auto assign = [&](auto const& value, std::string_view descriptor) -> std::optional<Error> {
        using ValueT = std::decay_t<decltype(value)>;
        if (!metadata.typeInfo || *metadata.typeInfo != typeid(ValueT)) {
            return Error{Error::Code::InvalidType,
                         std::string("History diagnostics path ") + std::string(descriptor)
                             + " expects type " + typeid(ValueT).name()};
        }
        if (obj == nullptr) {
            return Error{Error::Code::MalformedInput, "Output pointer is null"};
        }
        *static_cast<ValueT*>(obj) = value;
        return std::nullopt;
    };

    auto parseUint64 = [](std::string_view value) -> std::optional<std::uint64_t> {
        std::uint64_t parsed = 0;
        auto          res    = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (res.ec != std::errc{} || res.ptr != value.data() + value.size()) {
            return std::nullopt;
        }
        return parsed;
    };

    std::unique_lock lock(state->mutex);
    auto stats = gatherJournalStatsLocked(*state);
    auto headGeneration = std::optional<std::size_t>{static_cast<std::size_t>(
        std::min<std::uint64_t>(state->nextSequence,
                                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))) };

    if (relativePath == UndoPaths::HistoryDiagnosticsHeadSequence) {
        std::uint64_t headSeq = state->nextSequence == 0 ? 0 : state->nextSequence - 1;
        return assign(headSeq, UndoPaths::HistoryDiagnosticsHeadSequence);
    }

    if (relativePath.starts_with(UndoPaths::HistoryDiagnosticsEntriesPrefix)) {
        std::string_view remaining{relativePath};
        remaining.remove_prefix(std::string_view{UndoPaths::HistoryDiagnosticsEntriesPrefix}.size());
        auto slashPos = remaining.find('/');
        if (slashPos == std::string_view::npos) {
            return Error{Error::Code::NoObjectFound, "Missing history entry field"};
        }
        auto seqView   = remaining.substr(0, slashPos);
        auto fieldView = remaining.substr(slashPos + 1);
        auto sequence  = parseUint64(seqView);
        if (!sequence) {
            return Error{Error::Code::InvalidPath, "Invalid history entry sequence"};
        }

        UndoJournal::JournalEntry const* entryPtr = nullptr;
        for (std::size_t i = 0; i < state->journal.size(); ++i) {
            auto const& entry = state->journal.entryAt(i);
            if (entry.sequence == *sequence) {
                entryPtr = &entry;
                break;
            }
        }
        if (!entryPtr) {
            return Error{Error::Code::NoObjectFound, "History entry not found"};
        }

        auto const& entry = *entryPtr;
        auto const  opStr = entry.operation == UndoJournal::OperationKind::Insert ? "insert" : "take";

        if (fieldView == UndoPaths::HistoryDiagnosticsEntryOperation) {
            return assign(std::string(opStr), UndoPaths::HistoryDiagnosticsEntryOperation);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryPath) {
            return assign(entry.path, UndoPaths::HistoryDiagnosticsEntryPath);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryTag) {
            return assign(entry.tag, UndoPaths::HistoryDiagnosticsEntryTag);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryTimestamp) {
            return assign(entry.timestampMs, UndoPaths::HistoryDiagnosticsEntryTimestamp);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryMonotonic) {
            return assign(entry.monotonicNs, UndoPaths::HistoryDiagnosticsEntryMonotonic);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntrySequence) {
            return assign(entry.sequence, UndoPaths::HistoryDiagnosticsEntrySequence);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryBarrier) {
            return assign(entry.barrier, UndoPaths::HistoryDiagnosticsEntryBarrier);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryValueBytes) {
            return assign(entry.value.bytes.size(), UndoPaths::HistoryDiagnosticsEntryValueBytes);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryInverseBytes) {
            return assign(entry.inverseValue.bytes.size(), UndoPaths::HistoryDiagnosticsEntryInverseBytes);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryHasValue) {
            return assign(entry.value.present, UndoPaths::HistoryDiagnosticsEntryHasValue);
        }
        if (fieldView == UndoPaths::HistoryDiagnosticsEntryHasInverse) {
            return assign(entry.inverseValue.present, UndoPaths::HistoryDiagnosticsEntryHasInverse);
        }

        return Error{Error::Code::NoObjectFound, "History entry field not found"};
    }

    std::string mapped = std::string(UndoPaths::HistoryRoot);
    if (!relativePath.empty()) {
        mapped.push_back('/');
        mapped.append(relativePath);
    }
    return readHistoryStatsValue(stats, headGeneration, mapped, metadata, obj);
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
    auto head  = std::optional<std::size_t>{static_cast<std::size_t>(std::min<std::uint64_t>(
        state->nextSequence, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))) };
    return readHistoryStatsValue(stats, head, relativePath, metadata, obj);
}

auto UndoableSpace::gatherJournalStatsLocked(UndoJournalRootState const& state) const
    -> HistoryStats {
    HistoryStats stats;
    auto         journalStats = state.journal.stats();
    auto         metricsExpected = computeJournalByteMetrics(state);

    std::size_t undoBytes = journalStats.undoBytes;
    std::size_t redoBytes = journalStats.redoBytes;
    std::size_t liveBytes = state.liveBytes;
    if (metricsExpected) {
        undoBytes = metricsExpected->undoBytes;
        redoBytes = metricsExpected->redoBytes;
        liveBytes = metricsExpected->liveBytes;
    }

    stats.counts.undo = journalStats.undoCount;
    stats.counts.redo = journalStats.redoCount;

    stats.bytes.undo  = undoBytes;
    stats.bytes.redo  = redoBytes;
    stats.bytes.live  = liveBytes;
    stats.bytes.total = undoBytes + redoBytes + liveBytes;
    stats.bytes.disk  = state.telemetry.diskBytes;

    stats.limits.maxEntries             = state.options.maxEntries;
    stats.limits.maxBytesRetained       = state.options.maxBytesRetained;
    stats.limits.maxDiskBytes           = state.options.maxDiskBytes;
    stats.limits.ramCacheEntries        = state.options.ramCacheEntries;
    stats.limits.keepLatestForMs        = static_cast<std::uint64_t>(state.options.keepLatestFor.count());
    stats.limits.persistHistory         = state.options.persistHistory;
    stats.limits.restoreFromPersistence = state.options.restoreFromPersistence;

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
        HistoryOperationRecord const& record = *state.telemetry.lastOperation;
        HistoryLastOperation op;
        op.type            = record.type;
        op.timestampMs     = UndoUtilsAlias::toMillis(record.timestamp);
        op.durationMs      = static_cast<std::uint64_t>(record.duration.count());
        op.success         = record.success;
        op.undoCountBefore = record.undoCountBefore;
        op.undoCountAfter  = record.undoCountAfter;
        op.redoCountBefore = record.redoCountBefore;
        op.redoCountAfter  = record.redoCountAfter;
        op.bytesBefore     = record.bytesBefore;
        op.bytesAfter      = record.bytesAfter;
        op.tag             = record.tag;
        op.message         = record.message;
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

auto UndoableSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    auto fullPath       = path.toString();
    auto journalMatched = findJournalRootByPath(fullPath);
    if (!journalMatched.has_value()) {
        return inner->in(path, data);
    }

    auto state = journalMatched->state;
    if (!state) {
        InsertReturn ret;
        ret.errors.push_back(Error{Error::Code::UnknownError, "History root missing"});
        return ret;
    }

    if (journalMatched->diagnostics) {
        InsertReturn ret;
        ret.errors.push_back(Error{Error::Code::InvalidPermissions,
                                   "History diagnostics are read-only"});
        return ret;
    }

    if (!journalMatched->relativePath.empty()
        && journalMatched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
        return handleJournalControlInsert(*journalMatched, journalMatched->relativePath, data);
    }

    auto componentsExpected = parseJournalRelativeComponents(*state, fullPath);
    if (!componentsExpected) {
        InsertReturn ret;
        ret.errors.push_back(componentsExpected.error());
        return ret;
    }
    auto relativeComponents = std::move(componentsExpected.value());

    auto beforeExpected = captureJournalNodeData(*state, relativeComponents);
    if (!beforeExpected) {
        InsertReturn ret;
        ret.errors.push_back(beforeExpected.error());
        return ret;
    }
    auto beforeNode = beforeExpected.value();

    auto guardExpected = beginJournalTransactionInternal(state);
    if (!guardExpected) {
        InsertReturn ret;
        ret.errors.push_back(guardExpected.error());
        return ret;
    }

    auto guard  = std::move(guardExpected.value());
    auto result = inner->in(path, data);
    if (result.errors.empty()) {
        auto afterExpected = captureJournalNodeData(*state, relativeComponents);
        if (!afterExpected) {
            result.errors.push_back(afterExpected.error());
        } else {
            auto record = recordJournalMutation(*state,
                                                UndoJournal::OperationKind::Insert,
                                                fullPath,
                                                afterExpected.value(),
                                                beforeNode);
            if (!record) {
                result.errors.push_back(record.error());
            }
        }
    } else {
        std::scoped_lock stateLock(state->mutex);
        auto now = std::chrono::system_clock::now();
        for (auto const& err : result.errors) {
            if (!err.message) {
                continue;
            }
            HistoryTelemetry::UnsupportedRecord record;
            record.path        = fullPath;
            record.reason      = *err.message;
            record.timestamp   = now;
            record.occurrences = 1;
            auto& log          = state->telemetry.unsupportedLog;
            auto  it           = std::find_if(log.begin(), log.end(), [&](auto const& entry) {
                return entry.path == record.path && entry.reason == record.reason;
            });
            if (it != log.end()) {
                it->occurrences += 1;
                it->timestamp    = now;
            } else {
                log.push_back(record);
                if (log.size() > UndoUtilsAlias::MaxUnsupportedLogEntries) {
                    log.erase(log.begin());
                }
            }
            state->telemetry.unsupportedTotal += 1;
        }
        state->stateDirty = true;
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
    auto journalMatched = findJournalRootByPath(fullPath);

    if (!options.doPop) {
        if (!journalMatched.has_value()) {
            return inner->out(path, inputMetadata, options, obj);
        }
        if (journalMatched->diagnostics) {
            return readDiagnosticsHistoryValue(
                *journalMatched, journalMatched->relativePath, inputMetadata, obj);
        }
        if (!journalMatched->relativePath.empty()
            && journalMatched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
            return readJournalHistoryValue(
                *journalMatched, journalMatched->relativePath, inputMetadata, obj);
        }
        return inner->out(path, inputMetadata, options, obj);
    }

    if (!journalMatched.has_value()) {
        return inner->out(path, inputMetadata, options, obj);
    }

    auto state = journalMatched->state;
    if (!state) {
        return Error{Error::Code::UnknownError, "History root missing"};
    }

    if (journalMatched->diagnostics) {
        return Error{Error::Code::InvalidPermissions, "History diagnostics are read-only"};
    }

    if (!journalMatched->relativePath.empty()
        && journalMatched->relativePath.starts_with(UndoPaths::HistoryRoot)) {
        return Error{Error::Code::InvalidPermissions, "History command does not support take"};
    }

    auto componentsExpected = parseJournalRelativeComponents(*state, fullPath);
    if (!componentsExpected) {
        return componentsExpected.error();
    }
    auto relativeComponents = std::move(componentsExpected.value());

    auto beforeExpected = captureJournalNodeData(*state, relativeComponents);
    if (!beforeExpected) {
        return beforeExpected.error();
    }
    auto beforeNode = beforeExpected.value();

    auto guardExpected = beginJournalTransactionInternal(state);
    if (!guardExpected) {
        return guardExpected.error();
    }

    auto guard = std::move(guardExpected.value());
    auto error = inner->out(path, inputMetadata, options, obj);
    if (!error.has_value()) {
        auto afterExpected = captureJournalNodeData(*state, relativeComponents);
        if (!afterExpected) {
            return afterExpected.error();
        }
        auto record = recordJournalMutation(*state,
                                            UndoJournal::OperationKind::Take,
                                            fullPath,
                                            afterExpected.value(),
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

auto UndoableSpace::undo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    auto state = findJournalRoot(root);
    if (!state) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }
    return applyJournalSteps(state, steps, true);
}

auto UndoableSpace::redo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    auto state = findJournalRoot(root);
    if (!state) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }
    return applyJournalSteps(state, steps, false);
}

auto UndoableSpace::trimHistory(ConcretePathStringView, TrimPredicate)
    -> Expected<TrimStats> {
    return std::unexpected(Error{Error::Code::NotSupported,
                                 "Snapshot-based trim API has been removed"});
}

auto UndoableSpace::getHistoryStats(ConcretePathStringView root) const
    -> Expected<HistoryStats> {
    auto state = findJournalRoot(root);
    if (!state) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }

    std::unique_lock lock(state->mutex);
    state->telemetry.cachedUndo = state->journal.stats().undoCount;
    state->telemetry.cachedRedo = state->journal.stats().redoCount;
    return gatherJournalStatsLocked(*state);
}

} // namespace SP::History
