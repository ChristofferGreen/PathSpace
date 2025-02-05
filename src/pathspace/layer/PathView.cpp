#include "PathView.hpp"
#include "core/Error.hpp"

namespace SP {

auto PathView::in(Iterator const& path, InputData const& data) -> InsertReturn {
    if (!this->space)
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathSpace not set"}}};
    // Check write permission for the path
    auto perm = permission(path);
    if (!perm.write) {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "Write permission denied for path: " + path.toString()}}};
    }

    // If we have a root path, prepend it
    Iterator fullPath = root.empty() ? path : Iterator{root + "/" + path.toString()};

    // Forward to base class implementation
    return this->space->in(fullPath, data);
}

auto PathView::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    if (!this->space)
        return Error{Error::Code::InvalidPermissions, "PathSpace not set"};
    // Check read permission for the path
    auto perm = permission(path);
    if (!perm.read) {
        return Error{Error::Code::InvalidPermissions, "Read permission denied for path: " + path.toString()};
    }

    // If we have a root path, prepend it
    Iterator fullPath = root.empty() ? path : Iterator{root + "/" + path.toString()};

    // Forward to base class implementation
    return this->space->out(fullPath, inputMetadata, options, obj);
}

auto PathView::shutdown() -> void {
    if (!this->space)
        return;
    this->space->shutdown();
}

auto PathView::notify(std::string const& notificationPath) -> void {
    if (!this->space)
        return;
    this->space->notify(notificationPath);
}

} // namespace SP