#include "SnapshotCachedPathSpace.hpp"

#include "core/NodeData.hpp"
#include "path/ConcretePath.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace SP {

namespace {
struct StringViewHash {
    using is_transparent = void;
    auto operator()(std::string_view value) const noexcept -> std::size_t {
        return std::hash<std::string_view>{}(value);
    }
};

struct StringViewEq {
    using is_transparent = void;
    auto operator()(std::string_view lhs, std::string_view rhs) const noexcept -> bool {
        return lhs == rhs;
    }
};

using SnapshotValuePtr = std::shared_ptr<const std::vector<std::byte>>;
using SnapshotValueMap = std::unordered_map<std::string, SnapshotValuePtr, StringViewHash, StringViewEq>;
using DirtyRootSet = std::unordered_set<std::string, StringViewHash, StringViewEq>;

struct SnapshotView {
    SnapshotValueMap values;
    std::size_t bytes = 0;
};

struct DirtyRoots {
    DirtyRootSet roots;
};

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

auto dirtyRootsContainPrefix(DirtyRootSet const& roots, std::string_view path) -> bool {
    if (roots.empty()) {
        return false;
    }
    if (roots.find("/") != roots.end()) {
        return true;
    }
    if (path.empty()) {
        return false;
    }
    if (path.front() != '/') {
        return roots.find(path) != roots.end();
    }
    std::size_t pos = 1;
    while (pos < path.size()) {
        auto next = path.find('/', pos);
        if (next == std::string_view::npos) {
            return roots.find(path) != roots.end();
        }
        auto prefix = std::string_view(path.data(), next);
        if (roots.find(prefix) != roots.end()) {
            return true;
        }
        pos = next + 1;
    }
    return roots.find(path) != roots.end();
}
} // namespace

struct SnapshotCachedPathSpace::SnapshotState {
    std::atomic<bool> enabled{false};
    std::atomic<bool> allowSynchronousRebuild{false};
    std::shared_ptr<const SnapshotView> snapshotView;
    std::shared_ptr<const DirtyRoots> dirtyRootsView;
    std::atomic<std::size_t> hitCount{0};
    std::atomic<std::size_t> missCount{0};
    std::atomic<std::size_t> rebuildCount{0};
    std::atomic<std::size_t> rebuildFailCount{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<std::int64_t> lastRebuildMs{0};
    std::atomic<std::size_t> mutationCounter{0};
    bool dirty = false;
    bool rebuildInProgress = false;
    std::chrono::milliseconds debounce{200};
    std::chrono::steady_clock::time_point lastMutation{};
    std::chrono::steady_clock::time_point lastBuild{};
    std::size_t maxDirtyRoots = 128;
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
    auto snapshotView = std::make_shared<SnapshotView>();
    auto dirtyRoots = std::make_shared<DirtyRoots>();
    if (options.enabled) {
        dirtyRoots->roots.emplace("/");
    }
    std::shared_ptr<const SnapshotView> publishedView = snapshotView;
    std::shared_ptr<const DirtyRoots> publishedRoots = dirtyRoots;
    {
        std::lock_guard<std::mutex> guard(this->snapshotState->mutex);
        this->snapshotState->enabled.store(options.enabled, std::memory_order_release);
        this->snapshotState->debounce = options.rebuildDebounce;
        this->snapshotState->maxDirtyRoots = std::max<std::size_t>(1, options.maxDirtyRoots);
        this->snapshotState->allowSynchronousRebuild.store(options.allowSynchronousRebuild, std::memory_order_release);
        this->snapshotState->dirty = options.enabled;
        this->snapshotState->rebuildInProgress = false;
        this->snapshotState->lastMutation = now - this->snapshotState->debounce;
        this->snapshotState->lastBuild = std::chrono::steady_clock::time_point{};
        this->snapshotState->hitCount.store(0, std::memory_order_release);
        this->snapshotState->missCount.store(0, std::memory_order_release);
        this->snapshotState->rebuildCount.store(0, std::memory_order_release);
        this->snapshotState->rebuildFailCount.store(0, std::memory_order_release);
        this->snapshotState->lastRebuildMs.store(0, std::memory_order_release);
        this->snapshotState->bytes.store(0, std::memory_order_release);
        this->snapshotState->stopWorker = false;
        std::atomic_store_explicit(&this->snapshotState->snapshotView, publishedView, std::memory_order_release);
        std::atomic_store_explicit(&this->snapshotState->dirtyRootsView, publishedRoots, std::memory_order_release);
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
    return state->enabled.load(std::memory_order_acquire);
}

auto SnapshotCachedPathSpace::snapshotMetrics() const -> SnapshotMetrics {
    SnapshotMetrics metrics{};
    auto state = this->snapshotState;
    if (!state) {
        return metrics;
    }
    metrics.hits = state->hitCount.load(std::memory_order_acquire);
    metrics.misses = state->missCount.load(std::memory_order_acquire);
    metrics.rebuilds = state->rebuildCount.load(std::memory_order_acquire);
    metrics.rebuildFailures = state->rebuildFailCount.load(std::memory_order_acquire);
    metrics.lastRebuildMs = std::chrono::milliseconds{state->lastRebuildMs.load(std::memory_order_acquire)};
    metrics.bytes = state->bytes.load(std::memory_order_acquire);
    return metrics;
}

auto SnapshotCachedPathSpace::rebuildSnapshotNow() -> void {
    auto state = this->snapshotState;
    if (!state) {
        return;
    }
    {
        std::unique_lock<std::mutex> guard(state->mutex);
        if (!state->enabled.load(std::memory_order_acquire)) {
            return;
        }
        if (state->rebuildInProgress) {
            state->cv.wait(guard, [&]() {
                return !state->enabled.load(std::memory_order_acquire) || !state->rebuildInProgress;
            });
            if (!state->enabled.load(std::memory_order_acquire)) {
                return;
            }
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
                return state->stopWorker || (state->enabled.load(std::memory_order_acquire) && state->dirty);
            });
            if (state->stopWorker) {
                break;
            }
            if (!state->enabled.load(std::memory_order_acquire) || !state->dirty) {
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
                if (!state->enabled.load(std::memory_order_acquire) || !state->dirty) {
                    continue;
                }
            }
            if (!state->enabled.load(std::memory_order_acquire) || !state->dirty) {
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
    if (!state->enabled.load(std::memory_order_acquire)) {
        return;
    }
    state->dirty = true;
    state->lastMutation = now;
    state->mutationCounter.fetch_add(1, std::memory_order_acq_rel);
    auto currentRoots = std::atomic_load_explicit(&state->dirtyRootsView, std::memory_order_acquire);
    if (hasGlob) {
        auto nextRoots = std::make_shared<DirtyRoots>();
        nextRoots->roots.emplace("/");
        std::shared_ptr<const DirtyRoots> published = nextRoots;
        std::atomic_store_explicit(&state->dirtyRootsView, std::move(published), std::memory_order_release);
        state->cv.notify_all();
        return;
    }

    auto normalized = normalizeSnapshotPath(pathView);
    if (currentRoots && dirtyRootsContainPrefix(currentRoots->roots, normalized)) {
        state->cv.notify_all();
        return;
    }
    auto currentSize = currentRoots ? currentRoots->roots.size() : 0;
    if (currentSize >= state->maxDirtyRoots) {
        auto nextRoots = std::make_shared<DirtyRoots>();
        nextRoots->roots.emplace("/");
        std::shared_ptr<const DirtyRoots> published = nextRoots;
        std::atomic_store_explicit(&state->dirtyRootsView, std::move(published), std::memory_order_release);
        state->cv.notify_all();
        return;
    }
    auto nextRoots = std::make_shared<DirtyRoots>();
    if (currentRoots) {
        nextRoots->roots = currentRoots->roots;
    }
    for (auto it = nextRoots->roots.begin(); it != nextRoots->roots.end();) {
        if (isPathPrefix(normalized, *it)) {
            it = nextRoots->roots.erase(it);
        } else {
            ++it;
        }
    }
    nextRoots->roots.emplace(std::move(normalized));
    std::shared_ptr<const DirtyRoots> published = nextRoots;
    std::atomic_store_explicit(&state->dirtyRootsView, std::move(published), std::memory_order_release);
    state->cv.notify_all();
}

void SnapshotCachedPathSpace::markSnapshotDirty(Iterator const& path) {
    this->markSnapshotDirty(path.toStringView());
}

auto SnapshotCachedPathSpace::rebuildSnapshot(std::shared_ptr<SnapshotState> const& state) -> void {
    if (!state || !this->backing) {
        if (state) {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->rebuildFailCount.fetch_add(1, std::memory_order_acq_rel);
            state->rebuildInProgress = false;
            state->cv.notify_all();
        }
        return;
    }
    auto start = std::chrono::steady_clock::now();
    auto startMutation = state->mutationCounter.load(std::memory_order_acquire);
    SnapshotValueMap nextValues;
    std::size_t nextBytes = 0;
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
            nextBytes += bytes->size();
            nextValues.emplace(entry.path, std::make_shared<const std::vector<std::byte>>(std::move(*bytes)));
            return VisitControl::Continue;
        },
        options);

    std::lock_guard<std::mutex> guard(state->mutex);
    auto endMutation = state->mutationCounter.load(std::memory_order_acquire);
    if (!state->enabled.load(std::memory_order_acquire)) {
        state->rebuildInProgress = false;
        state->cv.notify_all();
        return;
    }
    if (!visitResult.has_value()) {
        state->rebuildFailCount.fetch_add(1, std::memory_order_acq_rel);
        state->rebuildInProgress = false;
        state->cv.notify_all();
        return;
    }
    auto nextView = std::make_shared<SnapshotView>();
    nextView->values = std::move(nextValues);
    nextView->bytes = nextBytes;
    std::shared_ptr<const SnapshotView> publishedView = nextView;
    std::atomic_store_explicit(&state->snapshotView, std::move(publishedView), std::memory_order_release);
    if (endMutation == startMutation) {
        auto clearedRoots = std::make_shared<DirtyRoots>();
        std::shared_ptr<const DirtyRoots> publishedRoots = clearedRoots;
        std::atomic_store_explicit(&state->dirtyRootsView, std::move(publishedRoots), std::memory_order_release);
        state->dirty = false;
    } else {
        state->dirty = true;
    }
    state->lastBuild = std::chrono::steady_clock::now();
    state->lastRebuildMs.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(state->lastBuild - start).count(),
        std::memory_order_release);
    state->rebuildCount.fetch_add(1, std::memory_order_acq_rel);
    state->bytes.store(nextBytes, std::memory_order_release);
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

    if (!state->enabled.load(std::memory_order_acquire)) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    bool shouldRebuild = false;
    if (state->allowSynchronousRebuild.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->enabled.load(std::memory_order_acquire) && !state->workerRunning && state->dirty && !state->rebuildInProgress
            && now - state->lastMutation >= state->debounce) {
            state->rebuildInProgress = true;
            shouldRebuild = true;
        }
    }

    if (shouldRebuild) {
        this->rebuildSnapshot(state);
    }

    if (!state->enabled.load(std::memory_order_acquire)) {
        return false;
    }

    auto normalized = normalizeSnapshotPath(pathView);
    auto dirtyRoots = std::atomic_load_explicit(&state->dirtyRootsView, std::memory_order_acquire);
    if (dirtyRoots && dirtyRootsContainPrefix(dirtyRoots->roots, normalized)) {
        state->missCount.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    auto view = std::atomic_load_explicit(&state->snapshotView, std::memory_order_acquire);
    if (!view) {
        return false;
    }
    auto it = view->values.find(normalized);
    if (it == view->values.end()) {
        state->missCount.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    auto snapshotBytes = it->second;
    if (!snapshotBytes) {
        return false;
    }
    state->hitCount.fetch_add(1, std::memory_order_acq_rel);

    auto snapshot = NodeData::deserializeSnapshot(
        std::span<const std::byte>{snapshotBytes->data(), snapshotBytes->size()});
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
