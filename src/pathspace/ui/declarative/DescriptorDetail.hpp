#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace SP::UI::Declarative::DescriptorDetail {

namespace BuilderWidgets = SP::UI::Runtime::Widgets;

struct ThemeContext {
    BuilderWidgets::WidgetTheme theme{};
    std::string name;
};

auto MakeDescriptorError(std::string message,
                         SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error;

auto KindFromString(std::string_view raw) -> std::optional<WidgetKind>;

auto ResolveThemeForWidget(PathSpace& space,
                           SP::UI::Runtime::WidgetPath const& widget)
    -> SP::Expected<ThemeContext>;

auto ReadButtonDescriptor(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<ButtonDescriptor>;
auto ReadToggleDescriptor(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<ToggleDescriptor>;
auto ReadSliderDescriptor(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<SliderDescriptor>;
auto ReadListDescriptor(PathSpace& space,
                        std::string const& root,
                        BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<ListDescriptor>;
auto ReadTreeDescriptor(PathSpace& space,
                        std::string const& root,
                        BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<TreeDescriptor>;
auto ReadStackDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<StackDescriptor>;
auto ReadLabelDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<LabelDescriptor>;
auto ReadInputFieldDescriptor(PathSpace& space,
                              SP::UI::Runtime::WidgetPath const& widget,
                              BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<InputFieldDescriptor>;
auto ReadPaintSurfaceDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<PaintSurfaceDescriptor>;

} // namespace SP::UI::Declarative::DescriptorDetail
