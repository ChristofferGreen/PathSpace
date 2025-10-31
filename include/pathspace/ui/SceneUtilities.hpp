#pragma once

#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <array>
#include <optional>
#include <string>

namespace SP::UI::Scene {

[[nodiscard]] auto MakeIdentityTransform() -> Transform;

struct SolidBackgroundOptions {
    Transform transform = MakeIdentityTransform();
    std::array<float, 4> color{0.11f, 0.12f, 0.15f, 1.0f};
    std::uint64_t drawable_id = 0x9000FFF0ull;
    std::optional<std::uint64_t> fingerprint;
    std::string authoring_node_id;
    int layer = 0;
    float z = 0.0f;
    std::uint32_t material_id = 0;
    std::uint32_t pipeline_flags = 0;
    std::uint8_t visibility = 1;
};

auto BuildSolidBackground(float width,
                          float height,
                          SolidBackgroundOptions const& options = {}) -> DrawableBucketSnapshot;

auto TranslateDrawableBucket(DrawableBucketSnapshot& bucket, float dx, float dy) -> void;

auto AppendDrawableBucket(DrawableBucketSnapshot& dest,
                          DrawableBucketSnapshot const& source) -> void;

} // namespace SP::UI::Scene
