#include "history/UndoableSpace.hpp"
#include "history/UndoSnapshotCodec.hpp"
#include "history/UndoHistoryMetadata.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "history/UndoableSpaceState.hpp"

#include "PathSpace.hpp"
#include "core/InsertReturn.hpp"
#include "core/Node.hpp"
#include "core/NodeData.hpp"
#include "log/TaggedLogger.hpp"
#include <cstring>
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <charconv>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace {

using SP::History::CowSubtreePrototype;
using SP::Error;
using SP::Expected;
using SP::ConcretePathStringView;
namespace UndoUtilsAlias = SP::History::UndoUtils;
namespace UndoPaths = SP::History::UndoUtils::Paths;
namespace UndoMetadata = SP::History::UndoMetadata;

auto canonicalizeOptOutPrefix(std::string const& rootPath, std::string const& prefix)
    -> Expected<std::string> {
    if (prefix.empty()) {
        return std::unexpected(
            Error{Error::Code::InvalidPath, "Execution opt-out prefix may not be empty"});
    }

    std::string candidate;
    if (!prefix.empty() && prefix.front() == '/') {
        candidate = prefix;
    } else if (rootPath == "/") {
        candidate.reserve(prefix.size() + 1);
        candidate.push_back('/');
        candidate.append(prefix);
    } else {
        candidate.reserve(rootPath.size() + 1 + prefix.size());
        candidate.append(rootPath);
        candidate.push_back('/');
        candidate.append(prefix);
    }

    ConcretePathStringView candidateView{std::string_view{candidate}};
    auto canonical = candidateView.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    auto canonicalStr = std::string{canonical->getPath()};

    ConcretePathStringView rootView{std::string_view{rootPath}};
    ConcretePathStringView canonicalView{std::string_view{canonicalStr}};
    auto isPrefix = rootView.isPrefixOf(canonicalView);
    if (!isPrefix) {
        return std::unexpected(isPrefix.error());
    }
    if (!isPrefix.value()) {
        return std::unexpected(Error{
            Error::Code::InvalidPermissions,
            "Execution opt-out prefix must be within the history root"});
    }
    return canonicalStr;
}

} // namespace

namespace SP::History {

using SP::ConcretePathStringView;

UndoableSpace::UndoableSpace(std::unique_ptr<PathSpaceBase> inner, HistoryOptions defaults)
    : inner(std::move(inner))
    , defaultOptions(defaults)
    , spaceUuid(UndoUtilsAlias::generateSpaceUuid()) {}

auto UndoableSpace::resolveRootNode() -> Node* {
    if (!inner)
        return nullptr;
    return inner->getRootNode();
}

auto UndoableSpace::enableHistory(ConcretePathStringView root, HistoryOptions opts) -> Expected<void> {
    auto canonical = root.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    auto componentsExpected = canonical->components();
    if (!componentsExpected) {
        return std::unexpected(componentsExpected.error());
    }

    auto normalized = std::string{canonical->getPath()};
    auto components = std::move(componentsExpected.value());

    {
        std::scoped_lock lock(rootsMutex);
        if (roots.find(normalized) != roots.end()) {
            return std::unexpected(Error{Error::Code::UnknownError, "History already enabled for path"});
        }
        if (!defaultOptions.allowNestedUndo || !opts.allowNestedUndo) {
            ConcretePathStringView normalizedView{canonical->getPath()};
            for (auto const& [existing, _] : roots) {
                ConcretePathStringView existingView{existing};
                auto existingIsPrefix = existingView.isPrefixOf(normalizedView);
                if (!existingIsPrefix) {
                    return std::unexpected(existingIsPrefix.error());
                }
                auto normalizedIsPrefix = normalizedView.isPrefixOf(existingView);
                if (!normalizedIsPrefix) {
                    return std::unexpected(normalizedIsPrefix.error());
                }
                if (existingIsPrefix.value() || normalizedIsPrefix.value()) {
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
    state->options            = defaultOptions;
    state->options.executionOptOutPrefixes = defaultOptions.executionOptOutPrefixes;
    state->options.maxEntries = opts.maxEntries ? opts.maxEntries : state->options.maxEntries;
    state->options.maxBytesRetained =
        opts.maxBytesRetained ? opts.maxBytesRetained : state->options.maxBytesRetained;
    state->options.manualGarbageCollect = opts.manualGarbageCollect;
    state->options.allowNestedUndo      = opts.allowNestedUndo;
    state->options.persistHistory       = state->options.persistHistory || opts.persistHistory;
    if (!opts.persistenceRoot.empty()) {
        state->options.persistenceRoot = opts.persistenceRoot;
    }
    if (!opts.persistenceNamespace.empty()) {
        state->options.persistenceNamespace = opts.persistenceNamespace;
    }
    if (opts.ramCacheEntries > 0) {
        state->options.ramCacheEntries = opts.ramCacheEntries;
    }
    if (state->options.ramCacheEntries == 0) {
        state->options.ramCacheEntries = 8;
    }
    state->options.maxDiskBytes =
        opts.maxDiskBytes ? opts.maxDiskBytes : state->options.maxDiskBytes;
    if (opts.keepLatestFor.count() > 0) {
        state->options.keepLatestFor = opts.keepLatestFor;
    }
    state->options.restoreFromPersistence =
        state->options.restoreFromPersistence && opts.restoreFromPersistence;
    if (!opts.executionOptOutPrefixes.empty()) {
        state->options.executionOptOutPrefixes.insert(state->options.executionOptOutPrefixes.end(),
                                                      opts.executionOptOutPrefixes.begin(),
                                                      opts.executionOptOutPrefixes.end());
    }
    {
        std::vector<std::string> canonicalOptOut;
        canonicalOptOut.reserve(state->options.executionOptOutPrefixes.size());
        for (auto const& prefix : state->options.executionOptOutPrefixes) {
            auto canonical = canonicalizeOptOutPrefix(state->rootPath, prefix);
            if (!canonical) {
                return std::unexpected(canonical.error());
            }
            canonicalOptOut.push_back(std::move(canonical.value()));
        }
        std::sort(canonicalOptOut.begin(), canonicalOptOut.end());
        canonicalOptOut.erase(std::unique(canonicalOptOut.begin(), canonicalOptOut.end()),
                              canonicalOptOut.end());
        state->options.executionOptOutPrefixes = canonicalOptOut;
        state->executionOptOutPrefixes         = std::move(canonicalOptOut);
    }
    state->encodedRoot       = encodeRootForPersistence(state->rootPath);
    state->persistenceEnabled = state->options.persistHistory;
    state->undoStack.clear();
    state->redoStack.clear();
    state->telemetry = {};

    if (state->persistenceEnabled) {
        auto setup = ensurePersistenceSetup(*state);
        if (!setup)
            return std::unexpected(setup.error());
        auto load = loadPersistentState(*state);
        if (!load)
            return std::unexpected(load.error());
        if (state->hasPersistentState) {
            std::scoped_lock rootLock(state->mutex);
            auto restore = restoreRootFromPersistence(*state);
            if (!restore)
                return std::unexpected(restore.error());
            applyRamCachePolicyLocked(*state);
            updateCacheTelemetryLocked(*state);
            std::scoped_lock lock(rootsMutex);
            roots.emplace(state->rootPath, std::move(state));
            return {};
        }
    }

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

    state->stateDirty = state->persistenceEnabled;
    updateCacheTelemetryLocked(*state);
    if (state->persistenceEnabled) {
        auto persist = persistStacksLocked(*state, true);
        if (!persist)
            return std::unexpected(persist.error());
    } else {
        updateDiskTelemetryLocked(*state);
    }

    {
        std::scoped_lock lock(rootsMutex);
        roots.emplace(state->rootPath, std::move(state));
    }

    return {};
}

auto UndoableSpace::disableHistory(ConcretePathStringView root) -> Expected<void> {
    auto canonical = root.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    auto normalized = std::string{canonical->getPath()};
    std::unique_lock lock(rootsMutex);
    auto it = roots.find(normalized);
    if (it == roots.end()) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }
    auto state = it->second;
    roots.erase(it);
    lock.unlock();
    if (state && state->persistenceEnabled) {
        std::error_code ec;
        std::filesystem::remove_all(state->persistencePath, ec);
    }
    return {};
}

auto UndoableSpace::findRoot(ConcretePathStringView root) const -> std::shared_ptr<RootState> {
    auto canonical = root.canonicalized();
    if (!canonical) {
        return {};
    }
    auto normalized = std::string{canonical->getPath()};
    std::scoped_lock lock(rootsMutex);
    auto it = roots.find(normalized);
    if (it == roots.end())
        return {};
    return it->second;
}

auto UndoableSpace::findRootByPath(std::string const& path) const -> std::optional<MatchedRoot> {
    ConcretePathStringView pathView{std::string_view{path}};
    auto canonical = pathView.canonicalized();
    if (!canonical) {
        return std::nullopt;
    }

    auto canonicalStr = std::string{canonical->getPath()};
    ConcretePathStringView canonicalView{canonical->getPath()};

    std::string                bestKey;
    std::shared_ptr<RootState> bestState;

    {
        std::scoped_lock lock(rootsMutex);
        for (auto const& [rootPath, state] : roots) {
            ConcretePathStringView rootView{rootPath};
            auto                  isPrefix = rootView.isPrefixOf(canonicalView);
            if (!isPrefix || !isPrefix.value()) {
                continue;
            }
            if (rootPath.size() > bestKey.size()) {
                bestKey   = rootPath;
                bestState = state;
            }
        }
    }

    if (!bestState)
        return std::nullopt;

    std::string relative;
    if (canonicalStr.size() > bestKey.size()) {
        relative = canonicalStr.substr(bestKey.size() + (bestKey == "/" ? 0 : 1));
    }
    return MatchedRoot{std::move(bestState), std::move(bestKey), std::move(relative)};
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
    if (inner)
        inner->shutdown();
}

auto UndoableSpace::notify(std::string const& notificationPath) -> void {
    if (inner)
        inner->notify(notificationPath);
}

} // namespace SP::History
