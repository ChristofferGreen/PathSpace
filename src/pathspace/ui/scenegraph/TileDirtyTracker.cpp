#include <pathspace/ui/scenegraph/TileDirtyTracker.hpp>

#include <algorithm>
#include <cmath>

#include <parallel_hashmap/phmap.h>

namespace SP::UI::SceneGraph {

auto TileDirtyTracker::reset() -> void {
    previous_.clear();
    has_previous_ = false;
}

auto TileDirtyTracker::rect_from_hint(Runtime::DirtyRectHint const& hint) -> IntRect {
    auto min_x = static_cast<int32_t>(std::floor(std::min(hint.min_x, hint.max_x)));
    auto max_x = static_cast<int32_t>(std::ceil(std::max(hint.min_x, hint.max_x)));
    auto min_y = static_cast<int32_t>(std::floor(std::min(hint.min_y, hint.max_y)));
    auto max_y = static_cast<int32_t>(std::ceil(std::max(hint.min_y, hint.max_y)));
    return IntRect{min_x, min_y, max_x, max_y};
}

auto TileDirtyTracker::clamp_to_surface(IntRect rect, int width, int height) const
    -> std::optional<IntRect> {
    rect.min_x = std::clamp(rect.min_x, 0, width);
    rect.max_x = std::clamp(rect.max_x, 0, width);
    rect.min_y = std::clamp(rect.min_y, 0, height);
    rect.max_y = std::clamp(rect.max_y, 0, height);
    if (rect.empty()) {
        return std::nullopt;
    }
    return rect;
}

auto TileDirtyTracker::compute_dirty(RenderCommandStore const& current,
                                     std::span<Runtime::DirtyRectHint const> dirty_hints,
                                     int surface_width,
                                     int surface_height,
                                     bool full_repaint) -> std::vector<IntRect> {
    std::vector<IntRect> dirty;
    if (surface_width <= 0 || surface_height <= 0) {
        reset();
        return dirty;
    }

    if (full_repaint) {
        reset();
        dirty.clear();
        previous_ = current;
        has_previous_ = true;
        return dirty; // caller should redraw everything.
    }

    dirty.reserve(current.active_count() + dirty_hints.size());

    // Always honor caller-provided dirty hints.
    for (auto const& hint : dirty_hints) {
        auto rect = clamp_to_surface(rect_from_hint(hint), surface_width, surface_height);
        if (rect) {
            dirty.push_back(*rect);
        }
    }

    phmap::flat_hash_set<std::uint64_t> seen_entities;
    seen_entities.reserve(current.active_count());

    auto mark_dirty_bbox = [&](IntRect const& rect) {
        if (auto clamped = clamp_to_surface(rect, surface_width, surface_height)) {
            dirty.push_back(*clamped);
        }
    };

    auto command_changed = [&](CommandId current_id, CommandId previous_id) -> bool {
        if (current.bbox(current_id).min_x != previous_.bbox(previous_id).min_x
            || current.bbox(current_id).min_y != previous_.bbox(previous_id).min_y
            || current.bbox(current_id).max_x != previous_.bbox(previous_id).max_x
            || current.bbox(current_id).max_y != previous_.bbox(previous_id).max_y) {
            return true;
        }
        if (current.z(current_id) != previous_.z(previous_id)) {
            return true;
        }
        if (current.kind(current_id) != previous_.kind(previous_id)) {
            return true;
        }
        if (current.payload_handle(current_id) != previous_.payload_handle(previous_id)) {
            return true;
        }
        if (current.opacity(current_id) != previous_.opacity(previous_id)) {
            return true;
        }
        return false;
    };

    auto const current_ids = current.active_ids();
    for (auto id : current_ids) {
        auto entity_id = current.entity_id(id);
        seen_entities.insert(entity_id);

        if (has_previous_) {
            auto previous_id = previous_.entity_index(entity_id);
            if (previous_id) {
                if (command_changed(id, *previous_id)) {
                    auto const& old_bbox = previous_.bbox(*previous_id);
                    auto const& new_bbox = current.bbox(id);
                    IntRect combined{
                        .min_x = std::min(old_bbox.min_x, new_bbox.min_x),
                        .min_y = std::min(old_bbox.min_y, new_bbox.min_y),
                        .max_x = std::max(old_bbox.max_x, new_bbox.max_x),
                        .max_y = std::max(old_bbox.max_y, new_bbox.max_y),
                    };
                    mark_dirty_bbox(combined);
                }
                continue;
            }
        }
        mark_dirty_bbox(current.bbox(id)); // new entity or no previous state.
    }

    if (has_previous_) {
        auto const previous_ids = previous_.active_ids();
        for (auto id : previous_ids) {
            auto entity_id = previous_.entity_id(id);
            if (seen_entities.find(entity_id) == seen_entities.end()) {
                mark_dirty_bbox(previous_.bbox(id));
            }
        }
    }

    previous_ = current;
    has_previous_ = true;
    return dirty;
}

} // namespace SP::UI::SceneGraph
