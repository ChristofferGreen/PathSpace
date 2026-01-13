#include "history/UndoableSpace.hpp"

#include "history/UndoJournalPersistence.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "history/UndoableSpaceState.hpp"
#include "log/TaggedLogger.hpp"

#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using SP::Error;
using SP::Expected;
namespace UndoUtilsAlias = SP::History::UndoUtils;
namespace UndoJournal    = SP::History::UndoJournal;

constexpr bool isValidPersistenceTokenChar(char c) noexcept {
    return (c >= 'a' && c <= 'z')
           || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || c == '_' || c == '-';
}

constexpr bool isValidPersistenceToken(std::string_view token, bool allowEmpty) noexcept {
    if (token.empty()) {
        return allowEmpty;
    }
    if (token == "." || token == "..") {
        return false;
    }
    for (char c : token) {
        if (!isValidPersistenceTokenChar(c)) {
            return false;
        }
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
constexpr char PreferredSeparatorLiteral[] = {std::filesystem::path::preferred_separator, '\0'};
static_assert(!isValidPersistenceToken(std::string_view{PreferredSeparatorLiteral, 1}, false),
              "Directory separators must not be permitted in persistence namespaces");

} // namespace

namespace SP::History {

auto UndoableSpace::ensureJournalPersistenceSetup(UndoJournalRootState& state) -> Expected<void> {
    if (!state.persistenceEnabled) {
        return {};
    }

    if (state.encodedRoot.empty()) {
        state.encodedRoot = encodeRootForPersistence(state.rootPath);
    }
    if (auto checkEncoded = validatePersistenceToken(state.encodedRoot, "encoded_root", false);
        !checkEncoded) {
        return checkEncoded;
    }

    auto const& namespaceToken = state.options.persistenceNamespace.empty()
                                     ? spaceUuid
                                     : state.options.persistenceNamespace;
    if (auto checkNamespace = validatePersistenceToken(namespaceToken, "namespace", false);
        !checkNamespace) {
        return checkNamespace;
    }

    auto baseRoot = persistenceRootPath(state.options);
    auto nsDir    = std::filesystem::path(namespaceToken);

    state.persistencePath = baseRoot / nsDir / state.encodedRoot;
    state.journalPath     = state.persistencePath / "journal.log";

    std::error_code ec;
    std::filesystem::create_directories(state.persistencePath, ec);
    if (ec) {
        return std::unexpected(Error{Error::Code::UnknownError,
                                     "Failed to create journal persistence directories"});
    }

    state.persistenceDirty             = false;
    state.telemetry.persistenceDirty   = false;
    return {};
}

auto UndoableSpace::loadJournalPersistence(UndoJournalRootState& state) -> Expected<void> {
    if (!state.persistenceEnabled) {
        return {};
    }

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
            state.nextSequence             = 0;
            state.telemetry.trimmedEntries = 0;
            state.telemetry.trimmedBytes   = 0;
            state.telemetry.trimOperations = 0;
            state.liveBytes                = 0;
            state.persistenceDirty         = false;
            state.telemetry.persistenceDirty = false;
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
        maxSequence  = std::max(maxSequence, entry.sequence);
        sequenceSeen = sequenceSeen || (entry.sequence != 0);

        std::optional<NodeData> payload;
        if (entry.value.present) {
            auto decoded = UndoJournal::decodeNodeDataPayload(entry.value);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            payload = std::move(decoded.value());
        }

        auto relativeExpected = parseJournalRelativeComponents(state, entry.path);
        if (!relativeExpected) {
            return std::unexpected(relativeExpected.error());
        }
        auto relativeComponents = std::move(relativeExpected.value());

        auto applyResult = applyJournalNodeData(state, relativeComponents, payload);
        if (!applyResult) {
            return applyResult;
        }

        state.journal.append(entry, false);
    }

    state.journal.setRetentionPolicy(state.journal.policy());

    auto stats = state.journal.stats();
    state.telemetry.cachedUndo = stats.undoCount;
    state.telemetry.cachedRedo = stats.redoCount;
    state.telemetry.undoBytes  = stats.undoBytes;
    state.telemetry.redoBytes  = stats.redoBytes;
    state.telemetry.trimmedEntries = stats.trimmedEntries;
    state.telemetry.trimmedBytes   = stats.trimmedBytes;
    if (stats.trimmedEntries == 0) {
        state.telemetry.trimOperations = 0;
    }

    std::uint64_t fallbackNext = entries.empty() ? 0 : static_cast<std::uint64_t>(entries.size());
    auto           nextFromSequence = sequenceSeen ? maxSequence + 1 : fallbackNext;
    state.nextSequence = std::max(state.nextSequence, nextFromSequence);

    lock.unlock();

    updateJournalDiskTelemetry(state);
    if (state.persistenceEnabled) {
        auto compact = compactJournalPersistence(state, false);
        if (!compact) {
            return compact;
        }
    }

    return {};
}

auto UndoableSpace::compactJournalPersistence(UndoJournalRootState& state, bool fsync)
    -> Expected<void> {
    if (!state.persistenceEnabled || !state.persistenceWriter) {
        return {};
    }
    auto flush = state.persistenceWriter->flush();
    if (!flush) {
        return flush;
    }
    (void)fsync;
    return Expected<void>{};
}

void UndoableSpace::updateJournalDiskTelemetry(UndoJournalRootState& state) {
    if (!state.persistenceEnabled) {
        state.telemetry.diskBytes = 0;
        state.telemetry.diskEntries = 0;
        return;
    }

    auto metadataBytes = UndoUtilsAlias::fileSizeOrZero(state.journalPath);
    state.telemetry.diskBytes   = metadataBytes;
    state.telemetry.diskEntries = state.journal.stats().totalEntries;
}

auto UndoableSpace::encodeRootForPersistence(std::string const& rootPath) const -> std::string {
    if (rootPath.empty() || rootPath == "/") {
        return "__root";
    }

    std::string encoded;
    encoded.reserve(rootPath.size());
    for (char c : rootPath) {
        if (c == '/') {
            encoded.push_back('_');
        } else {
            encoded.push_back(c);
        }
    }
    return encoded;
}

auto UndoableSpace::persistenceRootPath(HistoryOptions const& opts) const -> std::filesystem::path {
    if (!opts.persistenceRoot.empty()) {
        return std::filesystem::path(opts.persistenceRoot);
    }
    return defaultPersistenceRoot();
}

auto UndoableSpace::defaultPersistenceRoot() const -> std::filesystem::path {
    auto base = std::filesystem::path(".");
    if (auto env = std::getenv("PATHSPACE_HISTORY_ROOT")) {
        base = std::filesystem::path(env);
    }
    return base / "history";
}

} // namespace SP::History
