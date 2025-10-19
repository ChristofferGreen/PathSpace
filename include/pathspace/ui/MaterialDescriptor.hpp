#pragma once

#include <array>
#include <cstdint>

namespace SP::UI {

struct MaterialDescriptor {
    std::uint32_t material_id = 0;
    std::uint32_t pipeline_flags = 0;
    std::uint32_t primary_draw_kind = 0;
    std::uint32_t command_count = 0;
    std::uint32_t drawable_count = 0;
    std::array<float, 4> color_rgba{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> tint_rgba{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint64_t resource_fingerprint = 0;
    bool uses_image = false;
};

} // namespace SP::UI

