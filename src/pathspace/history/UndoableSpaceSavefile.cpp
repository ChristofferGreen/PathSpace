#include "history/UndoableSpace.hpp"

#include "history/UndoHistoryUtils.hpp"
#include "history/UndoSavefileCodec.hpp"
#include "history/UndoSnapshotCodec.hpp"
#include "history/UndoableSpaceState.hpp"

#include "core/Node.hpp"
#include "log/TaggedLogger.hpp"

#include <filesystem>

namespace {

using SP::Error;
using SP::Expected;
using SP::History::CowSubtreePrototype;
using SP::History::Detail::forEachHistoryStack;
namespace UndoSnapshotCodec = SP::History::UndoSnapshotCodec;
namespace UndoSavefile      = SP::History::UndoSavefile;
namespace UndoUtilsAlias    = SP::History::UndoUtils;

} // namespace

namespace SP::History {

auto UndoableSpace::exportHistorySavefile(ConcretePathStringView root,
                                          std::filesystem::path const& file,
                                          bool fsyncData) -> Expected<void> {
    auto state = findRoot(root);
    if (!state) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }

    std::unique_lock lock(state->mutex);
    if (state->activeTransaction) {
        return std::unexpected(
            Error{Error::Code::InvalidPermissions, "Cannot export while transaction active"});
    }

    auto ensureCached = [&](auto& stack, bool undoStack) -> Expected<void> {
        for (std::size_t idx = 0; idx < stack.size(); ++idx) {
            if (!stack[idx].cached) {
                auto load = loadEntrySnapshotLocked(*state, idx, undoStack);
                if (!load)
                    return load;
            }
        }
        return Expected<void>{};
    };

    if (auto ensureUndo = ensureCached(state->undoStack, true); !ensureUndo)
        return ensureUndo;
    if (auto ensureRedo = ensureCached(state->redoStack, false); !ensureRedo)
        return ensureRedo;

    UndoSavefile::Document document;
    document.rootPath                   = state->rootPath;
    document.options.maxEntries         = state->options.maxEntries;
    document.options.maxBytesRetained   = state->options.maxBytesRetained;
    document.options.ramCacheEntries    = state->options.ramCacheEntries;
    document.options.maxDiskBytes       = state->options.maxDiskBytes;
    document.options.keepLatestForMs    =
        static_cast<std::uint64_t>(state->options.keepLatestFor.count());
    document.options.manualGarbageCollect = state->options.manualGarbageCollect;

    document.stateMetadata.liveGeneration  = state->liveSnapshot.generation;
    document.stateMetadata.manualGc        = state->options.manualGarbageCollect;
    document.stateMetadata.ramCacheEntries = state->options.ramCacheEntries;
    document.stateMetadata.undoGenerations.clear();
    document.stateMetadata.redoGenerations.clear();
    forEachHistoryStack(*state, [&](auto const& stack, bool isUndo) {
        auto& generations =
            isUndo ? document.stateMetadata.undoGenerations : document.stateMetadata.redoGenerations;
        generations.reserve(stack.size());
        for (auto const& entry : stack) {
            generations.push_back(entry.snapshot.generation);
        }
    });

    UndoSavefile::EntryBlock liveEntry;
    liveEntry.metadata.generation  = state->liveSnapshot.generation;
    liveEntry.metadata.bytes       = state->liveBytes;
    liveEntry.metadata.timestampMs = UndoUtilsAlias::toMillis(std::chrono::system_clock::now());

    auto liveEncoded = UndoSnapshotCodec::encodeSnapshot(state->liveSnapshot);
    if (!liveEncoded)
        return std::unexpected(liveEncoded.error());
    liveEntry.snapshot = std::move(liveEncoded.value());
    document.liveEntry = std::move(liveEntry);

    auto encodeEntry = [](RootState::Entry const& entry) -> Expected<UndoSavefile::EntryBlock> {
        UndoSavefile::EntryBlock block;
        block.metadata.generation  = entry.snapshot.generation;
        block.metadata.bytes       = entry.bytes;
        block.metadata.timestampMs = UndoUtilsAlias::toMillis(entry.timestamp);
        auto encoded = UndoSnapshotCodec::encodeSnapshot(entry.snapshot);
        if (!encoded)
            return std::unexpected(encoded.error());
        block.snapshot = std::move(encoded.value());
        return block;
    };

    document.undoEntries.reserve(state->undoStack.size());
    for (auto const& entry : state->undoStack) {
        auto blockExpected = encodeEntry(entry);
        if (!blockExpected)
            return std::unexpected(blockExpected.error());
        document.undoEntries.push_back(std::move(blockExpected.value()));
    }

    document.redoEntries.reserve(state->redoStack.size());
    for (auto const& entry : state->redoStack) {
        auto blockExpected = encodeEntry(entry);
        if (!blockExpected)
            return std::unexpected(blockExpected.error());
        document.redoEntries.push_back(std::move(blockExpected.value()));
    }

    auto encoded = UndoSavefile::encode(document);
    auto span    = std::span<const std::byte>(encoded.data(), encoded.size());
    auto write   = UndoUtilsAlias::writeFileAtomic(file, span, fsyncData, true);
    if (!write)
        return write;

    return {};
}

auto UndoableSpace::importHistorySavefile(ConcretePathStringView root,
                                          std::filesystem::path const& file,
                                          bool applyOptions) -> Expected<void> {
    auto state = findRoot(root);
    if (!state) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }

    auto bytes = UndoUtilsAlias::readBinaryFile(file);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto documentExpected =
        UndoSavefile::decode(std::span<const std::byte>(bytes->data(), bytes->size()));
    if (!documentExpected)
        return std::unexpected(documentExpected.error());
    auto document = std::move(documentExpected.value());

    if (!document.rootPath.empty() && document.rootPath != state->rootPath) {
        return std::unexpected(Error{Error::Code::InvalidPath, "Savefile root mismatch"});
    }

    std::unique_lock lock(state->mutex);
    if (state->activeTransaction) {
        return std::unexpected(
            Error{Error::Code::InvalidPermissions, "Cannot import while transaction active"});
    }

    auto undoBefore  = state->undoStack.size();
    auto redoBefore  = state->redoStack.size();
    auto bytesBefore = computeTotalBytesLocked(*state);

    state->prototype  = CowSubtreePrototype{};
    state->undoStack.clear();
    state->redoStack.clear();
    state->telemetry  = {};

    if (applyOptions) {
        state->options.maxEntries          = document.options.maxEntries;
        state->options.maxBytesRetained    = document.options.maxBytesRetained;
        state->options.ramCacheEntries     = document.options.ramCacheEntries == 0
                                                 ? state->options.ramCacheEntries
                                                 : document.options.ramCacheEntries;
        state->options.maxDiskBytes        = document.options.maxDiskBytes;
        state->options.keepLatestFor       =
            std::chrono::milliseconds{document.options.keepLatestForMs};
        state->options.manualGarbageCollect = document.options.manualGarbageCollect;
        if (state->options.ramCacheEntries == 0) {
            state->options.ramCacheEntries = 8;
        }
    }

    auto decodeSnapshot = [&](UndoSavefile::EntryBlock const& block)
        -> Expected<CowSubtreePrototype::Snapshot> {
        return UndoSnapshotCodec::decodeSnapshot(
            state->prototype,
            std::span<const std::byte>(block.snapshot.data(), block.snapshot.size()));
    };

    auto liveSnapshotExpected = decodeSnapshot(document.liveEntry);
    if (!liveSnapshotExpected)
        return std::unexpected(liveSnapshotExpected.error());
    auto liveSnapshot = std::move(liveSnapshotExpected.value());

    auto makeEntry = [&](UndoSavefile::EntryBlock const& block) -> Expected<RootState::Entry> {
        auto snapshotExpected = decodeSnapshot(block);
        if (!snapshotExpected)
            return std::unexpected(snapshotExpected.error());
        RootState::Entry entry;
        entry.snapshot  = std::move(snapshotExpected.value());
        entry.bytes     = block.metadata.bytes;
        entry.timestamp = UndoUtilsAlias::fromMillis(block.timestampMs);
        entry.persisted = state->persistenceEnabled;
        entry.cached    = true;
        return entry;
    };

    std::size_t undoBytes = 0;
    state->undoStack.reserve(document.undoEntries.size());
    for (auto const& block : document.undoEntries) {
        auto entryExpected = makeEntry(block);
        if (!entryExpected)
            return std::unexpected(entryExpected.error());
        undoBytes += block.metadata.bytes;
        state->undoStack.push_back(std::move(entryExpected.value()));
    }

    std::size_t redoBytes = 0;
    state->redoStack.reserve(document.redoEntries.size());
    for (auto const& block : document.redoEntries) {
        auto entryExpected = makeEntry(block);
        if (!entryExpected)
            return std::unexpected(entryExpected.error());
        redoBytes += block.metadata.bytes;
        state->redoStack.push_back(std::move(entryExpected.value()));
    }

    state->liveSnapshot = std::move(liveSnapshot);
    state->liveBytes    = document.liveEntry.metadata.bytes;

    auto applyLive = applySnapshotLocked(*state, state->liveSnapshot);
    if (!applyLive)
        return applyLive;

    state->telemetry.undoBytes = undoBytes;
    state->telemetry.redoBytes = redoBytes;

    std::size_t maxGeneration = state->liveSnapshot.generation;
    auto updateMax = [&](UndoSavefile::EntryBlock const& block) {
        if (block.metadata.generation > maxGeneration)
            maxGeneration = block.metadata.generation;
    };
    updateMax(document.liveEntry);
    for (auto const& block : document.undoEntries)
        updateMax(block);
    for (auto const& block : document.redoEntries)
        updateMax(block);
    state->prototype.setNextGeneration(maxGeneration + 1);

    if (!state->options.manualGarbageCollect) {
        auto trimStats = applyRetentionLocked(*state, "import");
        (void)trimStats;
    }

    applyRamCachePolicyLocked(*state);

    state->stateDirty = state->persistenceEnabled;
    if (state->persistenceEnabled) {
        auto persist = persistStacksLocked(*state, true);
        if (!persist)
            return persist;
    } else {
        updateDiskTelemetryLocked(*state);
    }

    auto now = std::chrono::system_clock::now();
    RootState::OperationRecord record;
    record.type            = "import";
    record.timestamp       = now;
    record.duration        = std::chrono::milliseconds{0};
    record.success         = true;
    record.undoCountBefore = undoBefore;
    record.undoCountAfter  = state->undoStack.size();
    record.redoCountBefore = redoBefore;
    record.redoCountAfter  = state->redoStack.size();
    record.bytesBefore     = bytesBefore;
    record.bytesAfter      = computeTotalBytesLocked(*state);
    record.message         = "savefile_import";
    state->telemetry.lastOperation = std::move(record);

    return {};
}

} // namespace SP::History
