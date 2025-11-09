#include "history/UndoableSpace.hpp"

#include "history/UndoSnapshotCodec.hpp"
#include "history/UndoJournalPersistence.hpp"
#include "history/UndoHistoryMetadata.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "history/UndoableSpaceState.hpp"
#include "log/TaggedLogger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using SP::Error;
using SP::Expected;
using SP::History::Detail::forEachHistoryStack;
namespace UndoUtilsAlias    = SP::History::UndoUtils;
namespace UndoMetadata      = SP::History::UndoMetadata;
namespace UndoSnapshotCodec = SP::History::UndoSnapshotCodec;
namespace UndoJournal       = SP::History::UndoJournal;

constexpr bool isValidPersistenceTokenChar(char c) noexcept {
    return (c >= 'a' && c <= 'z')
           || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || c == '_' || c == '-';
}

constexpr bool isValidPersistenceToken(std::string_view token, bool allowEmpty) noexcept {
    if (token.empty())
        return allowEmpty;
    if (token == "." || token == "..")
        return false;
    for (char c : token) {
        if (!isValidPersistenceTokenChar(c))
            return false;
    }
    return true;
}

inline auto validatePersistenceToken(std::string_view token,
                                     std::string_view label,
                                     bool allowEmpty) -> Expected<void> {
    if (!isValidPersistenceToken(token, allowEmpty)) {
        std::string message = "Invalid history persistence ";
        message.append(label);
        message.append(" '");
        message.append(token);
        message.append("'; allowed characters are [A-Za-z0-9_-] and tokens may not be '.' or '..'");
        return std::unexpected(Error{Error::Code::InvalidPermissions, std::move(message)});
    }
    return {};
}

static_assert(isValidPersistenceToken("namespace", false));
static_assert(isValidPersistenceToken("valid_namespace-1", false));
static_assert(isValidPersistenceToken("", true));
static_assert(!isValidPersistenceToken(".", false));
static_assert(!isValidPersistenceToken("..", false));
static_assert(!isValidPersistenceToken("invalid/namespace", false));
static_assert(!isValidPersistenceToken("invalid namespace", false));
static_assert(!isValidPersistenceToken("\\", false));
constexpr char kPreferredSeparatorLiteral[] = {std::filesystem::path::preferred_separator, '\0'};
static_assert(!isValidPersistenceToken(std::string_view{kPreferredSeparatorLiteral, 1}, false),
              "Directory separators must not be permitted in persistence namespaces");

} // namespace

namespace SP::History {

auto UndoableSpace::ensureEntriesDirectory(RootState& state) -> Expected<void> {
    std::error_code ec;
    std::filesystem::create_directories(state.entriesPath, ec);
    if (ec) {
        return std::unexpected(
            Error{Error::Code::UnknownError, "Failed to create persistence directories"});
    }
    return {};
}

auto UndoableSpace::ensureJournalPersistenceSetup(UndoJournalRootState& state) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    if (state.encodedRoot.empty()) {
        state.encodedRoot = encodeRootForPersistence(state.rootPath);
    }
    if (auto checkEncoded = validatePersistenceToken(state.encodedRoot, "encoded_root", false); !checkEncoded)
        return checkEncoded;

    auto const& namespaceToken =
        state.options.persistenceNamespace.empty() ? spaceUuid : state.options.persistenceNamespace;
    if (auto checkNamespace = validatePersistenceToken(namespaceToken, "namespace", false); !checkNamespace)
        return checkNamespace;

    auto baseRoot = persistenceRootPath(state.options);
    auto nsDir    = std::filesystem::path(namespaceToken);

    state.persistencePath = baseRoot / nsDir / state.encodedRoot;
    state.journalPath     = state.persistencePath / "journal.log";

    std::error_code ec;
    std::filesystem::create_directories(state.persistencePath, ec);
    if (ec) {
        return std::unexpected(
            Error{Error::Code::UnknownError,
                  "Failed to create journal persistence directories"});
    }

    state.persistenceDirty             = false;
    state.telemetry.persistenceDirty   = false;

    return {};
}

auto UndoableSpace::loadJournalPersistence(UndoJournalRootState& state) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    std::vector<UndoJournal::JournalEntry> entries;
    auto replay = UndoJournal::replayJournal(
        state.journalPath,
        [&](UndoJournal::JournalEntry&& entry) -> Expected<void> {
            entries.push_back(std::move(entry));
            return Expected<void>{};
        });

    if (!replay) {
        if (replay.error().code == Error::Code::NotFound) {
            std::scoped_lock lock(state.mutex);
            state.journal.clear();
            state.nextSequence                 = 0;
            state.telemetry.cachedUndo         = 0;
            state.telemetry.cachedRedo         = 0;
            state.telemetry.undoBytes          = 0;
            state.telemetry.redoBytes          = 0;
            state.telemetry.trimmedEntries     = 0;
            state.telemetry.trimmedBytes       = 0;
            state.telemetry.trimOperations     = 0;
            state.persistenceDirty             = false;
            state.telemetry.persistenceDirty   = false;
            updateJournalDiskTelemetry(state);
            return {};
        }
        return std::unexpected(replay.error());
    }

    std::unique_lock lock(state.mutex);
    state.journal.clear();
    state.liveBytes = 0;

    std::uint64_t maxSequence  = 0;
    bool          sequenceSeen = false;

    for (auto& entry : entries) {
        maxSequence = std::max(maxSequence, entry.sequence);
        sequenceSeen = sequenceSeen || (entry.sequence != 0);

        std::optional<NodeData> payload;
        if (entry.value.present) {
            auto decoded = UndoJournal::decodeNodeDataPayload(entry.value);
            if (!decoded)
                return std::unexpected(decoded.error());
            payload = std::move(decoded.value());
        }

        auto relativeExpected = parseJournalRelativeComponents(state, entry.path);
        if (!relativeExpected)
            return std::unexpected(relativeExpected.error());
        auto relativeComponents = std::move(relativeExpected.value());

        auto applyResult = applyJournalNodeData(state, relativeComponents, payload);
        if (!applyResult)
            return applyResult;

        state.journal.append(entry, false);
    }

    state.journal.setRetentionPolicy(state.journal.policy());

    auto stats = state.journal.stats();
    state.telemetry.cachedUndo     = stats.undoCount;
    state.telemetry.cachedRedo     = stats.redoCount;
    state.telemetry.undoBytes      = stats.undoBytes;
    state.telemetry.redoBytes      = stats.redoBytes;
    state.telemetry.trimmedEntries = stats.trimmedEntries;
    state.telemetry.trimmedBytes   = stats.trimmedBytes;
    if (stats.trimmedEntries == 0) {
        state.telemetry.trimOperations = 0;
    }

    state.nextSequence = sequenceSeen ? maxSequence + 1
                                      : static_cast<std::uint64_t>(entries.size());

    state.persistenceDirty             = false;
    state.telemetry.persistenceDirty   = false;
    lock.unlock();

    updateJournalDiskTelemetry(state);
    return {};
}

auto UndoableSpace::compactJournalPersistence(UndoJournalRootState& state, bool fsync) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    if (state.persistenceWriter) {
        if (auto flush = state.persistenceWriter->flush(); !flush)
            return flush;
        state.persistenceWriter.reset();
    }

    std::vector<UndoJournal::JournalEntry> entries;
    entries.reserve(state.journal.size());
    for (std::size_t i = 0; i < state.journal.size(); ++i) {
        entries.push_back(state.journal.entryAt(i));
    }

    std::span<UndoJournal::JournalEntry const> span(entries.data(), entries.size());
    auto compact = UndoJournal::compactJournal(state.journalPath, span, fsync);
    if (!compact)
        return compact;

    state.persistenceDirty             = false;
    state.telemetry.persistenceDirty   = false;
    updateJournalDiskTelemetry(state);
    return {};
}

void UndoableSpace::updateJournalDiskTelemetry(UndoJournalRootState& state) {
    if (!state.persistenceEnabled) {
        state.telemetry.diskBytes   = 0;
        state.telemetry.diskEntries = state.journal.size();
        return;
    }
    state.telemetry.diskBytes   =
        static_cast<std::size_t>(UndoUtilsAlias::fileSizeOrZero(state.journalPath));
    state.telemetry.diskEntries = state.journal.size();
}

auto UndoableSpace::ensurePersistenceSetup(RootState& state) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    if (state.encodedRoot.empty()) {
        state.encodedRoot = encodeRootForPersistence(state.rootPath);
    }
    if (auto checkEncoded = validatePersistenceToken(state.encodedRoot, "encoded_root", false); !checkEncoded)
        return checkEncoded;

    auto const& namespaceToken =
        state.options.persistenceNamespace.empty() ? spaceUuid : state.options.persistenceNamespace;
    if (auto checkNamespace = validatePersistenceToken(namespaceToken, "namespace", false); !checkNamespace)
        return checkNamespace;

    auto baseRoot = persistenceRootPath(state.options);
    auto nsDir    = std::filesystem::path(namespaceToken);

    state.persistencePath = baseRoot / nsDir / state.encodedRoot;
    state.entriesPath     = state.persistencePath / "entries";

    if (auto ensureDir = ensureEntriesDirectory(state); !ensureDir)
        return ensureDir;

    state.stateDirty         = false;
    state.hasPersistentState = std::filesystem::exists(stateMetaPath(state));
    return {};
}

auto UndoableSpace::loadPersistentState(RootState& state) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    auto statePath = stateMetaPath(state);
    auto metaData  = UndoUtilsAlias::readBinaryFile(statePath);
    if (!metaData) {
        if (metaData.error().code == Error::Code::NotFound) {
            state.hasPersistentState = false;
            return {};
        }
        return std::unexpected(metaData.error());
    }

    auto stateMetaExpected = UndoMetadata::parseStateMeta(
        std::span<const std::byte>(metaData->data(), metaData->size()));
    if (!stateMetaExpected)
        return std::unexpected(stateMetaExpected.error());
    auto stateMeta = std::move(stateMetaExpected.value());

    state.options.manualGarbageCollect = stateMeta.manualGc;
    if (stateMeta.ramCacheEntries > 0)
        state.options.ramCacheEntries = stateMeta.ramCacheEntries;
    if (state.options.ramCacheEntries == 0)
        state.options.ramCacheEntries = 8;

    state.prototype = CowSubtreePrototype{};
    state.undoStack.clear();
    state.redoStack.clear();
    state.telemetry                 = {};
    state.telemetry.persistenceDirty = false;

    std::uintmax_t diskBytes = 0;
    std::size_t    diskEntries = 0;

    auto liveSnapshotPath     = entrySnapshotPath(state, stateMeta.liveGeneration);
    auto liveSnapshotExpected = UndoSnapshotCodec::loadSnapshotFromFile(state.prototype,
                                                                        liveSnapshotPath);
    if (!liveSnapshotExpected)
        return std::unexpected(liveSnapshotExpected.error());

    state.liveSnapshot = std::move(liveSnapshotExpected.value());
    state.liveBytes    = state.prototype.analyze(state.liveSnapshot).payloadBytes;

    auto liveMeta = UndoUtilsAlias::readBinaryFile(entryMetaPath(state, stateMeta.liveGeneration));
    if (liveMeta) {
        auto entryMetaParsed = UndoMetadata::parseEntryMeta(
            std::span<const std::byte>(liveMeta->data(), liveMeta->size()));
        if (entryMetaParsed) {
            RootState::OperationRecord record;
            record.type            = "restore";
            record.timestamp       = std::chrono::system_clock::time_point{
                std::chrono::milliseconds(entryMetaParsed->timestampMs)};
            record.duration        = std::chrono::milliseconds{0};
            record.success         = true;
            record.undoCountBefore = 0;
            record.undoCountAfter  = 0;
            record.redoCountBefore = 0;
            record.redoCountAfter  = 0;
            record.bytesBefore     = 0;
            record.bytesAfter      = state.liveBytes;
            record.message         = "persistence_restore";
            state.telemetry.lastOperation = std::move(record);
        }
    }

    diskBytes += UndoUtilsAlias::fileSizeOrZero(liveSnapshotPath);
    diskBytes += UndoUtilsAlias::fileSizeOrZero(entryMetaPath(state, stateMeta.liveGeneration));
    diskEntries += 1;

    auto loadEntryList = [&](std::vector<std::size_t> const& generations,
                             std::vector<RootState::Entry>& stack,
                             std::size_t&                   byteCounter) -> Expected<void> {
        for (auto generation : generations) {
            auto metaPath = entryMetaPath(state, generation);
            auto metaBytes = UndoUtilsAlias::readBinaryFile(metaPath);
            if (!metaBytes)
                return std::unexpected(metaBytes.error());
            auto metaParsed = UndoMetadata::parseEntryMeta(
                std::span<const std::byte>(metaBytes->data(), metaBytes->size()));
            if (!metaParsed)
                return std::unexpected(metaParsed.error());

            RootState::Entry entry;
            entry.snapshot.generation = generation;
            entry.bytes               = metaParsed->bytes;
            entry.timestamp           = std::chrono::system_clock::time_point{
                std::chrono::milliseconds(metaParsed->timestampMs)};
            entry.persisted = true;
            entry.cached    = false;

            byteCounter += entry.bytes;
            stack.push_back(std::move(entry));

            diskBytes += UndoUtilsAlias::fileSizeOrZero(entrySnapshotPath(state, generation));
            diskBytes += UndoUtilsAlias::fileSizeOrZero(metaPath);
            diskEntries += 1;
        }
        return Expected<void>{};
    };

    std::size_t undoBytes = 0;
    std::size_t redoBytes = 0;

    if (auto loadUndo = loadEntryList(stateMeta.undoGenerations, state.undoStack, undoBytes); !loadUndo)
        return loadUndo;
    if (auto loadRedo = loadEntryList(stateMeta.redoGenerations, state.redoStack, redoBytes); !loadRedo)
        return loadRedo;

    state.telemetry.undoBytes = undoBytes;
    state.telemetry.redoBytes = redoBytes;

    std::size_t maxGeneration = stateMeta.liveGeneration;
    for (auto g : stateMeta.undoGenerations)
        maxGeneration = std::max(maxGeneration, g);
    for (auto g : stateMeta.redoGenerations)
        maxGeneration = std::max(maxGeneration, g);

    state.prototype.setNextGeneration(maxGeneration + 1);

    state.telemetry.diskBytes   = static_cast<std::size_t>(diskBytes);
    state.telemetry.diskEntries = diskEntries;
    state.hasPersistentState    = true;
    state.stateDirty            = false;

    return {};
}

auto UndoableSpace::restoreRootFromPersistence(RootState& state) -> Expected<void> {
    if (!state.persistenceEnabled || !state.hasPersistentState || !state.options.restoreFromPersistence)
        return {};
    return applySnapshotLocked(state, state.liveSnapshot);
}

auto UndoableSpace::persistStacksLocked(RootState& state, bool forceFsync) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    auto flushNow = forceFsync || !state.options.manualGarbageCollect;
    if (auto ensureDir = ensureEntriesDirectory(state); !ensureDir)
        return ensureDir;

    auto persistSnapshot = [&](CowSubtreePrototype::Snapshot const& snapshot,
                               std::chrono::system_clock::time_point timestamp,
                               std::size_t                            bytesEstimate) -> Expected<void> {
        auto encoded = UndoSnapshotCodec::encodeSnapshot(snapshot);
        if (!encoded)
            return std::unexpected(encoded.error());
        auto snapshotPath = entrySnapshotPath(state, snapshot.generation);
        auto metaPath     = entryMetaPath(state, snapshot.generation);
        auto span         = std::span<const std::byte>(encoded->data(), encoded->size());
        if (auto write = UndoUtilsAlias::writeFileAtomic(snapshotPath, span, flushNow, true); !write)
            return write;

        UndoMetadata::EntryMetadata meta;
        meta.generation  = snapshot.generation;
        meta.bytes       = bytesEstimate;
        meta.timestampMs = UndoUtilsAlias::toMillis(timestamp);

        auto metaBytes = UndoMetadata::encodeEntryMeta(meta);
        auto metaSpan  = std::span<const std::byte>(metaBytes.data(), metaBytes.size());
        if (auto writeMeta = UndoUtilsAlias::writeFileAtomic(metaPath, metaSpan, flushNow, true);
            !writeMeta)
            return writeMeta;
        return Expected<void>{};
    };

    auto persistEntry = [&](RootState::Entry& entry) -> Expected<void> {
        if (entry.persisted)
            return Expected<void>{};
        if (!entry.cached) {
            return std::unexpected(Error{
                Error::Code::UnknownError, "Attempted to persist history entry without cache"});
        }
        auto result = persistSnapshot(entry.snapshot, entry.timestamp, entry.bytes);
        if (!result)
            return result;
        entry.persisted = true;
        return result;
    };

    std::optional<Error> persistError;
    forEachHistoryStack(state, [&](auto& stack, bool) {
        if (persistError)
            return;
        for (auto& entry : stack) {
            auto result = persistEntry(entry);
            if (!result) {
                persistError = result.error();
                break;
            }
        }
    });
    if (persistError)
        return std::unexpected(*persistError);

    if (state.stateDirty || forceFsync) {
        auto livePersist = persistSnapshot(state.liveSnapshot,
                                           std::chrono::system_clock::now(),
                                           state.liveBytes);
        if (!livePersist)
            return livePersist;

        UndoMetadata::StateMetadata stateMeta;
        stateMeta.liveGeneration  = state.liveSnapshot.generation;
        stateMeta.manualGc        = state.options.manualGarbageCollect;
        stateMeta.ramCacheEntries = state.options.ramCacheEntries;
        forEachHistoryStack(state, [&](auto const& stack, bool isUndo) {
            auto& target = isUndo ? stateMeta.undoGenerations : stateMeta.redoGenerations;
            target.reserve(stack.size());
            for (auto const& entry : stack) {
                target.push_back(entry.snapshot.generation);
            }
        });

        auto stateBytes = UndoMetadata::encodeStateMeta(stateMeta);
        auto stateSpan  = std::span<const std::byte>(stateBytes.data(), stateBytes.size());
        if (auto writeState = UndoUtilsAlias::writeFileAtomic(stateMetaPath(state), stateSpan, flushNow, true);
            !writeState)
            return writeState;

        state.stateDirty = false;
    }

    updateDiskTelemetryLocked(state);

    if (flushNow) {
        state.telemetry.persistenceDirty = false;
    } else {
        state.telemetry.persistenceDirty = true;
    }

    return {};
}

auto UndoableSpace::loadEntrySnapshotLocked(RootState& state, std::size_t stackIndex, bool undoStack)
    -> Expected<void> {
    auto& stack = undoStack ? state.undoStack : state.redoStack;
    if (stackIndex >= stack.size()) {
        return std::unexpected(
            Error{Error::Code::UnknownError, "History entry index out of range"});
    }
    auto& entry = stack[stackIndex];
    if (entry.cached)
        return {};
    CowSubtreePrototype loaderPrototype;
    auto path = entrySnapshotPath(state, entry.snapshot.generation);
    auto snapshotExpected = UndoSnapshotCodec::loadSnapshotFromFile(loaderPrototype, path);
    if (!snapshotExpected)
        return std::unexpected(snapshotExpected.error());
    entry.snapshot = std::move(snapshotExpected.value());
    entry.cached   = true;
    return {};
}

auto UndoableSpace::applyRamCachePolicyLocked(RootState& state) -> void {
    auto enforceStack = [&](std::vector<RootState::Entry>& stack, bool undoStack) {
        std::size_t limit = state.options.ramCacheEntries;
        if (limit == 0) {
            for (auto& entry : stack) {
                if (entry.cached) {
                    entry.snapshot.root.reset();
                    entry.cached = false;
                }
            }
            return;
        }

        std::size_t cached = 0;
        for (std::size_t idx = stack.size(); idx-- > 0;) {
            auto& entry = stack[idx];
            if (cached < limit) {
                if (!entry.cached && entry.persisted) {
                    auto load = loadEntrySnapshotLocked(state, idx, undoStack);
                    if (!load) {
                        sp_log("Failed to load history snapshot for caching: "
                                   + load.error().message.value_or("unknown"),
                               "UndoableSpace");
                    }
                }
                cached += 1;
            } else if (entry.cached) {
                entry.snapshot.root.reset();
                entry.cached = false;
            }
        }
    };

    forEachHistoryStack(state, enforceStack);
    updateCacheTelemetryLocked(state);
}

auto UndoableSpace::updateCacheTelemetryLocked(RootState& state) -> void {
    state.telemetry.cachedUndo = 0;
    state.telemetry.cachedRedo = 0;
    forEachHistoryStack(state, [&](auto const& stack, bool isUndo) {
        std::size_t count = 0;
        for (auto const& entry : stack) {
            if (entry.cached)
                count += 1;
        }
        if (isUndo) {
            state.telemetry.cachedUndo = count;
        } else {
            state.telemetry.cachedRedo = count;
        }
    });
}

auto UndoableSpace::updateDiskTelemetryLocked(RootState& state) -> void {
    if (!state.persistenceEnabled) {
        state.telemetry.diskBytes   = 0;
        state.telemetry.diskEntries = 0;
        return;
    }

    std::uintmax_t totalBytes = 0;
    std::size_t    count      = 0;

    auto addEntryFiles = [&](std::size_t generation, bool persisted) {
        if (!persisted)
            return;
        totalBytes += UndoUtilsAlias::fileSizeOrZero(entrySnapshotPath(state, generation));
        totalBytes += UndoUtilsAlias::fileSizeOrZero(entryMetaPath(state, generation));
        count += 1;
    };

    addEntryFiles(state.liveSnapshot.generation, true);
    forEachHistoryStack(state, [&](auto const& stack, bool) {
        for (auto const& entry : stack) {
            addEntryFiles(entry.snapshot.generation, entry.persisted);
        }
    });

    totalBytes += UndoUtilsAlias::fileSizeOrZero(stateMetaPath(state));

    state.telemetry.diskBytes   = static_cast<std::size_t>(totalBytes);
    state.telemetry.diskEntries = count;
}

auto UndoableSpace::encodeRootForPersistence(std::string const& rootPath) const -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0');
    for (unsigned char c : rootPath) {
        oss << std::setw(2) << static_cast<int>(c);
    }
    return oss.str();
}

auto UndoableSpace::persistenceRootPath(HistoryOptions const& opts) const -> std::filesystem::path {
    if (!opts.persistenceRoot.empty()) {
        return std::filesystem::path(opts.persistenceRoot);
    }
    if (!defaultOptions.persistenceRoot.empty()) {
        return std::filesystem::path(defaultOptions.persistenceRoot);
    }
    return defaultPersistenceRoot();
}

auto UndoableSpace::defaultPersistenceRoot() const -> std::filesystem::path {
    if (auto* env = std::getenv("PATHSPACE_HISTORY_ROOT"); env && *env) {
        return std::filesystem::path(env);
    }
    if (auto* tmp = std::getenv("TMPDIR"); tmp && *tmp) {
        return std::filesystem::path(tmp) / "pathspace_history";
    }
    return std::filesystem::temp_directory_path() / "pathspace_history";
}

auto UndoableSpace::entrySnapshotPath(RootState const& state, std::size_t generation) const
    -> std::filesystem::path {
    return state.entriesPath / (UndoSnapshotCodec::snapshotFileStem(generation) + ".snapshot");
}

auto UndoableSpace::entryMetaPath(RootState const& state, std::size_t generation) const
    -> std::filesystem::path {
    return state.entriesPath / (UndoSnapshotCodec::snapshotFileStem(generation) + ".meta");
}

auto UndoableSpace::stateMetaPath(RootState const& state) const -> std::filesystem::path {
    return state.persistencePath / "state.meta";
}

auto UndoableSpace::removeEntryFiles(RootState& state, std::size_t generation) -> void {
    if (!state.persistenceEnabled)
        return;
    UndoUtilsAlias::removePathIfExists(entrySnapshotPath(state, generation));
    UndoUtilsAlias::removePathIfExists(entryMetaPath(state, generation));
}

} // namespace SP::History
