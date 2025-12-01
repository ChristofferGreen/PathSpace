#pragma once
#include "PathSpace.hpp"

namespace SP {

struct Permission {
    bool read    = true;
    bool write   = true;
    bool execute = true;
};

struct PathView : public PathSpaceBase {
    PathView(std::shared_ptr<PathSpaceBase> const& space, std::function<Permission(Iterator const&)> permission, std::string root = "")
        : space(space), permission(permission), root(root) {}
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    virtual auto shutdown() -> void override;
    virtual auto notify(std::string const& notificationPath) -> void override;
    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override;

protected:
    auto getRootNode() -> Node* override;

private:
    std::string                                root;
    std::function<Permission(Iterator const&)> permission;
    std::shared_ptr<PathSpaceBase>             space;
};

} // namespace SP
