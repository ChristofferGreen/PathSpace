#include "PathIO.hpp"
#include "core/Error.hpp"

namespace SP {

auto PathIO::in(Iterator const& path, InputData const& data) -> InsertReturn {
    return {.errors = {Error{Error::Code::InvalidPermissions, "PathIO does not support in operations"}}};
}

auto PathIO::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    return {Error{Error::Code::InvalidPermissions, "PathIO does not support out operations"}};
}

auto PathIO::shutdown() -> void {
}

auto PathIO::notify(std::string const& notificationPath) -> void {
}

} // namespace SP