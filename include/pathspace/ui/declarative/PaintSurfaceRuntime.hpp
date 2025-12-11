#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/PaintSurfaceTypes.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>

#include <vector>

namespace SP {
class PathSpaceBase;
}

namespace SP::UI::Declarative::PaintRuntime {

using WidgetAction = SP::UI::Declarative::Reducers::WidgetAction;

auto EnsureBufferDefaults(PathSpaceBase& space,
                          std::string const& widget_path,
                          PaintBufferMetrics const& defaults) -> SP::Expected<void>;

auto HandleAction(PathSpaceBase& space,
                  WidgetAction const& action) -> SP::Expected<bool>;

auto LoadStrokeRecords(PathSpaceBase& space,
                       std::string const& widget_path)
    -> SP::Expected<std::vector<PaintStrokeRecord>>;

auto ReadBufferMetrics(PathSpaceBase& space,
                       std::string const& widget_path) -> SP::Expected<PaintBufferMetrics>;

auto ReadStrokePointsConsistent(PathSpaceBase& space,
                                std::string const& widget_path,
                                std::uint64_t stroke_id)
    -> SP::Expected<std::vector<PaintStrokePoint>>;

auto ApplyLayoutSize(PathSpaceBase& space,
                     std::string const& widget_path) -> SP::Expected<bool>;

inline auto ApplyLayoutSize(PathSpaceBase& space,
                            SP::UI::Runtime::WidgetPath const& widget)
    -> SP::Expected<bool> {
    return ApplyLayoutSize(space, widget.getPath());
}

} // namespace SP::UI::Declarative::PaintRuntime
