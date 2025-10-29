#include <pathspace/ui/FontManager.hpp>

namespace SP::UI {

FontManager::FontManager(PathSpace& space)
    : space_(&space) {
}

auto FontManager::register_font(App::AppRootPathView appRoot,
                                Builders::Resources::Fonts::RegisterFontParams const& params)
    -> SP::Expected<Builders::Resources::Fonts::FontResourcePaths> {
    return Builders::Resources::Fonts::Register(*space_, appRoot, params);
}

} // namespace SP::UI
