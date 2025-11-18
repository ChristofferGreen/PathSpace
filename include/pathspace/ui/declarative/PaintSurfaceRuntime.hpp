#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/PaintSurfaceTypes.hpp>

#include <vector>

namespace SP::UI::Declarative::PaintRuntime {

using WidgetAction = SP::UI::Builders::Widgets::Reducers::WidgetAction;

auto EnsureBufferDefaults(PathSpace& space,
                          std::string const& widget_path,
                          PaintBufferMetrics const& defaults) -> SP::Expected<void>;

auto HandleAction(PathSpace& space,
                  WidgetAction const& action) -> SP::Expected<bool>;

auto LoadStrokeRecords(PathSpace& space,
                       std::string const& widget_path)
    -> SP::Expected<std::vector<PaintStrokeRecord>>;

auto ReadBufferMetrics(PathSpace& space,
                       std::string const& widget_path) -> SP::Expected<PaintBufferMetrics>;

} // namespace SP::UI::Declarative::PaintRuntime
