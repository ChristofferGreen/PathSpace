#include "BuildersDetail.hpp"

namespace SP::UI::Builders {

using namespace Detail;

auto maybe_schedule_auto_render(PathSpace& space,
                                std::string const& targetPath,
                                PathWindowView::PresentStats const& stats,
                                PathWindowView::PresentPolicy const& policy) -> SP::Expected<bool> {
    return maybe_schedule_auto_render_impl(space, targetPath, stats, policy);
}

auto resolve_app_relative(AppRootPathView root,
                          UnvalidatedPathView maybeRelative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, maybeRelative);
}

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> SP::Expected<ConcretePath> {
    return SP::App::derive_target_base(root, rendererPath, targetPath);
}

namespace Window::TestHooks {

void SetBeforePresentHook(BeforePresentHook hook) {
    std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
    before_present_hook_storage() = std::move(hook);
}

void ResetBeforePresentHook() {
    std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
    before_present_hook_storage() = nullptr;
}

} // namespace Window::TestHooks

} // namespace SP::UI::Builders
