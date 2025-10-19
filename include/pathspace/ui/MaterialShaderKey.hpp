#pragma once

#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/SurfaceTypes.hpp>
#include <pathspace/ui/PipelineFlags.hpp>

namespace SP::UI {

struct MaterialShaderKey {
    std::uint32_t pipeline_flags = 0;
    bool alpha_blend = false;
    bool requires_unpremultiplied = false;
    bool srgb_framebuffer = false;
    bool uses_image = false;
    bool debug_overdraw = false;
    bool debug_wireframe = false;

    auto operator==(MaterialShaderKey const&) const -> bool = default;
};

[[nodiscard]] auto make_shader_key(MaterialDescriptor const& material,
                                   Builders::SurfaceDesc const& surface) -> MaterialShaderKey;

} // namespace SP::UI
