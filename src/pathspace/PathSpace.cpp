#include "PathSpace.hpp"
#include "task/TaskPool.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"
#include "path/ConcretePath.hpp"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace SP {

namespace {

auto gatherOrderedChildren(Node const& node) -> std::vector<std::string> {
    std::vector<std::string> names;
    node.children.for_each([&](auto const& kv) { names.emplace_back(kv.first); });
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

auto mergeWithNested(Node const& node, std::vector<std::string> names) -> std::vector<std::string> {
    PathSpaceBase const* nestedSpace = nullptr;
    {
        std::lock_guard<std::mutex> guard(node.payloadMutex);
        if (node.nested) {
            nestedSpace = node.nested.get();
        }
    }
    if (nestedSpace) {
        auto nestedNames = nestedSpace->listChildren();
        names.insert(names.end(), nestedNames.begin(), nestedNames.end());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }
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

} // namespace

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
        if (child->nested) {
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
    this->context_ = std::make_shared<PathSpaceContext>(exec);
    this->setExecutor(exec);
};

PathSpace::PathSpace(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    sp_log("PathSpace::PathSpace(context)", "Function Called");
    this->context_ = std::move(context);

        this->prefix = std::move(prefix);
    // Ensure we have an executor via context
    if (this->context_ && this->context_->executor()) {
        this->setExecutor(this->context_->executor());
    }
}
void PathSpace::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    sp_log("PathSpace::adoptContextAndPrefix", "Function Called");
    this->context_ = std::move(context);
    this->prefix = std::move(prefix);
    if (this->context_ && this->context_->executor()) {
        this->setExecutor(this->context_->executor());
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
    this->shutdown();
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
    // Wake any waiters before clearing to avoid dangling waits
    this->context_->notifyAll();
    this->leaf.clear();
    this->context_->clearWaits();
}

auto PathSpace::shutdown() -> void {
    sp_log("PathSpace::shutdown", "Function Called");
    sp_log("PathSpace::shutdown Starting shutdown", "PathSpaceShutdown");
    this->context_->invalidateSink();
    // Mark shutting down and wake all waiters so blocking outs can exit promptly
    this->context_->shutdown();
    sp_log("PathSpace::shutdown Context shutdown signaled", "PathSpaceShutdown");
    this->leaf.clear();
    // After clearing paths, purge any remaining wait registrations to prevent dangling waiters
    this->context_->clearWaits();
    sp_log("PathSpace::shutdown Cleared paths and waits", "PathSpaceShutdown");
}

auto PathSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    sp_log("PathSpace::in", "Function Called");
    InsertReturn ret;

    PathSpace* space = nullptr;
    if (data.metadata.dataCategory == DataCategory::UniquePtr) {
        if (data.metadata.typeInfo == &typeid(std::unique_ptr<PathSpace>)) {
            space           = reinterpret_cast<std::unique_ptr<PathSpace>*>(data.obj)->get();
        }
    }

    this->leaf.in(path, data, ret);

    if (space && ret.nbrSpacesInserted > 0 && data.metadata.dataCategory == DataCategory::UniquePtr) {
        std::string mountPrefix = this->prefix.empty() ? path.toString() : this->prefix + path.toString();
        space->adoptContextAndPrefix(this->context_, std::move(mountPrefix));
    }

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0) {
        if (!this->prefix.empty()) {
            std::string notePath = this->prefix;
            notePath.append(path.toStringView().data(), path.toStringView().size());
            sp_log("PathSpace::in notify: " + notePath, "PathSpace");
            this->context_->notify(notePath);
        } else {
            auto sv = path.toStringView();
            std::string_view notePath{sv.data(), sv.size()};
            sp_log("PathSpace::in notify: " + std::string(notePath), "PathSpace");
            this->context_->notify(notePath);
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
    if (options.isMinimal)
        return this->leaf.out(path, inputMetadata, obj, options.doPop);

    std::optional<Error> error;
    // First try entirely outside the loop to minimize lock time
    {
        error = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!error.has_value()) {
            // Successful read or pop; notify other waiters to re-check state
            if (!this->prefix.empty())
                this->context_->notify(this->prefix + std::string(path.toStringView()));
            else
                this->context_->notify(path.toStringView());
            return error;
        }
        if (options.doBlock == false)
            return error;
    }

    // Clamp blocking wait duration using PATHSPACE_TEST_TIMEOUT_MS (milliseconds) or PATHSPACE_TEST_TIMEOUT (seconds)
    auto maxWait = options.timeout;
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
    std::string waitPath = this->prefix.empty() ? path.toString() : this->prefix + path.toString();
    sp_log("PathSpace::out waiting on: " + waitPath, "PathSpace");
    sp_log(std::string("PathSpace::out block wait timeout(ms)=") + std::to_string(maxWait.count()), "PathSpace");

    // Second immediate re-check to close race between initial read and wait registration
    {
        auto secondTry = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!secondTry.has_value()) {
            // Successful read or pop; notify other waiters to re-check state
            if (!this->prefix.empty()) {
                std::string notePath = this->prefix;
                auto sv = path.toStringView();
                notePath.append(sv.data(), sv.size());
                sp_log("out(success pre-wait) notify: " + notePath, "PathSpace");
                this->context_->notify(notePath);
            } else {
                auto sv = path.toStringView();
                std::string_view notePath{sv.data(), sv.size()};
                sp_log("out(success pre-wait) notify: " + std::string(notePath), "PathSpace");
                this->context_->notify(notePath);
            }
            return std::nullopt;
        }
    }


    while (true) {
        // Check shutdown and deadline first
        if (this->context_ && this->context_->isShuttingDown()) {
            return Error{Error::Code::Timeout, "Shutting down while waiting for data at path: " + path.toString()};
        }
        auto now = std::chrono::system_clock::now();
        if (now >= deadline)
            return Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + path.toString()};

        // Quick re-check before waiting to avoid sleeping unnecessarily
        {
            auto quick = this->leaf.out(path, inputMetadata, obj, options.doPop);
            if (!quick.has_value()) {
                // Successful read or pop; notify other waiters to re-check state
                if (!this->prefix.empty()) {
                    std::string notePath = this->prefix;
                    auto sv = path.toStringView();
                    notePath.append(sv.data(), sv.size());
                    sp_log("out(success pre-wait in-loop) notify: " + notePath, "PathSpace");
                    this->context_->notify(notePath);
                } else {
                    auto sv = path.toStringView();
                    std::string_view notePath{sv.data(), sv.size()};
                    sp_log("out(success pre-wait in-loop) notify: " + std::string(notePath), "PathSpace");
                    this->context_->notify(notePath);
                }
                return std::nullopt;
            }
        }

        // Wait in short slices; never call leaf.out while holding the WatchRegistry lock
        {
            auto guard  = this->context_->wait(waitPath);
            auto now2   = std::chrono::system_clock::now();
            auto remain = deadline - now2;
            if (remain <= std::chrono::milliseconds(0)) {
                return Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + path.toString()};
            }
            // Start with a small slice and back off to reduce busy-waiting under contention.
            static thread_local std::chrono::milliseconds waitSlice{1};
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
            static thread_local std::chrono::milliseconds waitSlice{1};
            waitSlice = std::chrono::milliseconds(1);
            // Successful read or pop; notify other waiters to re-check state
            if (!this->prefix.empty()) {
                std::string notePath = this->prefix;
                auto sv = path.toStringView();
                notePath.append(sv.data(), sv.size());
                sp_log("out(success in-loop) notify: " + notePath, "PathSpace");
                this->context_->notify(notePath);
            } else {
                auto sv = path.toStringView();
                std::string_view notePath{sv.data(), sv.size()};
                sp_log("out(success in-loop) notify: " + std::string(notePath), "PathSpace");
                this->context_->notify(notePath);
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
        this->context_->notify(notePath);
    } else {
        std::string_view notePath{notificationPath.data(), notificationPath.size()};
        sp_log("PathSpace::notify forwarding: " + std::string(notePath), "PathSpace");
        this->context_->notify(notificationPath);
    }
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
        auto const& component = components[idx];
        Node const* child     = current->getChild(component);
        if (!child) {
            return {};
        }

        bool const isFinal = (idx + 1 == components.size());
        if (isFinal) {
            return mergeWithNested(*child, gatherOrderedChildren(*child));
        }

        PathSpaceBase const* nestedSpace = nullptr;
        {
            std::lock_guard<std::mutex> guard(child->payloadMutex);
            if (child->nested) {
                nestedSpace = child->nested.get();
            }
        }

        if (nestedSpace) {
            auto remaining = buildRemainingPath(components, idx + 1);
            ConcretePathString remainingPath{remaining};
            ConcretePathStringView remainingView{remainingPath.getPath()};
            return nestedSpace->listChildren(remainingView);
        }

        current = child;
    }

    return {};
}

} // namespace SP
