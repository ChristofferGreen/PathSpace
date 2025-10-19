#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>

#include <chrono>
#include <iostream>
#include <string>

int main() {
    using namespace SP;
    using namespace SP::UI::Builders;

    PathSpace space;
    AppRootPath appRoot{"/system/applications/widgets_demo"};

    Widgets::ButtonParams primary{};
    primary.name = "primary";
    primary.label = "Primary";
    primary.style.width = 180.0f;
    primary.style.height = 44.0f;

    SP::App::AppRootPathView appRootView{appRoot.getPath()};

    auto button = Widgets::CreateButton(space, appRootView, primary);
    if (!button) {
        std::cerr << "Failed to create button widget: "
                  << button.error().message.value_or("unspecified error") << "\n";
        return 1;
    }

    auto revision = Scene::ReadCurrentRevision(space, button->scene);
    if (!revision) {
        std::cerr << "Button scene published but revision unreadable: "
                  << revision.error().message.value_or("unspecified error") << "\n";
        return 2;
    }

    std::cout << "widgets_demo published button widget:\n"
              << "  scene: " << button->scene.getPath() << " (revision "
              << revision->revision << ")\n"
              << "  state path: " << button->state.getPath() << "\n"
              << "  label path: " << button->label.getPath() << "\n"
              << "Inspect the PathSpace tree to wire it into a renderer target.\n";

    return 0;
}
