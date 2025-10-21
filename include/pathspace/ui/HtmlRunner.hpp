#pragma once

#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <span>

namespace SP::UI::Html {

struct CanvasReplayOptions {
    std::uint64_t base_drawable_id = 1;
    std::uint32_t default_layer = 0;
    float z_step = 0.01f;
    std::span<Scene::StrokePoint const> stroke_points{};
};

auto commands_to_bucket(std::span<CanvasCommand const> commands,
                        CanvasReplayOptions const& options = {}) -> SP::Expected<Scene::DrawableBucketSnapshot>;

} // namespace SP::UI::Html
