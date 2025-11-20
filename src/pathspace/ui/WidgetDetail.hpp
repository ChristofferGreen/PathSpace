#pragma once

#include "BuildersDetail.hpp"
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>

// Widgets rely on a large set of inline helpers; isolate them here so non-widget
// translation units can include BuildersDetail.hpp without absorbing the extra
// drawable/metadata inlines.
namespace SP::UI::Builders::Detail {

#include "WidgetDrawablesDetail.inl"
#include "WidgetMetadataDetail.inl"

} // namespace SP::UI::Builders::Detail
