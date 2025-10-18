#include <pathspace/ui/HtmlAdapter.hpp>

namespace SP::UI::Html {

auto Adapter::emit(Scene::DrawableBucketSnapshot const& snapshot,
                   EmitOptions const& options) -> SP::Expected<EmitResult> {
    (void)snapshot;
    (void)options;
    return std::unexpected(SP::Error{SP::Error::Code::UnknownError,
                                     "HtmlAdapter::emit is not yet implemented"});
}

} // namespace SP::UI::Html
