#pragma once
#include "PathSpace.hpp"

#include <optional>
#include <utility>

namespace SP {

struct Permission {
    bool read    = true;
    bool write   = true;
    bool execute = true;
};

struct PathView : public PathSpaceBase {
    PathView(std::shared_ptr<PathSpaceBase> const& space, std::function<Permission(Iterator const&)> permission, std::string root = "")
        : root(std::move(root)), permission(std::move(permission)), space(space) {}
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;
    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override;

protected:
    auto getRootNode() -> Node* override;

private:
    std::string                                root;
    std::function<Permission(Iterator const&)> permission;
    std::shared_ptr<PathSpaceBase>             space;
};

namespace testing {
std::string joinCanonicalForTest(std::string const& prefix, std::string const& suffix);
std::optional<std::string> stripPrefixForTest(std::string const& absolute, std::string const& prefix);
} // namespace testing

} // namespace SP
