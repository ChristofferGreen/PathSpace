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
namespace UndoUtilsAlias = SP::History::UndoUtils;
namespace UndoPaths = SP::History::UndoUtils::Paths;
namespace UndoMetadata = SP::History::UndoMetadata;

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

    HistoryOptions resolvedOptions = defaultOptions;
    if (opts.maxEntries != 0) {
        resolvedOptions.maxEntries = opts.maxEntries;
    }
    if (opts.maxBytesRetained != 0) {
        resolvedOptions.maxBytesRetained = opts.maxBytesRetained;
    }
    resolvedOptions.manualGarbageCollect = opts.manualGarbageCollect;
    resolvedOptions.allowNestedUndo      = opts.allowNestedUndo;
    resolvedOptions.persistHistory       = resolvedOptions.persistHistory || opts.persistHistory;
    if (!opts.persistenceRoot.empty()) {
        resolvedOptions.persistenceRoot = opts.persistenceRoot;
    }
    if (!opts.persistenceNamespace.empty()) {
        resolvedOptions.persistenceNamespace = opts.persistenceNamespace;
    }
    if (opts.ramCacheEntries > 0) {
        resolvedOptions.ramCacheEntries = opts.ramCacheEntries;
    }
    if (resolvedOptions.ramCacheEntries == 0) {
        resolvedOptions.ramCacheEntries = 8;
    }
    if (opts.maxDiskBytes != 0) {
        resolvedOptions.maxDiskBytes = opts.maxDiskBytes;
    }
    if (opts.keepLatestFor.count() > 0) {
        resolvedOptions.keepLatestFor = opts.keepLatestFor;
    }
    resolvedOptions.restoreFromPersistence =
        resolvedOptions.restoreFromPersistence && opts.restoreFromPersistence;
    if (opts.sharedStackKey.has_value()) {
        if (opts.sharedStackKey->empty()) {
            resolvedOptions.sharedStackKey.reset();
        } else {
            resolvedOptions.sharedStackKey = opts.sharedStackKey;
        }
    }

    {
        std::scoped_lock lock(rootsMutex);
        if (roots.find(normalized) != roots.end()) {
            return std::unexpected(Error{Error::Code::UnknownError, "History already enabled for path"});
        }
        if (!(defaultOptions.allowNestedUndo && resolvedOptions.allowNestedUndo)) {
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
        if (resolvedOptions.sharedStackKey && !resolvedOptions.sharedStackKey->empty()) {
            for (auto const& [existingPath, existingState] : roots) {
                if (!existingState || !existingState->options.sharedStackKey
                    || existingState->options.sharedStackKey->empty()) {
                    continue;
                }
                if (*existingState->options.sharedStackKey == *resolvedOptions.sharedStackKey) {
                    std::string message = "UndoableSpace does not support shared undo stacks across multiple roots "
                                          "(key '" + *resolvedOptions.sharedStackKey + "' already bound to '" + existingPath + "')";
                    return std::unexpected(Error{Error::Code::InvalidPermissions, std::move(message)});
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
    state->options            = std::move(resolvedOptions);
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

auto UndoableSpace::listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> {
    if (!inner) {
        return {};
    }
    ConcretePathStringView view{canonicalPath};
    return inner->listChildren(view);
}

} // namespace SP::History
