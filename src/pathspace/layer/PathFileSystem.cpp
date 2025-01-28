#include "PathFileSystem.hpp"
#include "core/Error.hpp"

namespace SP {

auto PathFileSystem::in(Iterator const& path, InputData const& data) -> InsertReturn {
    return {};
}
auto PathFileSystem::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    if (inputMetadata.typeInfo != &typeid(std::string))
        return Error{Error::Code::TypeMismatch, "PathFileSystem only supports std::string"};
    auto const p = this->root + "/" + path.toString();
    return {};
}

auto PathFileSystem::shutdown() -> void {
}

auto PathFileSystem::notify(std::string const& notificationPath) -> void {
}

} // namespace SP