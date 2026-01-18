#include "PathSpace.hpp"
#include "task/TaskPool.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"
#include "path/ConcretePath.hpp"
#include "path/utils.hpp"
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

struct SubtreeEndpoints {
    Node* parent = nullptr;
    std::string leaf;
};

auto make_error(Error::Code code, std::string message) -> Error {
    return Error{code, std::move(message)};
}

auto split_components(std::string const& canonicalPath)
    -> Expected<std::vector<std::string>> {
    ConcretePathStringView view{canonicalPath};
    auto components = view.components();
    if (!components) {
        return std::unexpected(make_error(Error::Code::InvalidPath,
                                          "failed to parse path components"));
    }
    if (components->empty()) {
        return std::unexpected(make_error(Error::Code::InvalidPath,
                                          "root path cannot be relocated"));
    }
    return *components;
}

auto locate_parent(Node& root,
                   std::vector<std::string> const& components,
                   bool allow_children_creation)
    -> Expected<SubtreeEndpoints> {
    if (components.size() < 1) {
        return std::unexpected(make_error(Error::Code::InvalidPath,
                                          "path requires at least one component"));
    }
    Node* current = &root;
    for (std::size_t idx = 0; idx + 1 < components.size(); ++idx) {
        auto const& component = components[idx];
        Node* child = current->getChild(component);
        if (!child) {
            if (allow_children_creation && component == "children") {
                child = &current->getOrCreateChild(component);
            } else {
                return std::unexpected(make_error(Error::Code::NoSuchPath,
                                                  "missing component: " + component));
            }
        }
        std::lock_guard<std::mutex> guard(child->payloadMutex);
        if (child->data && child->data->hasNestedSpaces()) {
            return std::unexpected(make_error(Error::Code::NotSupported,
                                              "relocation across nested spaces is not supported"));
        }
        current = child;
    }

    return SubtreeEndpoints{.parent = current, .leaf = components.back()};
}

auto canonicalize(std::string_view path) -> Expected<std::string> {
    ConcretePathString raw{std::string(path)};
    auto canonical = raw.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    return canonical->getPath();
}

} // namespace

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

auto accumulate(PathSpace::CopyStats& into, PathSpace::CopyStats const& from) -> void {
    into.nodesVisited += from.nodesVisited;
    into.payloadsCopied += from.payloadsCopied;
    into.payloadsSkipped += from.payloadsSkipped;
    into.valuesCopied += from.valuesCopied;
    into.nestedSpacesCopied += from.nestedSpacesCopied;
    into.nestedSpacesSkipped += from.nestedSpacesSkipped;
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

    for (auto const& target : nestedTargets) {
        auto mountPrefix = makeMountPrefix(this->prefix, append_index_suffix(basePath, target.second));
        target.first->adoptContextAndPrefix(this->sharedContext(), mountPrefix);
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
void PathSpace::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    sp_log("PathSpace::adoptContextAndPrefix", "Function Called");
    auto previousPrefix = this->prefix;
    this->context = std::move(context);
    this->prefix = std::move(prefix);
    if (this->context && this->context->executor()) {
        this->setExecutor(this->context->executor());
    }
    // Retarget tasks and nested spaces to the newly adopted context/executor.
    if (this->context) {
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
                                                  this->prefix);
                        auto nestedCount = node->data->nestedCount();
                        for (std::size_t i = 0; i < nestedCount; ++i) {
                            if (auto* nested = node->data->nestedAt(i)) {
                                auto mountPrefix = makeMountPrefix(this->prefix, append_index_suffix(path, i));
                                nested->adoptContextAndPrefix(this->context, mountPrefix);
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
    sp_log("PathSpace::~PathSpace", "Function Called");
    if (this->context) {
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
    sp_log("PathSpace::clear", "Function Called");
    this->clearingInProgress.store(true, std::memory_order_release);

    if (this->context) {
        // Wake any waiters before clearing to avoid dangling waits
        this->context->notifyAll();
    }

    while (this->activeOutCount.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!this->context) {
        this->leaf.clear();
        this->clearingInProgress.store(false, std::memory_order_release);
        return;
    }

    this->leaf.clear();
    this->context->clearWaits();
    this->clearingInProgress.store(false, std::memory_order_release);
}

auto PathSpace::shutdown() -> void {
    sp_log("PathSpace::shutdown", "Function Called");
    sp_log("PathSpace::shutdown Starting shutdown", "PathSpaceShutdown");
    if (!this->context) {
        return;
    }
    this->clearingInProgress.store(true, std::memory_order_release);
    this->context->invalidateSink();
    // Mark shutting down and wake all waiters so blocking outs can exit promptly
    this->context->shutdown();
    sp_log("PathSpace::shutdown Context shutdown signaled", "PathSpaceShutdown");
    while (this->activeOutCount.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    this->leaf.clear();
    // After clearing paths, purge any remaining wait registrations to prevent dangling waiters
    this->context->clearWaits();
    sp_log("PathSpace::shutdown Cleared paths and waits", "PathSpaceShutdown");
    this->clearingInProgress.store(false, std::memory_order_release);
}

auto PathSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    sp_log("PathSpace::in", "Function Called");
    InsertReturn ret;

    this->leaf.in(path, data, ret);

    if (ret.nbrSpacesInserted > 0 && this->context) {
        for (auto const& req : ret.retargets) {
            if (!req.space)
                continue;
            auto mountPrefix = makeMountPrefix(this->prefix, req.mountPrefix);
            req.space->adoptContextAndPrefix(this->context, std::move(mountPrefix));
        }
        // Prevent upstream callers from re-applying retargets with an incorrect prefix.
        ret.retargets.clear();
    }

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0) {
        if (this->context && !this->context->hasWaiters()) {
            if (ret.nbrValuesSuppressed > 0 && ret.nbrValuesInserted >= ret.nbrValuesSuppressed) {
                ret.nbrValuesInserted -= ret.nbrValuesSuppressed;
            } else if (ret.nbrValuesSuppressed > 0) {
                ret.nbrValuesInserted = 0;
            }
            return ret;
        }
        if (this->context) {
            if (!this->prefix.empty()) {
                std::string notePath = this->prefix;
                notePath.append(path.toStringView().data(), path.toStringView().size());
                sp_log("PathSpace::in notify: " + notePath, "PathSpace");
                this->context->notify(notePath);
            } else {
                auto sv = path.toStringView();
                std::string_view notePath{sv.data(), sv.size()};
                sp_log("PathSpace::in notify: " + std::string(notePath), "PathSpace");
                this->context->notify(notePath);
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

    auto contextPtr = this->context;
    auto prefixCopy = this->prefix;

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

    std::optional<Error> error;
    // First try entirely outside the loop to minimize lock time
    {
        error = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!error.has_value()) {
            retargetNestedIfNeeded();
            // Successful read or pop; notify other waiters to re-check state
            if (!prefixCopy.empty())
                contextPtr->notify(prefixCopy + std::string(path.toStringView()));
            else
                contextPtr->notify(path.toStringView());
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
    std::string waitPath = prefixCopy.empty() ? path.toString() : prefixCopy + path.toString();
    sp_log("PathSpace::out waiting on: " + waitPath, "PathSpace");
    sp_log(std::string("PathSpace::out block wait timeout(ms)=") + std::to_string(maxWait.count()), "PathSpace");
    // Second immediate re-check to close race between initial read and wait registration
    {
        auto secondTry = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!secondTry.has_value()) {
            // Successful read or pop; notify other waiters to re-check state
            if (!prefixCopy.empty()) {
                std::string notePath = prefixCopy;
                auto sv = path.toStringView();
                notePath.append(sv.data(), sv.size());
                sp_log("out(success pre-wait) notify: " + notePath, "PathSpace");
                contextPtr->notify(notePath);
            } else {
                auto sv = path.toStringView();
                std::string_view notePath{sv.data(), sv.size()};
                sp_log("out(success pre-wait) notify: " + std::string(notePath), "PathSpace");
                contextPtr->notify(notePath);
            }
            return std::nullopt;
        }
    }


    static thread_local std::chrono::milliseconds waitSlice{1};

    while (true) {
        // Check shutdown and deadline first
        if (contextPtr && contextPtr->isShuttingDown()) {
            return Error{Error::Code::Timeout, "Shutting down while waiting for data at path: " + path.toString()};
        }
        if (this->clearingInProgress.load(std::memory_order_acquire)) {
            return Error{Error::Code::Timeout, "PathSpace clearing in progress"};
        }
        auto now = std::chrono::system_clock::now();
        if (now >= deadline)
            return Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + path.toString()};

        // Quick re-check before waiting to avoid sleeping unnecessarily
        {
            auto quick = this->leaf.out(path, inputMetadata, obj, options.doPop);
            if (!quick.has_value()) {
                retargetNestedIfNeeded();
                // Successful read or pop; notify other waiters to re-check state
                if (!prefixCopy.empty()) {
                    std::string notePath = prefixCopy;
                    auto sv = path.toStringView();
                    notePath.append(sv.data(), sv.size());
                    sp_log("out(success pre-wait in-loop) notify: " + notePath, "PathSpace");
                    contextPtr->notify(notePath);
                } else {
                    auto sv = path.toStringView();
                    std::string_view notePath{sv.data(), sv.size()};
                    sp_log("out(success pre-wait in-loop) notify: " + std::string(notePath), "PathSpace");
                    contextPtr->notify(notePath);
                }
                return std::nullopt;
            }
        }

        // Wait in short slices; never call leaf.out while holding the WatchRegistry lock
        {
            auto guard  = contextPtr->wait(waitPath);
            auto now2   = std::chrono::system_clock::now();
            auto remain = deadline - now2;
            if (remain <= std::chrono::milliseconds(0)) {
                return Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + path.toString()};
            }
            // Start with a small slice and back off to reduce busy-waiting under contention.
            auto slice = waitSlice;
            // Cap slice and ensure we never exceed the remaining time.
            auto cap = std::chrono::milliseconds(8);
            if (slice > cap) slice = cap;
            if (remain < slice)
                slice = std::chrono::duration_cast<std::chrono::milliseconds>(remain);
            sp_log(std::string("PathSpace::out wait slice(ms)=") + std::to_string(slice.count()), "PathSpace");
            auto __wait_start = std::chrono::system_clock::now();
            guard.wait_until(__wait_start + slice);
            auto __wait_end = std::chrono::system_clock::now();
            sp_log(std::string("PathSpace::out woke after(ms)=") + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(__wait_end - __wait_start).count()), "PathSpace");
            // Exponential backoff for next slice (reset on success below)
            waitSlice = std::min(cap, waitSlice * 2);
        }
        { static thread_local int spinCount = 0; if ((++spinCount & 7) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

        // After being notified (or slice elapsed), try to read again outside of the registry lock
        auto retry = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!retry.has_value()) {
            retargetNestedIfNeeded();
            waitSlice = std::chrono::milliseconds(1);
            // Successful read or pop; notify other waiters to re-check state
                if (!prefixCopy.empty()) {
                    std::string notePath = prefixCopy;
                    auto sv = path.toStringView();
                    notePath.append(sv.data(), sv.size());
                    sp_log("out(success in-loop) notify: " + notePath, "PathSpace");
                    contextPtr->notify(notePath);
                } else {
                    auto sv = path.toStringView();
                    std::string_view notePath{sv.data(), sv.size()};
                    sp_log("out(success in-loop) notify: " + std::string(notePath), "PathSpace");
                    contextPtr->notify(notePath);
                }
                return std::nullopt;
            } else {
            // Log why retry failed to help diagnose missed-notify or readiness races
            sp_log(std::string("out(retry) still failing, error=")
                       + retry->message.value_or("no-message")
                       + " code=" + std::to_string(static_cast<int>(retry->code)),
                   "PathSpace");
        }
    }
}

auto PathSpace::notify(std::string const& notificationPath) -> void {
    sp_log("PathSpace::notify", "Function Called");
    if (!this->prefix.empty()) {
        std::string notePath = this->prefix;
        notePath.append(notificationPath.data(), notificationPath.size());
        sp_log("PathSpace::notify forwarding: " + notePath, "PathSpace");
        this->context->notify(notePath);
    } else {
        std::string_view notePath{notificationPath.data(), notificationPath.size()};
        sp_log("PathSpace::notify forwarding: " + std::string(notePath), "PathSpace");
        this->context->notify(notificationPath);
    }
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
    if (!options.doBlock || !this->context) {
        return this->leaf.spanPackMut(paths, metadata, options, fn);
    }

    auto deadline = std::chrono::system_clock::now() + options.timeout;
    Out nonBlockingOptions = options;
    nonBlockingOptions.doBlock = false;

    std::size_t waitIndex = 0;
    while (true) {
        auto res = this->leaf.spanPackMut(paths, metadata, nonBlockingOptions, fn);
        if (res) {
            return res;
        }
        auto err = res.error();
        if (err.code != Error::Code::NoObjectFound && err.code != Error::Code::NoSuchPath) {
            return std::unexpected(err);
        }

        auto now = std::chrono::system_clock::now();
        if (now >= deadline) {
            return std::unexpected(Error{Error::Code::Timeout, "Span pack take timed out"});
        }

        auto pathToWait = paths[waitIndex % paths.size()];
        ++waitIndex;
        if (!this->prefix.empty()) {
            pathToWait = this->prefix + pathToWait;
        }

        auto guard = this->context->wait(pathToWait);
        guard.wait_until(deadline);
    }
}

auto PathSpace::packInsert(std::span<const std::string> paths,
                           InputMetadata const& metadata,
                           std::span<void const* const> values) -> InsertReturn {
    auto ret = this->leaf.packInsert(paths, metadata, values);
    if (!this->context) {
        return ret;
    }

    if (ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0 || ret.nbrSpacesInserted > 0) {
        // Notify waiters for each affected path; apply prefix when this PathSpace is mounted.
        for (auto const& path : paths) {
            if (!this->prefix.empty()) {
                this->context->notify(this->prefix + path);
            } else {
                this->context->notify(path);
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
    this->leaf.clear();
    Node& dstRoot       = this->leaf.rootNode();
    Node const& srcRoot = other.leaf.rootNode();
    copyNodeRecursive(srcRoot, dstRoot, this->context, this->prefix, "/", local);
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
    stats.nodesVisited += 1;

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
                    stats.payloadsCopied += 1;
                    stats.valuesCopied += dst.data->valueCount();
                    snapshotRestored = true;
                } else {
                    stats.payloadsSkipped += 1;
                }
            } else {
                stats.payloadsSkipped += 1;
            }

            auto nestedCount = src.data->nestedCount();
            sp_log("copyNodeRecursive path=" + currentPath + " nestedCount=" + std::to_string(nestedCount),
                   "Copy");
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
                stats.payloadsSkipped += 1;
            } else {
                dst.data = std::make_unique<NodeData>(std::move(tmp));
                stats.payloadsCopied += 1;
                stats.valuesCopied += dst.data->valueCount();
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
            stats.nestedSpacesSkipped += 1;
            continue;
        }
        if (auto nestedPathSpace = dynamic_cast<PathSpace const*>(nestedShared.get())) {
            CopyStats nestedStats{};
            auto      nestedCopy = nestedPathSpace->clone(&nestedStats);
            accumulate(stats, nestedStats);
            auto indexedPath = append_index_suffix(currentPath, idx);
            auto mountPrefix = makeMountPrefix(basePrefix, indexedPath);
            auto nestedPtr = std::make_unique<PathSpace>(std::move(nestedCopy));
            nestedPtr->adoptContextAndPrefix(context, mountPrefix);
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
                        // moved into slot on success
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
                stats.nestedSpacesSkipped += 1;
            } else {
                stats.nestedSpacesCopied += 1;
            }
        } else {
            stats.nestedSpacesSkipped += 1;
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
        copyNodeRecursive(*srcChild, *dstChild, context, basePrefix, childPath, stats);
    }
}

} // namespace SP
