#include "PathView.hpp"

namespace SP {

auto PathView::in(Iterator const& path, InputData const& data) -> InsertReturn {
    return InsertReturn{};
}
auto PathView::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    return std::nullopt;
}
auto PathView::shutdown() -> void {}
auto PathView::clear() -> void {}

} // namespace SP