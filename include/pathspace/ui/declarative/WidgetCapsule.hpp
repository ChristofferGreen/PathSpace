#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/WidgetRenderPackage.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace SP::UI::Declarative {

struct WidgetCapsule {
    WidgetKind   kind = WidgetKind::Button;
    std::string  widget_path;
    std::string  render_lambda;
    std::uint64_t render_revision = 0;
};

[[nodiscard]] auto CapsulesFeatureEnabled() -> bool;
[[nodiscard]] auto CapsulesOnlyRuntimeEnabled() -> bool;

[[nodiscard]] auto LoadWidgetCapsule(PathSpace& space,
                                     std::string const& widget_root,
                                     WidgetKind kind) -> SP::Expected<WidgetCapsule>;

[[nodiscard]] auto BuildRenderPackageFromBucket(
    WidgetCapsule const& capsule,
    SP::UI::Scene::DrawableBucketSnapshot const& bucket,
    std::vector<SP::UI::Runtime::DirtyRectHint> const& pending_dirty,
    std::uint64_t render_sequence) -> WidgetRenderPackage;

} // namespace SP::UI::Declarative
