#pragma once

#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

namespace SP::UI {

using Runtime::SurfaceDesc;

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

struct MaterialResourceResidency {
    std::uint64_t fingerprint = 0;
    std::uint64_t cpu_bytes = 0;
    std::uint64_t gpu_bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool uses_image = false;
    bool uses_font_atlas = false;
};

[[nodiscard]] auto make_shader_key(MaterialDescriptor const& material,
                                   SurfaceDesc const& surface) -> MaterialShaderKey;

} // namespace SP::UI
