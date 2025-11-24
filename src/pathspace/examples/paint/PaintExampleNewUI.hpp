#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <string>
#include <string_view>

namespace PathSpaceExamples::PaintExampleNew {

struct ButtonUiResult {
    SP::UI::Builders::WidgetPath stack_path;
    std::string button_path;
    float layout_width = 0.0f;
    float layout_height = 0.0f;
};

auto MountButtonUI(SP::PathSpace& space,
                   SP::App::ConcretePathView window_view,
                   int window_width,
                   int window_height,
                   SP::UI::Declarative::Button::Args button_args)
    -> SP::Expected<ButtonUiResult>;

auto EnableWindowInput(SP::PathSpace& space,
                       SP::Window::CreateResult const& window,
                       std::string_view telemetry_tag) -> SP::Expected<void>;

} // namespace PathSpaceExamples::PaintExampleNew
