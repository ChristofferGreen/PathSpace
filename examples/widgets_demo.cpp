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

    auto buttonRevision = Scene::ReadCurrentRevision(space, button->scene);
    if (!buttonRevision) {
        std::cerr << "Button scene published but revision unreadable: "
                  << buttonRevision.error().message.value_or("unspecified error") << "\n";
        return 2;
    }

    std::cout << "widgets_demo published button widget:\n"
              << "  scene: " << button->scene.getPath() << " (revision "
              << buttonRevision->revision << ")\n"
              << "  state path: " << button->state.getPath() << "\n"
              << "  label path: " << button->label.getPath() << "\n";

    Widgets::ToggleParams toggleParams{};
    toggleParams.name = "primary_toggle";
    toggleParams.style.width = 60.0f;
    toggleParams.style.height = 32.0f;

    auto toggle = Widgets::CreateToggle(space, appRootView, toggleParams);
    if (!toggle) {
        std::cerr << "Failed to create toggle widget: "
                  << toggle.error().message.value_or("unspecified error") << "\n";
        return 3;
    }

    Widgets::ToggleState checkedState{};
    checkedState.checked = true;
    auto toggleChanged = Widgets::UpdateToggleState(space, *toggle, checkedState);
    if (!toggleChanged) {
        std::cerr << "Failed to update toggle state: "
                  << toggleChanged.error().message.value_or("unspecified error") << "\n";
        return 4;
    }

    auto toggleRevision = Scene::ReadCurrentRevision(space, toggle->scene);
    if (!toggleRevision) {
        std::cerr << "Toggle scene published but revision unreadable: "
                  << toggleRevision.error().message.value_or("unspecified error") << "\n";
        return 5;
    }

    std::cout << "widgets_demo published toggle widget:\n"
              << "  scene: " << toggle->scene.getPath() << " (revision "
              << toggleRevision->revision << ")\n"
              << "  state path: " << toggle->state.getPath() << "\n"
              << "  initial checked state applied via UpdateToggleState\n"
              << "Inspect the PathSpace tree to wire widgets into a renderer target.\n";

    return 0;
}
