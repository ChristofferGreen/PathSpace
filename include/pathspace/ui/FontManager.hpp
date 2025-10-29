#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>

namespace SP::UI {

class FontManager {
public:
    explicit FontManager(PathSpace& space);

    auto register_font(App::AppRootPathView appRoot,
                       Builders::Resources::Fonts::RegisterFontParams const& params)
        -> SP::Expected<Builders::Resources::Fonts::FontResourcePaths>;

private:
    PathSpace* space_;
};

} // namespace SP::UI

