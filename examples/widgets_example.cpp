#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace SP;
    using namespace SP::UI::Builders;

    PathSpace space;
    AppRootPath appRoot{"/system/applications/widgets_example"};

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

    std::cout << "widgets_example published button widget:\n"
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

    std::cout << "widgets_example published toggle widget:\n"
              << "  scene: " << toggle->scene.getPath() << " (revision "
              << toggleRevision->revision << ")\n"
              << "  state path: " << toggle->state.getPath() << "\n"
              << "  initial checked state applied via UpdateToggleState\n"
              << "Inspect the PathSpace tree to wire widgets into a renderer target.\n";

    Widgets::SliderParams sliderParams{};
    sliderParams.name = "volume_slider";
    sliderParams.minimum = 0.0f;
    sliderParams.maximum = 100.0f;
    sliderParams.value = 25.0f;
    sliderParams.step = 5.0f;

    auto slider = Widgets::CreateSlider(space, appRootView, sliderParams);
    if (!slider) {
        std::cerr << "Failed to create slider widget: "
                  << slider.error().message.value_or("unspecified error") << "\n";
        return 6;
    }

    Widgets::SliderState sliderState{};
    sliderState.value = 45.0f;
    auto sliderChanged = Widgets::UpdateSliderState(space, *slider, sliderState);
    if (!sliderChanged) {
        std::cerr << "Failed to update slider state: "
                  << sliderChanged.error().message.value_or("unspecified error") << "\n";
        return 7;
    }

    auto sliderRevision = Scene::ReadCurrentRevision(space, slider->scene);
    if (!sliderRevision) {
        std::cerr << "Slider scene published but revision unreadable: "
                  << sliderRevision.error().message.value_or("unspecified error") << "\n";
        return 8;
    }

    std::cout << "widgets_example published slider widget:\n"
              << "  scene: " << slider->scene.getPath() << " (revision "
              << sliderRevision->revision << ")\n"
              << "  state path: " << slider->state.getPath() << "\n"
              << "  range path: " << slider->range.getPath() << "\n";

    Widgets::ListParams listParams{};
    listParams.name = "inventory_list";
    listParams.items = {
        Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
        Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
        Widgets::ListItem{.id = "elixir", .label = "Elixir", .enabled = true},
    };
    listParams.style.width = 240.0f;
    listParams.style.item_height = 36.0f;

    auto list = Widgets::CreateList(space, appRootView, listParams);
    if (!list) {
        std::cerr << "Failed to create list widget: "
                  << list.error().message.value_or("unspecified error") << "\n";
        return 9;
    }

    Widgets::ListState listState{};
    listState.selected_index = 1;
    auto listChanged = Widgets::UpdateListState(space, *list, listState);
    if (!listChanged) {
        std::cerr << "Failed to update list state: "
                  << listChanged.error().message.value_or("unspecified error") << "\n";
        return 10;
    }

    auto listRevision = Scene::ReadCurrentRevision(space, list->scene);
    if (!listRevision) {
        std::cerr << "List scene published but revision unreadable: "
                  << listRevision.error().message.value_or("unspecified error") << "\n";
        return 11;
    }

    std::cout << "widgets_example published list widget:\n"
              << "  scene: " << list->scene.getPath() << " (revision "
              << listRevision->revision << ")\n"
              << "  state path: " << list->state.getPath() << "\n"
              << "  items path: " << list->items.getPath() << "\n";

    return 0;
}
