#pragma once
#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "path/Iterator.hpp"
#include "path/ConcretePath.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace SP {

/**
 * PathAlias — a lightweight alias/mount layer that forwards to an upstream space
 * by rewriting the path with a target prefix.
 *
 * Characteristics:
 * - Mount-agnostic: The alias itself can be inserted anywhere in a parent PathSpace.
 * - Transparent forwarding: in()/out()/notify() are forwarded to the upstream space
 *   with the alias path appended to the current target prefix.
 * - Atomic retargeting: setTargetPrefix() changes the forwarding prefix atomically
 *   and can trigger notifications to wake alias waiters.
 *
 * Notes:
 * - This layer does not attempt to reflect upstream notifications automatically.
 *   For that, callers should either notify through the alias, or a higher-level
 *   link/alias manager should bridge notifications.
 * - On retargeting, a notification is emitted on the alias mount prefix (if known)
 *   to encourage waiters to re-check.
 */
class PathAlias final : public PathSpaceBase {
public:
    PathAlias(std::shared_ptr<PathSpaceBase> upstream, std::string targetPrefix)
        : upstream(std::move(upstream)) {
        setTargetPrefix(std::move(targetPrefix));
    }

    // Atomically change the target prefix to which this alias forwards.
    // Emits a notification on the alias mount path if a context is present.
    void setTargetPrefix(std::string newPrefix) {
        // Normalize prefix to start with '/', and without trailing '/' (unless it's just "/")
        if (newPrefix.empty() || newPrefix[0] != '/')
            newPrefix.insert(newPrefix.begin(), '/');
        // Remove trailing slashes (keep root "/")
        while (newPrefix.size() > 1 && newPrefix.back() == '/')
            newPrefix.pop_back();

        {
            std::lock_guard<std::mutex> lg(this->aliasMutex);
            this->targetPrefixValue = std::move(newPrefix);
        }

        // Wake waiters on the alias mount to re-check after retarget
        if (auto ctx = this->getContext(); ctx) {
            std::string aliasRoot;
            {
            std::lock_guard<std::mutex> lg(this->aliasMutex);
            aliasRoot = this->mountPrefixValue;
        }
            if (!aliasRoot.empty()) {
                ctx->notify(aliasRoot);
            } else {
                // Fallback broad notification if alias mount is unknown
                ctx->notifyAll();
            }
        }
    }

    // Current target prefix (thread-safe snapshot).
    std::string targetPrefix() const {
        std::lock_guard<std::mutex> lg(this->aliasMutex);
        return this->targetPrefixValue;
    }

    // Forward insert: map alias path under the current target prefix and forward upstream.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        if (!this->upstream) {
            return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"}}};
        }
        auto mappedStr = this->mapPath(path);
        Iterator mapped{mappedStr};
        return this->upstream->in(mapped, data);
    }

    // Forward read/take: map alias path under the current target prefix and forward upstream.
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override {
        if (!this->upstream) {
            return Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"};
        }
        auto mappedStr = this->mapPath(path);
        Iterator mapped{mappedStr};
        return this->upstream->out(mapped, inputMetadata, options, obj);
    }

    // Forward notify: map alias path under the current target prefix and notify upstream.
    auto notify(std::string const& notificationPath) -> void override {
        if (!this->upstream)
            return;
        auto mapped = this->mapPathRaw(notificationPath);
        this->upstream->notify(mapped);
    }

    auto spanPackConst(std::span<const std::string> paths,
                       InputMetadata const& metadata,
                       Out const& options,
                       SpanPackConstCallback const& fn) const -> Expected<void> override {
        if (!this->upstream) {
            return std::unexpected(Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"});
        }

        std::vector<std::string> mapped;
        mapped.reserve(paths.size());
        for (auto const& path : paths) {
            mapped.emplace_back(this->mapPathRaw(path));
        }

        return this->upstream->spanPackConst(std::span<const std::string>(mapped.data(), mapped.size()), metadata, options, fn);
    }

    auto spanPackMut(std::span<const std::string> paths,
                     InputMetadata const& metadata,
                     Out const& options,
                     SpanPackMutCallback const& fn) const -> Expected<void> override {
        if (!this->upstream) {
            return std::unexpected(Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"});
        }

        std::vector<std::string> mapped;
        mapped.reserve(paths.size());
        for (auto const& path : paths) {
            mapped.emplace_back(this->mapPathRaw(path));
        }

        return this->upstream->spanPackMut(std::span<const std::string>(mapped.data(), mapped.size()), metadata, options, fn);
    }

    auto packInsert(std::span<const std::string> paths,
                    InputMetadata const& metadata,
                    std::span<void const* const> values) -> InsertReturn override {
        if (!this->upstream) {
            return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"}}};
        }

        std::vector<std::string> mapped;
        mapped.reserve(paths.size());
        for (auto const& path : paths) {
            mapped.emplace_back(this->mapPathRaw(path));
        }

        return this->upstream->packInsert(std::span<const std::string>(mapped.data(), mapped.size()), metadata, values);
    }

    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override {
        if (!this->upstream) {
            return std::unexpected(Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"});
        }

        VisitOptions mapped = options;
        mapped.root        = mapVisitRoot(options.root);

        auto aliasVisitor = [&](PathEntry const& upstreamEntry, ValueHandle& handle) -> VisitControl {
            PathEntry remapped = upstreamEntry;
            remapped.path      = stripTargetPrefix(upstreamEntry.path);
            return visitor(remapped, handle);
        };

        return this->upstream->visit(aliasVisitor, mapped);
    }

    // No special shutdown behavior; upstream should be managed externally.
    auto shutdown() -> void override { /* no-op */ }

    // Capture shared context and remember alias mount prefix for targeted notifications.
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
        std::lock_guard<std::mutex> lg(this->aliasMutex);
        this->mountPrefixValue = std::move(prefix);
    }

protected:
    auto getRootNode() -> Node* override {
        if (!this->upstream)
            return nullptr;
        return this->upstream->getRootNode();
    }

private:
    // Join targetPrefix and a tail path, ensuring exactly one slash at the boundary.
    static std::string joinPaths(std::string const& prefix, std::string const& tail) {
        if (prefix.empty())
            return tail;
        if (tail.empty())
            return prefix;

        bool prefixEndsWithSlash = !prefix.empty() && prefix.back() == '/';
        bool tailStartsWithSlash = !tail.empty() && tail.front() == '/';

        if (prefixEndsWithSlash && tailStartsWithSlash) {
            return prefix + tail.substr(1);
        } else if (!prefixEndsWithSlash && !tailStartsWithSlash) {
            return prefix + "/" + tail;
        } else {
            return prefix + tail;
        }
    }

    // Thread-safe mapping of Iterator tail (current->end) to a new full-path string.
    std::string mapPath(Iterator const& path) const {
        std::string prefixCopy;
        {
            std::lock_guard<std::mutex> lg(this->aliasMutex);
            prefixCopy = this->targetPrefixValue;
        }
        // Use currentToEnd() so nested sub-iterators map correctly (e.g., after alias component)
        return joinPaths(prefixCopy, std::string(path.currentToEnd()));
    }

    // Thread-safe mapping of raw path string (used for notify).
    std::string mapPathRaw(std::string const& path) const {
        std::string prefixCopy;
        {
            std::lock_guard<std::mutex> lg(this->aliasMutex);
            prefixCopy = this->targetPrefixValue;
        }
        return joinPaths(prefixCopy, path);
    }

protected:
    auto listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> override {
        if (!this->upstream) return {};
        auto mapped = this->mapPathRaw(std::string(canonicalPath));
        auto kids   = this->upstream->read<Children>(ConcretePathStringView{mapped});
        if (!kids) {
            return {};
        }
        return kids->names;
    }

    std::string mapVisitRoot(std::string const& path) const {
        if (path.empty() || path == "/") {
            std::lock_guard<std::mutex> lg(this->aliasMutex);
            return this->targetPrefixValue.empty() ? std::string{"/"} : this->targetPrefixValue;
        }
        return this->mapPathRaw(path);
    }

    std::string stripTargetPrefix(std::string const& upstreamPath) const {
        std::string prefixCopy;
        {
            std::lock_guard<std::mutex> lg(this->aliasMutex);
            prefixCopy = this->targetPrefixValue;
        }
        if (prefixCopy.empty() || prefixCopy == "/") {
            return upstreamPath;
        }
        if (upstreamPath == prefixCopy) {
            return "/";
        }
        if (upstreamPath.size() > prefixCopy.size() && upstreamPath.rfind(prefixCopy, 0) == 0) {
            auto remainder = upstreamPath.substr(prefixCopy.size());
            if (remainder.empty()) {
                return "/";
            }
            if (remainder.front() != '/') {
                return std::string{"/"} + remainder;
            }
            return remainder;
        }
        return upstreamPath;
    }

private:
    std::shared_ptr<PathSpaceBase> upstream;
    mutable std::mutex             aliasMutex;
    std::string                    targetPrefixValue; // e.g., "/system/input/mouse/0"
    std::string                    mountPrefixValue;  // alias location within parent (for notifications)
};

} // namespace SP
