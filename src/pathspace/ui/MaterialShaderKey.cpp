#include <pathspace/ui/MaterialShaderKey.hpp>

namespace SP::UI {

auto make_shader_key(MaterialDescriptor const& material,
                     Builders::SurfaceDesc const& surface) -> MaterialShaderKey {
    MaterialShaderKey key{};
    key.pipeline_flags = material.pipeline_flags;
    key.alpha_blend = (material.pipeline_flags & PipelineFlags::AlphaBlend) != 0u;
    key.requires_unpremultiplied = PipelineFlags::requires_unpremultiplied_src(material.pipeline_flags);
    key.srgb_framebuffer =
        (material.pipeline_flags & PipelineFlags::SrgbFramebuffer) != 0u
        || surface.color_space == Builders::ColorSpace::sRGB;
    key.uses_image = material.uses_image;
    key.debug_overdraw = (material.pipeline_flags & PipelineFlags::DebugOverdraw) != 0u;
    key.debug_wireframe = (material.pipeline_flags & PipelineFlags::DebugWireframe) != 0u;
    return key;
}

} // namespace SP::UI
