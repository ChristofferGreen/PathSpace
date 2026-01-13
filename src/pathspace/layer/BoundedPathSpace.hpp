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
        : backing(std::move(backing)), maxItems(maxItems ? maxItems : 1) {}

    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        if (!this->backing) return {.errors = {Error{Error::Code::InvalidPermissions, "No backing PathSpace"}}};

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
                std::lock_guard<std::mutex> lock(this->countsMutex);
                if (this->counts[p] < this->maxItems) break;
            }
            auto err = this->backing->out(path, data.metadata, Out{.doPop = true}, data.obj);
            if (err) {
                // type mismatch or other failure: drop insert
                return {};
            }
            {
                std::lock_guard<std::mutex> lock(this->countsMutex);
                auto& cnt = this->counts[p];
                if (cnt > 0) --cnt;
            }
        }

        // Restore caller's value before inserting (it may have been clobbered by pop loop).
        if (data.metadata.deserialize && !originalBytes.empty()) {
            data.metadata.deserialize(data.obj, originalBytes);
        }

        auto ret = this->backing->in(path, data);
        if (ret.errors.empty()) {
            std::lock_guard<std::mutex> lock(this->countsMutex);
            ++this->counts[p];
        }
        return ret;
    }

    auto out(Iterator const& path,
             InputMetadata const& inputMetadata,
             Out const& options,
             void* obj) -> std::optional<Error> override {
        if (!this->backing) return Error{Error::Code::InvalidPermissions, "No backing PathSpace"};
        auto err = this->backing->out(path, inputMetadata, options, obj);
        if (!err && options.doPop) {
            std::string p = path.toString();
            std::lock_guard<std::mutex> lock(this->countsMutex);
            auto it = this->counts.find(p);
            if (it != this->counts.end() && it->second > 0) --it->second;
        }
        return err;
    }

    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        if (this->backing) this->backing->adoptContextAndPrefix(std::move(context), std::move(prefix));
    }

    auto notify(std::string const& notificationPath) -> void override {
        if (this->backing) this->backing->notify(notificationPath);
    }

    auto shutdown() -> void override {
        if (this->backing) this->backing->shutdown();
    }

    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override {
        if (!this->backing) return std::unexpected(Error{Error::Code::InvalidPermissions, "No backing PathSpace"});
        return this->backing->visit(visitor, options);
    }

protected:
    auto getRootNode() -> Node* override {
        if (!this->backing) return nullptr;
        return this->backing->getRootNode();
    }

private:
    std::shared_ptr<PathSpaceBase> backing;
    std::size_t maxItems;
    std::unordered_map<std::string, std::size_t> counts;
    std::mutex countsMutex;
};

} // namespace SP
