#include "SnapshotCachedPathSpace.hpp"

#include "core/NodeData.hpp"
#include "path/ConcretePath.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace SP {

namespace {
auto normalizeSnapshotPath(std::string_view path) -> std::string {
    ConcretePathString raw{std::string(path)};
    if (auto canonical = raw.canonicalized()) {
        return canonical->getPath();
    }
    return std::string(path);
}

auto hasGlobChars(std::string_view path) -> bool {
    return path.find_first_of("*?[") != std::string_view::npos;
}

auto isPathPrefix(std::string_view prefix, std::string_view path) -> bool {
    if (prefix == "/") {
        return true;
    }
    if (path.size() < prefix.size()) {
        return false;
    }
    if (path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (path.size() == prefix.size()) {
        return true;
    }
    return path[prefix.size()] == '/';
}
} // namespace

struct SnapshotCachedPathSpace::SnapshotState {
    bool enabled = false;
    bool dirty = false;
    bool rebuildInProgress = false;
    bool allowSynchronousRebuild = false;
    std::chrono::milliseconds debounce{200};
    std::chrono::steady_clock::time_point lastMutation{};
    std::chrono::steady_clock::time_point lastBuild{};
    std::size_t maxDirtyRoots = 128;
    std::unordered_map<std::string, std::vector<std::byte>> values;
    std::vector<std::string> dirtyRoots;
    std::size_t hitCount = 0;
    std::size_t missCount = 0;
    std::size_t rebuildCount = 0;
    std::size_t rebuildFailCount = 0;
    std::chrono::milliseconds lastRebuildMs{0};
    std::size_t bytes = 0;
    std::atomic<std::size_t> mutationCounter{0};
    bool workerRunning = false;
    bool stopWorker = false;
    std::condition_variable cv;
    std::thread worker;
    std::mutex mutex;
};

SnapshotCachedPathSpace::SnapshotCachedPathSpace(std::shared_ptr<PathSpaceBase> backing)
    : backing(std::move(backing)) {}

SnapshotCachedPathSpace::~SnapshotCachedPathSpace() {
    this->stopSnapshotWorker();
}

auto SnapshotCachedPathSpace::setSnapshotOptions(SnapshotOptions options) -> void {
    if (!this->snapshotState) {
        this->snapshotState = std::make_shared<SnapshotState>();
    }
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> guard(this->snapshotState->mutex);
        this->snapshotState->enabled = options.enabled;
        this->snapshotState->debounce = options.rebuildDebounce;
        this->snapshotState->maxDirtyRoots = std::max<std::size_t>(1, options.maxDirtyRoots);
        this->snapshotState->allowSynchronousRebuild = options.allowSynchronousRebuild;
        this->snapshotState->dirtyRoots.clear();
        this->snapshotState->values.clear();
        if (options.enabled) {
            this->snapshotState->dirtyRoots.emplace_back("/");
        }
        this->snapshotState->dirty = options.enabled;
        this->snapshotState->rebuildInProgress = false;
        this->snapshotState->lastMutation = now - this->snapshotState->debounce;
        this->snapshotState->lastBuild = std::chrono::steady_clock::time_point{};
        this->snapshotState->hitCount = 0;
        this->snapshotState->missCount = 0;
        this->snapshotState->rebuildCount = 0;
        this->snapshotState->rebuildFailCount = 0;
        this->snapshotState->lastRebuildMs = std::chrono::milliseconds{0};
        this->snapshotState->bytes = 0;
        this->snapshotState->stopWorker = false;
        this->snapshotState->cv.notify_all();
    }
    if (options.enabled) {
        this->startSnapshotWorker();
    } else {
        this->stopSnapshotWorker();
    }
}

auto SnapshotCachedPathSpace::snapshotEnabled() const noexcept -> bool {
    auto state = this->snapshotState;
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> guard(state->mutex);
    return state->enabled;
}

auto SnapshotCachedPathSpace::snapshotMetrics() const -> SnapshotMetrics {
    SnapshotMetrics metrics{};
    auto state = this->snapshotState;
    if (!state) {
        return metrics;
    }
    std::lock_guard<std::mutex> guard(state->mutex);
    metrics.hits = state->hitCount;
    metrics.misses = state->missCount;
    metrics.rebuilds = state->rebuildCount;
    metrics.rebuildFailures = state->rebuildFailCount;
    metrics.lastRebuildMs = state->lastRebuildMs;
    metrics.bytes = state->bytes;
    return metrics;
}

auto SnapshotCachedPathSpace::rebuildSnapshotNow() -> void {
    auto state = this->snapshotState;
    if (!state) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->enabled || state->rebuildInProgress) {
            return;
        }
        state->rebuildInProgress = true;
    }
    this->rebuildSnapshot(state);
}

auto SnapshotCachedPathSpace::startSnapshotWorker() -> void {
    auto state = this->snapshotState;
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> guard(state->mutex);
    if (state->workerRunning) {
        return;
    }
    state->stopWorker = false;
    state->workerRunning = true;
    state->worker = std::thread([state, owner = this]() {
        std::unique_lock<std::mutex> lock(state->mutex);
        while (true) {
            state->cv.wait(lock, [&]() {
                return state->stopWorker || (state->enabled && state->dirty);
            });
            if (state->stopWorker) {
                break;
            }
            if (!state->enabled || !state->dirty) {
                continue;
            }
            auto nextWake = state->lastMutation + state->debounce;
            if (state->cv.wait_until(lock, nextWake, [&]() { return state->stopWorker; })) {
                if (state->stopWorker) {
                    break;
                }
            }
            if (state->rebuildInProgress) {
                state->cv.wait(lock, [&]() { return state->stopWorker || !state->rebuildInProgress; });
                if (state->stopWorker) {
                    break;
                }
                if (!state->enabled || !state->dirty) {
                    continue;
                }
            }
            if (!state->enabled || !state->dirty) {
                continue;
            }
            state->rebuildInProgress = true;
            lock.unlock();
            owner->rebuildSnapshot(state);
            lock.lock();
        }
        state->workerRunning = false;
        state->cv.notify_all();
    });
}

auto SnapshotCachedPathSpace::stopSnapshotWorker() -> void {
    auto state = this->snapshotState;
    if (!state) {
        return;
    }
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->workerRunning) {
            return;
        }
        state->stopWorker = true;
        state->cv.notify_all();
        toJoin = std::move(state->worker);
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

void SnapshotCachedPathSpace::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    auto ctxCopy = context;
    auto prefixCopy = prefix;
    PathSpaceBase::adoptContextAndPrefix(std::move(context), std::move(prefix));
    if (this->backing) {
        this->backing->adoptContextAndPrefix(std::move(ctxCopy), std::move(prefixCopy));
    }
}

auto SnapshotCachedPathSpace::markSnapshotDirty(std::string_view pathView) -> void {
    auto state = this->snapshotState;
    if (!state) {
        return;
    }
    auto hasGlob = hasGlobChars(pathView);
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> guard(state->mutex);
    if (!state->enabled) {
        return;
    }
    state->dirty = true;
    state->lastMutation = now;
    state->mutationCounter.fetch_add(1, std::memory_order_acq_rel);
    if (hasGlob) {
        state->dirtyRoots.clear();
        state->dirtyRoots.emplace_back("/");
        state->cv.notify_all();
        return;
    }

    auto normalized = normalizeSnapshotPath(pathView);
    if (state->dirtyRoots.size() >= state->maxDirtyRoots) {
        state->dirtyRoots.clear();
        state->dirtyRoots.emplace_back("/");
        state->cv.notify_all();
        return;
    }
    for (auto const& root : state->dirtyRoots) {
        if (isPathPrefix(root, normalized)) {
            return;
        }
    }
    state->dirtyRoots.erase(std::remove_if(state->dirtyRoots.begin(),
                                           state->dirtyRoots.end(),
                                           [&](std::string const& root) {
                                               return isPathPrefix(normalized, root);
                                           }),
                            state->dirtyRoots.end());
    state->dirtyRoots.push_back(std::move(normalized));
    state->cv.notify_all();
}

void SnapshotCachedPathSpace::markSnapshotDirty(Iterator const& path) {
    this->markSnapshotDirty(path.toStringView());
}

auto SnapshotCachedPathSpace::rebuildSnapshot(std::shared_ptr<SnapshotState> const& state) -> void {
    if (!state || !this->backing) {
        if (state) {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->rebuildFailCount += 1;
            state->rebuildInProgress = false;
            state->cv.notify_all();
        }
        return;
    }
    auto start = std::chrono::steady_clock::now();
    auto startMutation = state->mutationCounter.load(std::memory_order_acquire);
    std::unordered_map<std::string, std::vector<std::byte>> nextValues;
    VisitOptions options{};
    options.root = "/";
    options.maxDepth = VisitOptions::UnlimitedDepth;
    options.maxChildren = VisitOptions::UnlimitedChildren;
    options.includeNestedSpaces = true;
    options.includeValues = true;

    auto visitResult = this->backing->visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (!entry.hasValue) {
                return VisitControl::Continue;
            }
            auto bytes = VisitDetail::Access::SerializeNodeData(handle);
            if (!bytes) {
                return VisitControl::Continue;
            }
            nextValues.emplace(entry.path, std::move(*bytes));
            return VisitControl::Continue;
        },
        options);

    std::lock_guard<std::mutex> guard(state->mutex);
    auto endMutation = state->mutationCounter.load(std::memory_order_acquire);
    if (!state->enabled) {
        state->rebuildInProgress = false;
        state->cv.notify_all();
        return;
    }
    if (!visitResult.has_value()) {
        state->rebuildFailCount += 1;
        state->rebuildInProgress = false;
        state->cv.notify_all();
        return;
    }
    std::size_t bytes = 0;
    for (auto const& [key, value] : nextValues) {
        (void)key;
        bytes += value.size();
    }
    state->values = std::move(nextValues);
    if (endMutation == startMutation) {
        state->dirtyRoots.clear();
        state->dirty = false;
    } else {
        state->dirty = true;
    }
    state->lastBuild = std::chrono::steady_clock::now();
    state->lastRebuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(state->lastBuild - start);
    state->rebuildCount += 1;
    state->bytes = bytes;
    state->rebuildInProgress = false;
    state->cv.notify_all();
}

auto SnapshotCachedPathSpace::trySnapshotRead(Iterator const& path,
                                              InputMetadata const& inputMetadata,
                                              Out const& options,
                                              void* obj) -> bool {
    auto state = this->snapshotState;
    if (!state) {
        return false;
    }
    if (options.doPop || options.doBlock) {
        return false;
    }
    if (inputMetadata.dataCategory == DataCategory::Execution) {
        return false;
    }
    auto pathView = path.toStringView();
    if (hasGlobChars(pathView)) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    bool shouldRebuild = false;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->enabled) {
            return false;
        }
        if (state->allowSynchronousRebuild && !state->workerRunning && state->dirty && !state->rebuildInProgress
            && now - state->lastMutation >= state->debounce) {
            state->rebuildInProgress = true;
            shouldRebuild = true;
        }
    }

    if (shouldRebuild) {
        this->rebuildSnapshot(state);
    }

    std::vector<std::byte> snapshotBytes;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->enabled) {
            return false;
        }
        auto normalized = normalizeSnapshotPath(pathView);
        for (auto const& root : state->dirtyRoots) {
            if (isPathPrefix(root, normalized)) {
                state->missCount += 1;
                return false;
            }
        }
        auto it = state->values.find(normalized);
        if (it == state->values.end()) {
            state->missCount += 1;
            return false;
        }
        snapshotBytes = it->second;
        state->hitCount += 1;
    }

    auto snapshot = NodeData::deserializeSnapshot(
        std::span<const std::byte>{snapshotBytes.data(), snapshotBytes.size()});
    if (!snapshot) {
        return false;
    }
    if (auto error = snapshot->deserialize(obj, inputMetadata)) {
        return false;
    }
    return true;
}

auto SnapshotCachedPathSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    if (!this->backing) {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "SnapshotCachedPathSpace backing not set"}}};
    }
    auto ret = this->backing->in(path, data);
    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0 || ret.nbrValuesSuppressed > 0) {
        this->markSnapshotDirty(path);
    }
    return ret;
}

auto SnapshotCachedPathSpace::out(Iterator const& path,
                                  InputMetadata const& inputMetadata,
                                  Out const& options,
                                  void* obj) -> std::optional<Error> {
    if (!this->backing) {
        return Error{Error::Code::InvalidPermissions, "SnapshotCachedPathSpace backing not set"};
    }
    if (this->trySnapshotRead(path, inputMetadata, options, obj)) {
        return std::nullopt;
    }
    auto err = this->backing->out(path, inputMetadata, options, obj);
    if (!err && options.doPop) {
        this->markSnapshotDirty(path);
    }
    return err;
}

auto SnapshotCachedPathSpace::shutdown() -> void {
    this->stopSnapshotWorker();
    if (this->backing) {
        this->backing->shutdown();
    }
}

auto SnapshotCachedPathSpace::notify(std::string const& notificationPath) -> void {
    if (this->backing) {
        this->backing->notify(notificationPath);
    }
}

auto SnapshotCachedPathSpace::visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> {
    if (!this->backing) {
        return std::unexpected(Error{Error::Code::InvalidPermissions, "SnapshotCachedPathSpace backing not set"});
    }
    return this->backing->visit(visitor, options);
}

auto SnapshotCachedPathSpace::spanPackConst(std::span<const std::string> paths,
                                            InputMetadata const& metadata,
                                            Out const& options,
                                            SpanPackConstCallback const& fn) const -> Expected<void> {
    if (!this->backing) {
        return std::unexpected(Error{Error::Code::InvalidPermissions, "SnapshotCachedPathSpace backing not set"});
    }
    return this->backing->spanPackConst(paths, metadata, options, fn);
}

auto SnapshotCachedPathSpace::spanPackMut(std::span<const std::string> paths,
                                          InputMetadata const& metadata,
                                          Out const& options,
                                          SpanPackMutCallback const& fn) const -> Expected<void> {
    if (!this->backing) {
        return std::unexpected(Error{Error::Code::InvalidPermissions, "SnapshotCachedPathSpace backing not set"});
    }
    auto result = this->backing->spanPackMut(paths, metadata, options, fn);
    if (result.has_value()) {
        for (auto const& path : paths) {
            const_cast<SnapshotCachedPathSpace*>(this)->markSnapshotDirty(path);
        }
    }
    return result;
}

auto SnapshotCachedPathSpace::packInsert(std::span<const std::string> paths,
                                         InputMetadata const& metadata,
                                         std::span<void const* const> values) -> InsertReturn {
    if (!this->backing) {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "SnapshotCachedPathSpace backing not set"}}};
    }
    auto ret = this->backing->packInsert(paths, metadata, values);
    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0 || ret.nbrValuesSuppressed > 0) {
        for (auto const& path : paths) {
            this->markSnapshotDirty(path);
        }
    }
    return ret;
}

auto SnapshotCachedPathSpace::packInsertSpans(std::span<const std::string> paths,
                                              std::span<SpanInsertSpec const> specs) -> InsertReturn {
    if (!this->backing) {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "SnapshotCachedPathSpace backing not set"}}};
    }
    auto ret = this->backing->packInsertSpans(paths, specs);
    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0 || ret.nbrValuesSuppressed > 0) {
        for (auto const& path : paths) {
            this->markSnapshotDirty(path);
        }
    }
    return ret;
}

auto SnapshotCachedPathSpace::listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> {
    if (!this->backing) {
        return {};
    }
    auto children = this->backing->read<Children>(canonicalPath);
    if (!children.has_value()) {
        return {};
    }
    return children->names;
}

auto SnapshotCachedPathSpace::typedPeekFuture(std::string_view pathIn) const -> std::optional<FutureAny> {
    if (!this->backing) {
        return std::nullopt;
    }
    auto fut = this->backing->read(pathIn);
    if (!fut.has_value()) {
        return std::nullopt;
    }
    return *fut;
}

} // namespace SP
