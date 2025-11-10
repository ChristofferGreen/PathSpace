#include "history/UndoableSpace.hpp"

#include "history/UndoHistoryUtils.hpp"
#include "history/UndoSavefileCodec.hpp"
#include "history/UndoableSpaceState.hpp"

#include <chrono>
#include <filesystem>
#include <span>
#include <vector>

namespace {

using SP::Error;
using SP::Expected;
namespace UndoJournal    = SP::History::UndoJournal;
namespace UndoSavefile   = SP::History::UndoSavefile;
namespace UndoUtilsAlias = SP::History::UndoUtils;

} // namespace

namespace SP::History {

auto UndoableSpace::exportHistorySavefile(ConcretePathStringView root,
                                          std::filesystem::path const& file,
                                          bool fsyncData) -> Expected<void> {
    if (auto state = findJournalRoot(root); state) {
        std::unique_lock lock(state->mutex);
        if (state->activeTransaction) {
            return std::unexpected(Error{Error::Code::InvalidPermissions,
                                         "Cannot export while transaction active"});
        }

        UndoSavefile::Document document;
        document.rootPath               = state->rootPath;
        document.options.maxEntries     = state->options.maxEntries;
        document.options.maxBytesRetained = state->options.maxBytesRetained;
        document.options.maxDiskBytes   = state->options.maxDiskBytes;
        document.options.keepLatestForMs = static_cast<std::uint64_t>(state->options.keepLatestFor.count());
        document.options.manualGarbageCollect = state->options.manualGarbageCollect;
        document.nextSequence            = state->nextSequence;
        document.undoCount               = state->journal.cursor();

        document.entries.reserve(state->journal.size());
        for (std::size_t idx = 0; idx < state->journal.size(); ++idx) {
            document.entries.push_back(state->journal.entryAt(idx));
        }

        lock.unlock();

        auto encodedExpected = UndoSavefile::encode(document);
        if (!encodedExpected)
            return std::unexpected(encodedExpected.error());

        auto const& encoded = encodedExpected.value();
        auto span           = std::span<const std::byte>(encoded.data(), encoded.size());
        auto write = UndoUtilsAlias::writeFileAtomic(file, span, fsyncData, true);
        if (!write)
            return write;

        return Expected<void>{};
    }

    return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
}

auto UndoableSpace::importHistorySavefile(ConcretePathStringView root,
                                          std::filesystem::path const& file,
                                          bool applyOptions) -> Expected<void> {
    auto bytes = UndoUtilsAlias::readBinaryFile(file);
    if (!bytes)
        return std::unexpected(bytes.error());

    auto documentExpected =
        UndoSavefile::decode(std::span<const std::byte>(bytes->data(), bytes->size()));
    if (!documentExpected)
        return std::unexpected(documentExpected.error());
    auto document = std::move(documentExpected.value());

    if (auto state = findJournalRoot(root); state) {
        if (!document.rootPath.empty() && document.rootPath != state->rootPath) {
            return std::unexpected(Error{Error::Code::InvalidPath, "Savefile root mismatch"});
        }

        std::unique_lock lock(state->mutex);
        if (state->activeTransaction) {
            return std::unexpected(Error{Error::Code::InvalidPermissions,
                                         "Cannot import while transaction active"});
        }

        if (applyOptions) {
            state->options.maxEntries           = document.options.maxEntries;
            state->options.maxBytesRetained     = document.options.maxBytesRetained;
            state->options.maxDiskBytes         = document.options.maxDiskBytes;
            state->options.keepLatestFor        = std::chrono::milliseconds{document.options.keepLatestForMs};
            state->options.manualGarbageCollect = document.options.manualGarbageCollect;
            if (state->options.ramCacheEntries == 0) {
                state->options.ramCacheEntries = 8;
            }
        }

        state->journal.clear();
        state->telemetry.cachedUndo       = 0;
        state->telemetry.cachedRedo       = 0;
        state->telemetry.undoBytes        = 0;
        state->telemetry.redoBytes        = 0;
        state->telemetry.trimmedEntries   = 0;
        state->telemetry.trimmedBytes     = 0;
        state->telemetry.trimOperations   = 0;
        state->telemetry.persistenceDirty = false;
        state->persistenceDirty           = state->persistenceEnabled;
        state->stateDirty                 = state->persistenceEnabled;
        state->liveBytes                  = 0;

        auto retention = state->journal.policy();
        retention.maxEntries = state->options.maxEntries;
        retention.maxBytes   = state->options.maxBytesRetained;
        state->journal.setRetentionPolicy(retention);

        auto applyEntry = [&](UndoJournal::JournalEntry const& entry,
                              bool applyToLive) -> Expected<void> {
            std::optional<NodeData> payload;
            if (applyToLive && entry.value.present) {
                auto decoded = UndoJournal::decodeNodeDataPayload(entry.value);
                if (!decoded)
                    return std::unexpected(decoded.error());
                payload = std::move(decoded.value());
            }

            auto relativeExpected = parseJournalRelativeComponents(*state, entry.path);
            if (!relativeExpected)
                return std::unexpected(relativeExpected.error());
            auto relativeComponents = std::move(relativeExpected.value());

            if (applyToLive) {
                auto applyResult = applyJournalNodeData(*state, relativeComponents, payload);
                if (!applyResult)
                    return applyResult;
            }

            state->journal.append(entry, false);
            return Expected<void>{};
        };

        auto undoCount = document.undoCount;
        if (undoCount > document.entries.size()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                         "Savefile undo count exceeds entry count"});
        }

        std::uint64_t maxSequence  = 0;
        bool          sequenceSeen = false;

        for (std::size_t idx = 0; idx < document.entries.size(); ++idx) {
            bool applyToLive = idx < undoCount;
            auto const& entry = document.entries[idx];
            maxSequence       = std::max(maxSequence, entry.sequence);
            sequenceSeen      = sequenceSeen || (entry.sequence != 0);

            auto result = applyEntry(entry, applyToLive);
            if (!result)
                return result;
        }

        // Convert trailing entries into redo stack by rewinding cursor.
        for (std::size_t idx = undoCount; idx < document.entries.size(); ++idx) {
            auto redoEntry = state->journal.undo();
            if (!redoEntry.has_value()) {
                return std::unexpected(Error{Error::Code::UnknownError,
                                             "Failed to rebuild redo stack"});
            }
        }

        std::vector<UndoJournal::JournalEntry> persistedEntries;
        if (state->persistenceEnabled) {
            persistedEntries.reserve(state->journal.size());
            for (std::size_t idx = 0; idx < state->journal.size(); ++idx) {
                persistedEntries.push_back(state->journal.entryAt(idx));
            }
        }

        std::uint64_t fallbackNext = document.entries.empty()
                                          ? 0
                                          : static_cast<std::uint64_t>(document.entries.size());
        auto nextFromSequence = sequenceSeen ? maxSequence + 1 : fallbackNext;
        state->nextSequence   = std::max(document.nextSequence, nextFromSequence);

        auto stats = state->journal.stats();
        state->telemetry.cachedUndo = stats.undoCount;
        state->telemetry.cachedRedo = stats.redoCount;
        state->telemetry.undoBytes  = stats.undoBytes;
        state->telemetry.redoBytes  = stats.redoBytes;

        state->liveBytes = computeJournalLiveBytes(*state);

        lock.unlock();

        if (state->persistenceEnabled) {
            auto persist = UndoJournal::compactJournal(
                state->journalPath, std::span<const UndoJournal::JournalEntry>(persistedEntries), true);
            if (!persist)
                return persist;

            std::scoped_lock relock(state->mutex);
            state->persistenceDirty           = false;
            state->telemetry.persistenceDirty = false;
        }

        updateJournalDiskTelemetry(*state);

        return Expected<void>{};
    }

    return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
}

} // namespace SP::History
