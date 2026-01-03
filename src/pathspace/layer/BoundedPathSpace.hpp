#pragma once

#include "PathSpace.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace SP {

// Non-templated bounded wrapper: limits queue length per path.
// - On insert: if count >= maxItems, pop oldest entries of the same type (via out with doPop) until space; if pop fails (type mismatch), the insert is dropped.
// - On out/take (doPop): the count is decremented when a value is popped.
// - Reads without pop leave the count unchanged.
class BoundedPathSpace final : public PathSpaceBase {
public:
    BoundedPathSpace(std::shared_ptr<PathSpaceBase> backing, std::size_t maxItems)
        : backing_(std::move(backing)), maxItems_(maxItems ? maxItems : 1) {}

    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        if (!backing_) return {.errors = {Error{Error::Code::InvalidPermissions, "No backing PathSpace"}}};

        std::string p = path.toString();

        // Preserve original payload bytes so we can restore after pop loop.
        SlidingBuffer originalBytes;
        // Only snapshot when we have a known size to restore.
        if (data.metadata.serialize && data.obj) {
            data.metadata.serialize(data.obj, originalBytes);
        }

        // Pop until there is room (same type only).
        while (true) {
            {
                std::lock_guard<std::mutex> lock(m_);
                if (counts_[p] < maxItems_) break;
            }
            auto err = backing_->out(path, data.metadata, Out{.doPop = true}, data.obj);
            if (err) {
                // type mismatch or other failure: drop insert
                return {};
            }
            {
                std::lock_guard<std::mutex> lock(m_);
                auto& cnt = counts_[p];
                if (cnt > 0) --cnt;
            }
        }

        // Restore caller's value before inserting (it may have been clobbered by pop loop).
        if (data.metadata.deserialize && !originalBytes.empty()) {
            data.metadata.deserialize(data.obj, originalBytes);
        }

        auto ret = backing_->in(path, data);
        if (ret.errors.empty()) {
            std::lock_guard<std::mutex> lock(m_);
            ++counts_[p];
        }
        return ret;
    }

    auto out(Iterator const& path,
             InputMetadata const& inputMetadata,
             Out const& options,
             void* obj) -> std::optional<Error> override {
        if (!backing_) return Error{Error::Code::InvalidPermissions, "No backing PathSpace"};
        auto err = backing_->out(path, inputMetadata, options, obj);
        if (!err && options.doPop) {
            std::string p = path.toString();
            std::lock_guard<std::mutex> lock(m_);
            auto it = counts_.find(p);
            if (it != counts_.end() && it->second > 0) --it->second;
        }
        return err;
    }

    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        if (backing_) backing_->adoptContextAndPrefix(std::move(context), std::move(prefix));
    }

    auto notify(std::string const& notificationPath) -> void override {
        if (backing_) backing_->notify(notificationPath);
    }

    auto shutdown() -> void override {
        if (backing_) backing_->shutdown();
    }

    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override {
        if (!backing_) return std::unexpected(Error{Error::Code::InvalidPermissions, "No backing PathSpace"});
        return backing_->visit(visitor, options);
    }

protected:
    auto getRootNode() -> Node* override {
        if (!backing_) return nullptr;
        return backing_->getRootNode();
    }

private:
    std::shared_ptr<PathSpaceBase> backing_;
    std::size_t maxItems_;
    std::unordered_map<std::string, std::size_t> counts_;
    std::mutex m_;
};

} // namespace SP
