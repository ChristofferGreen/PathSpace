#include "PathFileSystem.hpp"
#include "core/Error.hpp"

namespace SP {

auto PathFileSystem::in(Iterator const& path, InputData const& data) -> InsertReturn {
    return {};
}

auto PathFileSystem::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    if (inputMetadata.typeInfo != &typeid(std::string))
        return Error{Error::Code::TypeMismatch, "PathFileSystem only supports std::string"};
    auto const p = this->root + "/" + std::string(path.currentToEnd());

    std::ifstream file(p);
    if (!file.is_open())
        return Error{Error::Code::NotFound, "File not found"};
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    *reinterpret_cast<std::string*>(obj) = content;
    return {};
}

auto PathFileSystem::shutdown() -> void {
}

auto PathFileSystem::notify(std::string const& notificationPath) -> void {
}

} // namespace SP