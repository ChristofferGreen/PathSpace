#include "PathSpace.hpp"
#include "task/TaskPool.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"
#include "path/ConcretePath.hpp"
#include "core/NodeData.hpp"
#include "type/InputData.hpp"
#include <algorithm>
#include <deque>
#include <cstdlib>
#include <cstddef>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <vector>

namespace SP {

static bool subtree_has_payload(Node const& node) {
    if (node.data && !node.data->empty()) {
        return true;
    }
    if (node.podPayload && node.podPayload->size() > 0) {
        return true;
    }
    bool has = false;
    node.children.for_each([&](auto const& kv) {
        if (has) return;
        auto const& child = kv.second;
        if (child && subtree_has_payload(*child)) {
            has = true;
        }
    });
    return has;
}

auto gatherOrderedChildren(Node const& node) -> std::vector<std::string> {
    std::vector<std::string> names;
    node.children.for_each([&](auto const& kv) {
        auto const& child = kv.second;
        if (child && subtree_has_payload(*child)) {
            names.emplace_back(kv.first);
        }
    });
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

auto mergeWithNested(Node const& node, std::vector<std::string> names) -> std::vector<std::string> {
    std::vector<std::pair<std::shared_ptr<PathSpaceBase>, std::size_t>> nestedList;
    {
        std::lock_guard<std::mutex> guard(node.payloadMutex);
        if (node.data) {
            auto nestedCount = node.data->nestedCount();
            nestedList.reserve(nestedCount);
            for (std::size_t idx = 0; idx < nestedCount; ++idx) {
                if (auto nestedSpace = node.data->borrowNestedShared(idx)) {
                    nestedList.emplace_back(std::move(nestedSpace), idx);
                }
            }
        }
    }
    for (auto const& entry : nestedList) {
        auto const& nestedSpace = entry.first;
        auto const  idx         = entry.second;
        auto nestedNames = nestedSpace->read<Children>("/");
        if (!nestedNames) {
            continue;
        }
        for (auto const& child : nestedNames->names) {
            if (idx == 0) {
                names.emplace_back(child);
            } else {
                names.emplace_back(append_index_suffix(child, idx));
            }
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

auto buildRemainingPath(std::vector<std::string> const& components, std::size_t startIndex) -> std::string {
    if (startIndex >= components.size()) {
        return "/";
    }
    std::string result{"/"};
    result.append(components[startIndex]);
    for (std::size_t idx = startIndex + 1; idx < components.size(); ++idx) {
        result.push_back('/');
        result.append(components[idx]);
    }
    return result;
}

auto joinBaseComponents(std::vector<std::string> const& components) -> std::string {
    if (components.empty()) {
        return "/";
    }
    std::string result;
    for (std::size_t i = 0; i < components.size(); ++i) {
        result.push_back('/');
        result.append(components[i]);
    }
    return result.empty() ? std::string("/") : result;
}

auto baseComponentsFromCanonical(std::string const& canonicalPath) -> Expected<std::vector<std::string>> {
    ConcretePathStringView view{canonicalPath};
    auto                   components = view.components();
    if (!components) {
        return std::unexpected(components.error());
    }
    std::vector<std::string> base;
    base.reserve(components->size());
    for (auto const& comp : *components) {
        auto parsed = parse_indexed_component(comp);
        if (parsed.malformed) {
            return std::unexpected(Error{Error::Code::InvalidPath, "Malformed indexed path component"});
        }
        base.emplace_back(parsed.base);
    }
    return base;
}

namespace {

auto appendComponent(std::string const& base, std::string_view component) -> std::string {
    if (base.empty() || base == "/") {
        return std::string{"/"}.append(component);
    }
    if (base.back() == '/') {
        std::string result = base;
        result.append(component);
        return result;
    }
    std::string result = base;
    result.push_back('/');
    result.append(component);
    return result;
}

auto makeMountPrefix(std::string const& spacePrefix, std::string const& path) -> std::string {
    if (spacePrefix.empty()) {
        return path;
    }
    std::string result = spacePrefix;
    result.append(path);
    return result;
}

// Upper bound on one blocking wait before the reader re-checks the space on its own. Normal
// operation never relies on it (writers notify); it only bounds the damage of a writer that
// forgets to. PATHSPACE_WAIT_RECHECK_MS overrides it (tests set it very large to prove that no
// wakeup is being missed).
auto waitSafetyRecheckInterval() -> std::chrono::milliseconds {
    static auto const interval = []() {
        constexpr std::chrono::milliseconds Default{50};
        if (const char* env = std::getenv("PATHSPACE_WAIT_RECHECK_MS")) {
            char* endptr = nullptr;
            long  ms     = std::strtol(env, &endptr, 10);
            if (endptr != env && ms > 0) {
                return std::chrono::milliseconds{ms};
            }
        }
        return Default;
    }();
    return interval;
}

auto countCategory(std::deque<ElementType> const& types, DataCategory category) -> std::size_t {
    std::size_t total = 0;
    for (auto const& t : types) {
        if (t.category == category) {
            total += t.elements;
        }
    }
    return total;
}

} // namespace

void PathSpace::retargetNestedMounts(Node const* node, std::string const& basePath) {
    if (!node || !this->sharedContext()) {
        return;
    }

    std::vector<std::pair<std::shared_ptr<PathSpaceBase>, std::size_t>> nestedTargets;
    {
        std::lock_guard<std::mutex> guard(node->payloadMutex);
        if (!node->data) {
            return;
        }
        auto count = node->data->nestedCount();
        nestedTargets.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (auto nested = node->data->borrowNestedShared(i)) {
                nestedTargets.emplace_back(std::move(nested), i);
            }
        }
    }

    auto ctx = this->sharedContext();
    auto pfx = this->getPrefix();
    for (auto const& target : nestedTargets) {
        auto mountPrefix = makeMountPrefix(pfx, append_index_suffix(basePath, target.second));
        target.first->adoptContextAndPrefix(ctx, mountPrefix);
    }
}

PathSpace::PathSpace(TaskPool* pool) {
    sp_log("PathSpace::PathSpace", "Function Called");
    // Ensure a non-null executor without relying on a global singleton.
    // If no pool is provided, create and own a TaskPool and use it as the executor.
    if (!pool) {
        this->pool = &TaskPool::Instance();
    } else {
        this->pool = pool;
    }
    Executor* exec = static_cast<Executor*>(this->pool);
    this->context = std::make_shared<PathSpaceContext>(exec);
    this->setExecutor(exec);
};

PathSpace::PathSpace(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    sp_log("PathSpace::PathSpace(context)", "Function Called");
    this->context = std::move(context);

    this->prefix = std::move(prefix);

    // Prefer the executor already attached to the shared context; otherwise, fall back
    // to the global TaskPool so immediate executions still have a scheduler.
    Executor* exec = nullptr;
    if (this->context) {
        exec = this->context->executor();
    }

    if (!exec) {
        this->pool = &TaskPool::Instance();
        exec       = static_cast<Executor*>(this->pool);
        if (this->context) {
            this->context->setExecutor(exec);
        }
    } else {
        this->pool = dynamic_cast<TaskPool*>(exec);
        if (!this->pool) {
            this->pool = &TaskPool::Instance();
        }
    }

    this->setExecutor(exec);
}

PathSpace::PathSpace(PathSpace const& other) {
    sp_log("PathSpace::PathSpace(copy)", "Function Called");
    this->pool = other.pool ? other.pool : &TaskPool::Instance();
    Executor* exec = static_cast<Executor*>(this->pool);
    this->context = std::make_shared<PathSpaceContext>(exec);
    this->setExecutor(exec);
    this->prefix = other.prefix;
    this->activeOutCount.store(0, std::memory_order_relaxed);
    this->clearingInProgress.store(false, std::memory_order_release);
    this->copyFrom(other, nullptr);
}

auto PathSpace::operator=(PathSpace const& other) -> PathSpace& {
    sp_log("PathSpace::operator=(copy)", "Function Called");
    if (this == &other) {
        return *this;
    }
    this->clear();
    this->ownedPool.reset();
    this->pool = other.pool ? other.pool : &TaskPool::Instance();
    Executor* exec = static_cast<Executor*>(this->pool);
    this->context = std::make_shared<PathSpaceContext>(exec);
    this->setExecutor(exec);
    this->prefix = other.prefix;
    this->activeOutCount.store(0, std::memory_order_relaxed);
    this->clearingInProgress.store(false, std::memory_order_release);
    this->copyFrom(other, nullptr);
    return *this;
}

auto PathSpace::clone(CopyStats* stats) const -> PathSpace {
    PathSpace copy{this->pool ? this->pool : &TaskPool::Instance()};
    copy.prefix = this->prefix;
    copy.copyFrom(*this, stats);
    return copy;
}

void PathSpace::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> newContext, std::string newPrefix) {
    sp_log("PathSpace::adoptContextAndPrefix", "Function Called");
    std::string                       previousPrefix;
    std::shared_ptr<PathSpaceContext> previousContext;
    {
        // Readers snapshot context/prefix under the same mutex (see PathSpaceBase::getContext).
        std::lock_guard<std::mutex> guard(this->bindingMutex);
        previousPrefix  = this->prefix;
        previousContext = this->context;
        this->context   = newContext;
        this->prefix    = newPrefix;
    }
    // Blocked readers wait on the previous context under the previous prefix; wake them so they
    // re-evaluate against the new binding.
    if (previousContext && previousContext != newContext) {
        previousContext->notifyAll();
    }
    auto const& ctx = newContext;
    auto const& pfx = newPrefix;
    if (ctx && ctx->executor()) {
        this->setExecutor(ctx->executor());
    }
    // Retarget tasks and nested spaces to the newly adopted context/executor.
    if (ctx) {
        auto* root = this->getRootNode();
        if (root) {
            std::vector<std::pair<Node*, std::string>> stack{{root, "/"}};
            while (!stack.empty()) {
                auto [node, path] = stack.back();
                stack.pop_back();
                {
                    std::lock_guard<std::mutex> guard(node->payloadMutex);
                    if (node->data) {
                        node->data->retargetTasks(this->getNotificationSink(),
                                                  this->getExecutor(),
                                                  previousPrefix,
                                                  pfx);
                        auto nestedCount = node->data->nestedCount();
                        for (std::size_t i = 0; i < nestedCount; ++i) {
                            if (auto* nested = node->data->nestedAt(i)) {
                                auto mountPrefix = makeMountPrefix(pfx, append_index_suffix(path, i));
                                nested->adoptContextAndPrefix(ctx, mountPrefix);
                            }
                        }
                    }
                }
                node->children.for_each([&](auto const& kv) {
                    auto childPath = appendComponent(path, kv.first);
                    stack.emplace_back(kv.second.get(), childPath);
                });
            }
        }
    }
}
void PathSpace::setOwnedPool(TaskPool* p) {
    // Optional helper: if transferring ownership explicitly, adopt and manage the pool lifetime.
    if (p) {
        // Do not take ownership of the global singleton
        if (p == &TaskPool::Instance()) {
            this->ownedPool.reset();
            this->pool = p;
        } else {
            this->ownedPool.reset(p);
            this->pool = this->ownedPool.get();
        }
        this->setExecutor(static_cast<Executor*>(this->pool));
    }
}

PathSpace::~PathSpace() {
    auto ctx = this->getContext();
    sp_log("PathSpace::~PathSpace", "Function Called");
    if (ctx) {
        this->shutdown();
    }
    // If we own a TaskPool instance, ensure worker threads are stopped before destruction.
    // Never shut down the global singleton instance here.
    if (this->ownedPool) {
        TaskPool* owned = this->ownedPool.get();
        if (owned && owned != &TaskPool::Instance()) {
            owned->shutdown();
        }
        this->ownedPool.reset();
        if (this->pool == owned) {
            this->pool = nullptr;
        }
    }
}

auto PathSpace::clear() -> void {
    auto ctx = this->getContext();
    sp_log("PathSpace::clear", "Function Called");
    this->clearingInProgress.store(true, std::memory_order_release);

    if (ctx) {
        // Wake any waiters before clearing to avoid dangling waits
        ctx->notifyAll();
    }

    while (this->activeOutCount.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!ctx) {
        this->leaf.clear();
        this->clearingInProgress.store(false, std::memory_order_release);
        return;
    }

    this->leaf.clear();
    ctx->clearWaits();
    this->clearingInProgress.store(false, std::memory_order_release);
}

auto PathSpace::shutdown() -> void {
    auto ctx = this->getContext();
    sp_log("PathSpace::shutdown", "Function Called");
    sp_log("PathSpace::shutdown Starting shutdown", "PathSpaceShutdown");
    if (!ctx) {
        return;
    }
    this->clearingInProgress.store(true, std::memory_order_release);
    ctx->invalidateSink();
    // Mark shutting down and wake all waiters so blocking outs can exit promptly
    ctx->shutdown();
    sp_log("PathSpace::shutdown Context shutdown signaled", "PathSpaceShutdown");
    while (this->activeOutCount.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    this->leaf.clear();
    // After clearing paths, purge any remaining wait registrations to prevent dangling waiters
    ctx->clearWaits();
    sp_log("PathSpace::shutdown Cleared paths and waits", "PathSpaceShutdown");
    this->clearingInProgress.store(false, std::memory_order_release);
}

auto PathSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    auto ctx = this->getContext();
    auto pfx = this->getPrefix();
    sp_log("PathSpace::in", "Function Called");
    InsertReturn ret;

    this->leaf.in(path, data, ret);

    if (ret.nbrSpacesInserted > 0 && ctx) {
        for (auto const& req : ret.retargets) {
            if (!req.space)
                continue;
            auto mountPrefix = makeMountPrefix(pfx, req.mountPrefix);
            req.space->adoptContextAndPrefix(ctx, std::move(mountPrefix));
        }
        // Prevent upstream callers from re-applying retargets with an incorrect prefix.
        ret.retargets.clear();
    }

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0) {
        if (ctx && !ctx->hasWaiters()) {
            if (ret.nbrValuesSuppressed > 0 && ret.nbrValuesInserted >= ret.nbrValuesSuppressed) {
                ret.nbrValuesInserted -= ret.nbrValuesSuppressed;
            } else if (ret.nbrValuesSuppressed > 0) {
                ret.nbrValuesInserted = 0;
            }
            return ret;
        }
        if (ctx) {
            if (!pfx.empty()) {
                std::string notePath = pfx;
                notePath.append(path.toStringView().data(), path.toStringView().size());
                sp_log("PathSpace::in notify: " + notePath, "PathSpace");
                ctx->notify(notePath);
            } else {
                auto sv = path.toStringView();
                std::string_view notePath{sv.data(), sv.size()};
                sp_log("PathSpace::in notify: " + std::string(notePath), "PathSpace");
                ctx->notify(notePath);
            }
        }
    }
    if (ret.nbrValuesSuppressed > 0) {
        if (ret.nbrValuesInserted >= ret.nbrValuesSuppressed) {
            ret.nbrValuesInserted -= ret.nbrValuesSuppressed;
        } else {
            ret.nbrValuesInserted = 0;
        }
    }
    return ret;
}

auto PathSpace::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    sp_log("PathSpace::outBlock", "Function Called");

    struct ActiveOutGuard {
        PathSpace* self;
        bool engaged{false};
        ~ActiveOutGuard() {
            if (engaged) {
                self->activeOutCount.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    } guard{this};

    auto contextPtr = this->getContext();
    auto prefixCopy = this->getPrefix();

    if (this->clearingInProgress.load(std::memory_order_acquire)) {
        return Error{Error::Code::Timeout, "PathSpace clearing in progress"};
    }
    guard.engaged = true;
    this->activeOutCount.fetch_add(1, std::memory_order_acq_rel);
    auto retargetNestedIfNeeded = [&]() {
        if (!(options.doPop && inputMetadata.dataCategory == DataCategory::UniquePtr && contextPtr)) {
            return;
        }
        ConcretePathString raw{path.toString()};
        auto canonical = raw.canonicalized();
        if (!canonical) {
            return;
        }
        auto baseComponents = baseComponentsFromCanonical(canonical->getPath());
        if (!baseComponents) {
            return;
        }
        Node* node = this->getRootNode();
        for (auto const& comp : *baseComponents) {
            if (!node) {
                break;
            }
            node = node->getChild(comp);
        }
        if (!node) {
            return;
        }
        auto basePath = joinBaseComponents(*baseComponents);
        this->retargetNestedMounts(node, basePath);
    };

    if (options.isMinimal) {
        auto err = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!err.has_value()) {
            retargetNestedIfNeeded();
        }
        return err;
    }

    // After a successful read or pop, wake other waiters on the path so they re-check their state.
    auto notifyAfterSuccess = [&]() {
        if (!contextPtr) {
            return;
        }
        auto const notePath = prefixCopy.empty() ? path.toString() : prefixCopy + path.toString();
        sp_log("out(success) notify: " + notePath, "PathSpace");
        contextPtr->notify(notePath);
    };

    // First try entirely outside the wait machinery to minimize lock time.
    {
        auto error = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!error.has_value()) {
            retargetNestedIfNeeded();
            notifyAfterSuccess();
            return error;
        }
        if (options.doBlock == false)
            return error;
    }

    // Clamp blocking wait duration using PATHSPACE_TEST_TIMEOUT_MS (milliseconds) or PATHSPACE_TEST_TIMEOUT (seconds)
    auto maxWait = options.timeout;
    if (maxWait <= std::chrono::milliseconds{0}) {
        // Guard against accidental zero/negative timeouts to avoid spurious failures in blocking reads.
        maxWait = DEFAULT_TIMEOUT;
    }
    if (const char* envms = std::getenv("PATHSPACE_TEST_TIMEOUT_MS")) {
        char* endptr = nullptr;
        long ms = std::strtol(envms, &endptr, 10);
        if (endptr != envms && ms > 0) {
            auto clamp = std::chrono::milliseconds(ms);
            if (clamp < maxWait) {
                maxWait = clamp;
            }
        }
    } else if (const char* env = std::getenv("PATHSPACE_TEST_TIMEOUT")) {
        char* endptr = nullptr;
        long secs = std::strtol(env, &endptr, 10);
        if (endptr != env && secs > 0) {
            auto clamp = std::chrono::milliseconds(secs * 1000);
            if (clamp < maxWait) {
                maxWait = clamp;
            }
        }
    }
    auto const deadline = std::chrono::system_clock::now() + maxWait;
    sp_log("PathSpace::out waiting on: " + path.toString(), "PathSpace");
    sp_log(std::string("PathSpace::out block wait timeout(ms)=") + std::to_string(maxWait.count()), "PathSpace");

    // Register as a waiter *before* each attempt. A WaitMap guard captures the notification
    // version when it is created, so a notify that lands between the attempt and the wait makes
    // the wait return immediately instead of being lost. The wait therefore blocks until a
    // notification or the deadline; the periodic re-check below is only a safety net for writers
    // that mutate the space without notifying.
    while (true) {
        if (contextPtr && contextPtr->isShuttingDown()) {
            return Error{Error::Code::Timeout, "Shutting down while waiting for data at path: " + path.toString()};
        }
        if (this->clearingInProgress.load(std::memory_order_acquire)) {
            return Error{Error::Code::Timeout, "PathSpace clearing in progress"};
        }
        auto const now = std::chrono::system_clock::now();
        if (now >= deadline) {
            return Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + path.toString()};
        }

        // The binding can change while blocked (re-home), so look it up for every attempt.
        contextPtr = this->getContext();
        prefixCopy = this->getPrefix();
        if (!contextPtr) {
            return Error{Error::Code::Timeout, "No context to wait on for path: " + path.toString()};
        }
        auto waitGuard = contextPtr->wait(prefixCopy.empty() ? path.toString() : prefixCopy + path.toString());
        auto attempt   = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!attempt.has_value()) {
            retargetNestedIfNeeded();
            notifyAfterSuccess();
            return std::nullopt;
        }
        sp_log(std::string("out(retry) still failing, error=") + attempt->message.value_or("no-message")
                   + " code=" + std::to_string(static_cast<int>(attempt->code)),
               "PathSpace");

        auto const wakeBy = std::min(deadline, now + waitSafetyRecheckInterval());
        waitGuard.wait_until(wakeBy);
    }
}

auto PathSpace::notify(std::string const& notificationPath) -> void {
    auto ctx = this->getContext();
    auto pfx = this->getPrefix();
    sp_log("PathSpace::notify", "Function Called");
    if (!pfx.empty()) {
        std::string notePath = pfx;
        notePath.append(notificationPath.data(), notificationPath.size());
        sp_log("PathSpace::notify forwarding: " + notePath, "PathSpace");
        ctx->notify(notePath);
    } else {
        std::string_view notePath{notificationPath.data(), notificationPath.size()};
        sp_log("PathSpace::notify forwarding: " + std::string(notePath), "PathSpace");
        ctx->notify(notificationPath);
    }
}

auto PathSpace::notifyAbsolute(std::string const& absolutePath) -> void {
    if (auto ctx = this->getContext()) {
        ctx->notify(absolutePath);
    }
}

auto PathSpace::absoluteNotificationPath(std::string relativePath) const -> std::string {
    auto pfx = this->getPrefix();
    if (pfx.empty()) {
        return relativePath;
    }
    return pfx + relativePath;
}

auto PathSpace::spanPackConst(std::span<const std::string> paths,
                              InputMetadata const& metadata,
                              Out const& options,
                              SpanPackConstCallback const& fn) const -> Expected<void> {
    return this->leaf.spanPackConst(paths, metadata, options, fn);
}

auto PathSpace::spanPackMut(std::span<const std::string> paths,
                            InputMetadata const& metadata,
                            Out const& options,
                            SpanPackMutCallback const& fn) const -> Expected<void> {
    auto ctx = this->getContext();
    auto pfx = this->getPrefix();
    if (!options.doBlock || !ctx) {
        return this->leaf.spanPackMut(paths, metadata, options, fn);
    }

    auto deadline = std::chrono::system_clock::now() + options.timeout;
    Out nonBlockingOptions = options;
    nonBlockingOptions.doBlock = false;

    std::size_t waitIndex = 0;
    while (true) {
        // Register before the attempt so a notify between the attempt and the wait is not lost.
        auto pathToWait = paths[waitIndex % paths.size()];
        ++waitIndex;
        if (!pfx.empty()) {
            pathToWait = pfx + pathToWait;
        }
        auto guard = ctx->wait(pathToWait);

        auto res = this->leaf.spanPackMut(paths, metadata, nonBlockingOptions, fn);
        if (res) {
            return res;
        }
        auto err = res.error();
        if (err.code != Error::Code::NoObjectFound && err.code != Error::Code::NoSuchPath) {
            return std::unexpected(err);
        }

        auto const now = std::chrono::system_clock::now();
        if (now >= deadline) {
            return std::unexpected(Error{Error::Code::Timeout, "Span pack take timed out"});
        }

        guard.wait_until(std::min(deadline, now + waitSafetyRecheckInterval()));
    }
}

auto PathSpace::packInsert(std::span<const std::string> paths,
                           InputMetadata const& metadata,
                           std::span<void const* const> values) -> InsertReturn {
    auto ctx = this->getContext();
    auto pfx = this->getPrefix();
    auto ret = this->leaf.packInsert(paths, metadata, values);
    if (!ctx) {
        return ret;
    }

    if (ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0 || ret.nbrSpacesInserted > 0) {
        // Notify waiters for each affected path; apply prefix when this PathSpace is mounted.
        for (auto const& path : paths) {
            if (!pfx.empty()) {
                ctx->notify(pfx + path);
            } else {
                ctx->notify(path);
            }
        }
    }
    return ret;
}

auto PathSpace::packInsertSpans(std::span<const std::string> paths,
                                std::span<SpanInsertSpec const> specs) -> InsertReturn {
    auto ctx = this->getContext();
    auto pfx = this->getPrefix();
    auto ret = this->leaf.packInsertSpans(paths, specs);
    if (!ctx) {
        return ret;
    }
    if (ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0 || ret.nbrSpacesInserted > 0) {
        for (auto const& path : paths) {
            if (!pfx.empty()) {
                ctx->notify(pfx + path);
            } else {
                ctx->notify(path);
            }
        }
    }
    return ret;
}

auto PathSpace::getRootNode() -> Node* {
    return &this->leaf.rootNode();
}

auto PathSpace::listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> {
    ConcretePathStringView pathView{canonicalPath};
    auto componentsExpected = pathView.components();
    if (!componentsExpected) {
        return {};
    }

    auto const& components = componentsExpected.value();
    Node const* current    = &this->leaf.rootNode();

    if (components.empty()) {
        return mergeWithNested(*current, gatherOrderedChildren(*current));
    }

    for (std::size_t idx = 0; idx < components.size(); ++idx) {
        auto parsed           = parse_indexed_component(components[idx]);
        if (parsed.malformed) {
            return {};
        }
        Node const* child     = current->getChild(parsed.base);
        if (!child) {
            return {};
        }

        bool const isFinal = (idx + 1 == components.size());
        if (isFinal) {
            if (parsed.index.has_value()) {
                std::shared_ptr<PathSpaceBase> nested;
                {
                    std::lock_guard<std::mutex> guard(child->payloadMutex);
                    if (child->data) {
                        nested = child->data->borrowNestedShared(*parsed.index);
                    }
                }
                if (!nested) {
                    return {};
                }
                return nested->listChildrenCanonical("/");
            }
            return mergeWithNested(*child, gatherOrderedChildren(*child));
        }

        std::shared_ptr<PathSpaceBase> nestedSpace;
        {
            std::lock_guard<std::mutex> guard(child->payloadMutex);
            if (child->data) {
                nestedSpace = child->data->borrowNestedShared(parsed.index.value_or(0));
            }
        }

        if (parsed.index.has_value() && !nestedSpace) {
            return {};
        }

        if (nestedSpace && parsed.index.has_value()) {
            auto remaining = buildRemainingPath(components, idx + 1);
            auto names = nestedSpace->read<Children>(remaining);
            if (names) return names->names;
            return {};
        }

        if (nestedSpace && !parsed.index.has_value()) {
            auto remaining = buildRemainingPath(components, idx + 1);
            auto names = nestedSpace->read<Children>(remaining);
            if (names) return names->names;
            return {};
        }

        current = child;
    }

    return {};
}

void PathSpace::copyFrom(PathSpace const& other, CopyStats* stats) {
    CopyStats local{};
    struct SpaceFrame {
        PathSpace const* srcSpace;
        PathSpace*       dstSpace;
    };
    struct Frame {
        Node const* src;
        Node* dst;
        std::string path;
    };

    std::vector<SpaceFrame> spaceStack;
    spaceStack.push_back(SpaceFrame{&other, this});

    while (!spaceStack.empty()) {
        SpaceFrame spaceFrame = spaceStack.back();
        spaceStack.pop_back();
        PathSpace const* srcSpace = spaceFrame.srcSpace;
        PathSpace* dstSpace = spaceFrame.dstSpace;

        dstSpace->leaf.clear();
        Node& dstRoot       = dstSpace->leaf.rootNode();
        Node const& srcRoot = srcSpace->leaf.rootNode();

        std::vector<Frame> stack;
        stack.push_back(Frame{&srcRoot, &dstRoot, "/"});

        while (!stack.empty()) {
            Frame frame = std::move(stack.back());
            stack.pop_back();

            Node const& src = *frame.src;
            Node&       dst = *frame.dst;
            std::string const& currentPath = frame.path;

            local.nodesVisited += 1;

            std::vector<std::shared_ptr<PathSpaceBase>> nestedSpaces;
            bool                                        snapshotRestored = false;
            {
                std::lock_guard<std::mutex> guard(src.payloadMutex);
                if (src.data) {
                    auto snapshot = src.data->serializeSnapshot();
                    if (snapshot) {
                        auto restored = NodeData::deserializeSnapshot(*snapshot);
                        if (restored) {
                            dst.data = std::make_unique<NodeData>(std::move(*restored));
                            local.payloadsCopied += 1;
                            local.valuesCopied += dst.data->valueCount();
                            snapshotRestored = true;
                        } else {
                            local.payloadsSkipped += 1;
                        }
                    } else {
                        local.payloadsSkipped += 1;
                    }

                    auto nestedCount = src.data->nestedCount();
                    nestedSpaces.reserve(nestedCount);
                    for (std::size_t i = 0; i < nestedCount; ++i) {
                        nestedSpaces.push_back(src.data->borrowNestedShared(i));
                    }
                } else if (src.podPayload) {
                    auto payload = src.podPayload;
                    auto const& meta = payload->podMetadata();
                    auto elemSize    = payload->elementSize();
                    NodeData tmp;
                    bool ok = true;
                    auto spanErr = payload->withSpanRaw([&](void const* data, std::size_t count) {
                        auto* base = static_cast<std::byte const*>(data);
                        for (std::size_t i = 0; i < count; ++i) {
                            InputData in{base + i * elemSize, meta};
                            if (auto e = tmp.serialize(in)) {
                                ok = false;
                                return;
                            }
                        }
                    });
                    if (spanErr.has_value() || !ok) {
                        local.payloadsSkipped += 1;
                    } else {
                        dst.data = std::make_unique<NodeData>(std::move(tmp));
                        local.payloadsCopied += 1;
                        local.valuesCopied += dst.data->valueCount();
                        snapshotRestored = true;
                    }
                }
            }

            if (!snapshotRestored && !nestedSpaces.empty() && !dst.data) {
                dst.data = std::make_unique<NodeData>();
            }

            for (std::size_t idx = 0; idx < nestedSpaces.size(); ++idx) {
                auto const& nestedShared = nestedSpaces[idx];
                if (!nestedShared) {
                    local.nestedSpacesSkipped += 1;
                    continue;
                }
                if (auto nestedPathSpace = dynamic_cast<PathSpace const*>(nestedShared.get())) {
                    auto indexedPath = append_index_suffix(currentPath, idx);
                    auto mountPrefix = makeMountPrefix(dstSpace->prefix, indexedPath);
                    auto* nestedPool = nestedPathSpace->pool ? nestedPathSpace->pool : &TaskPool::Instance();
                    auto nestedPtr = std::make_unique<PathSpace>(nestedPool);
                    nestedPtr->adoptContextAndPrefix(dstSpace->context, mountPrefix);
                    PathSpace* nestedRaw = nestedPtr.get();
                    if (!dst.data) {
                        dst.data = std::make_unique<NodeData>();
                    }
                    auto expectedSlots = countCategory(dst.data->typeSummary(), DataCategory::UniquePtr);
                    auto attached      = dst.data->nestedCount();
                    auto attachNested = [&](std::unique_ptr<PathSpace> space) -> std::optional<Error> {
                        std::unique_ptr<PathSpaceBase> basePtr = std::move(space);
                        if (snapshotRestored) {
                            if (idx < attached && dst.data->nestedAt(idx) == nullptr) {
                                auto placed = dst.data->emplaceNestedAt(idx, basePtr);
                                if (placed.has_value()) {
                                    return placed;
                                }
                                return std::nullopt;
                            } else if (expectedSlots > attached) {
                                auto placed = dst.data->emplaceNestedAt(attached, basePtr);
                                if (placed.has_value()) {
                                    return placed;
                                }
                                return std::nullopt;
                            }
                        }
                        InputData nestedInput{std::move(basePtr)};
                        return dst.data->serialize(nestedInput);
                    };

                    if (auto err = attachNested(std::move(nestedPtr)); err.has_value()) {
                        local.nestedSpacesSkipped += 1;
                    } else {
                        local.nestedSpacesCopied += 1;
                        spaceStack.push_back(SpaceFrame{nestedPathSpace, nestedRaw});
                    }
                } else {
                    local.nestedSpacesSkipped += 1;
                }
            }

            std::vector<std::pair<std::string, Node const*>> childEntries;
            src.children.for_each([&](auto const& kv) {
                childEntries.emplace_back(kv.first, kv.second.get());
                dst.getOrCreateChild(kv.first);
            });

            for (auto const& entry : childEntries) {
                auto const& name     = entry.first;
                auto const* srcChild = entry.second;
                Node*       dstChild = dst.getChild(name);
                if (!dstChild || !srcChild) {
                    continue;
                }
                auto childPath = appendComponent(currentPath, name);
                stack.push_back(Frame{srcChild, dstChild, std::move(childPath)});
            }
        }
    }

    if (stats) {
        *stats = local;
    }
}

void PathSpace::copyNodeRecursive(Node const& src,
                                  Node& dst,
                                  std::shared_ptr<PathSpaceContext> const& context,
                                  std::string const& basePrefix,
                                  std::string const& currentPath,
                                  CopyStats& stats) {
    (void)src;
    (void)dst;
    (void)context;
    (void)basePrefix;
    (void)currentPath;
    (void)stats;
    std::abort();
}

} // namespace SP
