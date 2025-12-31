#pragma once

#include <pathspace/ui/scenegraph/RenderCommandStore.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>

#include <optional>
#include <span>
#include <vector>

namespace SP::UI::SceneGraph {

// Tracks the previous frame's commands and computes which rectangles need to
// be re-rendered when entities change or when legacy DirtyRectHint inputs are
// provided.
class TileDirtyTracker {
public:
    TileDirtyTracker() = default;

    // Clears cached state; the next compute will treat the frame as a full repaint.
    auto reset() -> void;

    // Returns surface-space rectangles that should be treated as dirty for the
    // current frame. If full_repaint is true, the tracker resets and returns
    // an empty list so callers render every tile.
    auto compute_dirty(RenderCommandStore const& current,
                       std::span<Runtime::DirtyRectHint const> dirty_hints,
                       int surface_width,
                       int surface_height,
                       bool full_repaint) -> std::vector<IntRect>;

private:
    [[nodiscard]] auto clamp_to_surface(IntRect rect, int width, int height) const
        -> std::optional<IntRect>;

    [[nodiscard]] static auto rect_from_hint(Runtime::DirtyRectHint const& hint) -> IntRect;

    RenderCommandStore previous_;
    bool has_previous_ = false;
};

} // namespace SP::UI::SceneGraph
