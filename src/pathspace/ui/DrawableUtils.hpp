#pragma once

#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SP::UI::detail {

inline bool bounding_box_intersects(Scene::DrawableBucketSnapshot const& bucket,
                                    std::size_t drawable_index,
                                    int width,
                                    int height) {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return true;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return true;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    if (box.max[0] <= 0.0f || box.max[1] <= 0.0f) {
        return false;
    }
    if (box.min[0] >= static_cast<float>(width)
        || box.min[1] >= static_cast<float>(height)) {
        return false;
    }
    if (box.max[0] <= box.min[0] || box.max[1] <= box.min[1]) {
        return false;
    }
    return true;
}

inline bool bounding_sphere_intersects(Scene::DrawableBucketSnapshot const& bucket,
                                       std::size_t drawable_index,
                                       int width,
                                       int height) {
    if (drawable_index >= bucket.bounds_spheres.size()) {
        return true;
    }
    auto const& sphere = bucket.bounds_spheres[drawable_index];
    auto const radius = std::max(0.0f, sphere.radius);
    auto const min_x = sphere.center[0] - radius;
    auto const max_x = sphere.center[0] + radius;
    auto const min_y = sphere.center[1] - radius;
    auto const max_y = sphere.center[1] + radius;
    if (max_x <= 0.0f || max_y <= 0.0f) {
        return false;
    }
    if (min_x >= static_cast<float>(width)
        || min_y >= static_cast<float>(height)) {
        return false;
    }
    return true;
}

inline std::vector<std::uint32_t> build_draw_order(Scene::DrawableBucketSnapshot const& bucket) {
    std::vector<std::uint32_t> order;
    order.reserve(bucket.drawable_ids.size());
    if (!bucket.opaque_indices.empty() || !bucket.alpha_indices.empty()) {
        order.insert(order.end(), bucket.opaque_indices.begin(), bucket.opaque_indices.end());
        order.insert(order.end(), bucket.alpha_indices.begin(), bucket.alpha_indices.end());
    }
    if (order.empty()) {
        for (std::uint32_t i = 0; i < bucket.drawable_ids.size(); ++i) {
            order.push_back(i);
        }
    }
    return order;
}

inline bool point_inside_clip(float x,
                              float y,
                              Scene::DrawableBucketSnapshot const& bucket,
                              std::size_t drawable_index) {
    if (drawable_index >= bucket.clip_head_indices.size()) {
        return true;
    }
    auto node_index = bucket.clip_head_indices[drawable_index];
    if (node_index < 0) {
        return true;
    }
    while (node_index >= 0) {
        if (static_cast<std::size_t>(node_index) >= bucket.clip_nodes.size()) {
            break;
        }
        auto const& node = bucket.clip_nodes[static_cast<std::size_t>(node_index)];
        if (node.type == Scene::ClipNodeType::Rect) {
            auto const& rect = node.rect;
            if (x < rect.min_x || x > rect.max_x || y < rect.min_y || y > rect.max_y) {
                return false;
            }
        }
        node_index = node.next;
    }
    return true;
}

inline std::vector<std::string> build_focus_chain(std::string const& authoring_id) {
    std::vector<std::string> chain;
    if (authoring_id.empty()) {
        return chain;
    }
    chain.push_back(authoring_id);
    auto current = authoring_id;
    while (true) {
        auto pos = current.rfind('/');
        if (pos == std::string::npos) {
            break;
        }
        current = current.substr(0, pos);
        if (current.empty()) {
            break;
        }
        chain.push_back(current);
    }
    return chain;
}

inline bool point_inside_bounds(float x,
                                float y,
                                Scene::DrawableBucketSnapshot const& bucket,
                                std::size_t drawable_index) {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return true;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return true;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    return (x >= box.min[0] && x <= box.max[0]
            && y >= box.min[1] && y <= box.max[1]);
}

} // namespace SP::UI::detail
