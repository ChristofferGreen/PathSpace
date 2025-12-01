#pragma once
#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "path/Iterator.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

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
        : upstream_(std::move(upstream)) {
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
            std::lock_guard<std::mutex> lg(mutex_);
            targetPrefix_ = std::move(newPrefix);
        }

        // Wake waiters on the alias mount to re-check after retarget
        if (auto ctx = this->getContext(); ctx) {
            std::string aliasRoot;
            {
                std::lock_guard<std::mutex> lg(mutex_);
                aliasRoot = mountPrefix_;
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
        std::lock_guard<std::mutex> lg(mutex_);
        return targetPrefix_;
    }

    // Forward insert: map alias path under the current target prefix and forward upstream.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        if (!upstream_) {
            return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"}}};
        }
        auto mappedStr = mapPath_(path);
        Iterator mapped{mappedStr};
        return upstream_->in(mapped, data);
    }

    // Forward read/take: map alias path under the current target prefix and forward upstream.
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override {
        if (!upstream_) {
            return Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"};
        }
        auto mappedStr = mapPath_(path);
        Iterator mapped{mappedStr};
        return upstream_->out(mapped, inputMetadata, options, obj);
    }

    // Forward notify: map alias path under the current target prefix and notify upstream.
    auto notify(std::string const& notificationPath) -> void override {
        if (!upstream_)
            return;
        auto mapped = mapPathRaw_(notificationPath);
        upstream_->notify(mapped);
    }

    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override {
        if (!upstream_) {
            return std::unexpected(Error{Error::Code::InvalidPermissions, "PathAlias upstream not set"});
        }

        VisitOptions mapped = options;
        mapped.root        = mapVisitRoot_(options.root);

        auto aliasVisitor = [&](PathEntry const& upstreamEntry, ValueHandle& handle) -> VisitControl {
            PathEntry remapped = upstreamEntry;
            remapped.path      = stripTargetPrefix_(upstreamEntry.path);
            return visitor(remapped, handle);
        };

        return upstream_->visit(aliasVisitor, mapped);
    }

    // No special shutdown behavior; upstream should be managed externally.
    auto shutdown() -> void override { /* no-op */ }

    // Capture shared context and remember alias mount prefix for targeted notifications.
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
        std::lock_guard<std::mutex> lg(mutex_);
        mountPrefix_ = std::move(prefix);
    }

protected:
    auto getRootNode() -> Node* override {
        if (!upstream_)
            return nullptr;
        return upstream_->getRootNode();
    }

private:
    // Join targetPrefix_ and a tail path, ensuring exactly one slash at the boundary.
    static std::string joinPaths_(std::string const& prefix, std::string const& tail) {
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
    std::string mapPath_(Iterator const& path) const {
        std::string prefixCopy;
        {
            std::lock_guard<std::mutex> lg(mutex_);
            prefixCopy = targetPrefix_;
        }
        // Use currentToEnd() so nested sub-iterators map correctly (e.g., after alias component)
        return joinPaths_(prefixCopy, std::string(path.currentToEnd()));
    }

    // Thread-safe mapping of raw path string (used for notify).
    std::string mapPathRaw_(std::string const& path) const {
        std::string prefixCopy;
        {
            std::lock_guard<std::mutex> lg(mutex_);
            prefixCopy = targetPrefix_;
        }
        return joinPaths_(prefixCopy, path);
    }

    std::string mapVisitRoot_(std::string const& path) const {
        if (path.empty() || path == "/") {
            std::lock_guard<std::mutex> lg(mutex_);
            return targetPrefix_.empty() ? std::string{"/"} : targetPrefix_;
        }
        return mapPathRaw_(path);
    }

    std::string stripTargetPrefix_(std::string const& upstreamPath) const {
        std::string prefixCopy;
        {
            std::lock_guard<std::mutex> lg(mutex_);
            prefixCopy = targetPrefix_;
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
    std::shared_ptr<PathSpaceBase> upstream_;
    mutable std::mutex             mutex_;
    std::string                    targetPrefix_; // e.g., "/system/input/mouse/0"
    std::string                    mountPrefix_;  // alias location within parent (for notifications)
};

} // namespace SP
