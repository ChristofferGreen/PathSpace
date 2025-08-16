#include "PathIO.hpp"
#include "core/Error.hpp"

namespace SP {

auto PathIO::in(Iterator const& path, InputData const& data) -> InsertReturn {
    (void)path;
    (void)data;
    // Base PathIO does not implement writes; subclasses should override.
    return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathIO base does not support in()"}}};
}

auto PathIO::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    (void)path;
    (void)inputMetadata;
    (void)options;
    (void)obj;
    // Base PathIO does not implement reads; subclasses should override.
    return Error{Error::Code::InvalidPermissions, "PathIO base does not support out()"};
}

auto PathIO::shutdown() -> void {
    // Base PathIO: no-op
}

auto PathIO::notify(std::string const& notificationPath) -> void {
    (void)notificationPath;
    // Base PathIO: no-op
}

} // namespace SP