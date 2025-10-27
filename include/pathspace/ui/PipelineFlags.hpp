#pragma once

#include <cstdint>

namespace SP::UI::PipelineFlags {

inline constexpr std::uint32_t Opaque              = 0x0000'0001u;
inline constexpr std::uint32_t AlphaBlend          = 0x0000'0002u;
inline constexpr std::uint32_t UnpremultipliedSrc  = 0x0000'0004u;

inline constexpr std::uint32_t ClipRect            = 0x0000'0010u;
inline constexpr std::uint32_t ClipPath            = 0x0000'0020u;
inline constexpr std::uint32_t ScissorEnabled      = 0x0000'0040u;

inline constexpr std::uint32_t TextLCD             = 0x0000'0100u;
inline constexpr std::uint32_t TextNoSubpixel      = 0x0000'0200u;

inline constexpr std::uint32_t SrgbFramebuffer     = 0x0000'1000u;
inline constexpr std::uint32_t LinearFramebuffer   = 0x0000'2000u;

inline constexpr std::uint32_t DebugOverdraw       = 0x0001'0000u;
inline constexpr std::uint32_t DebugWireframe      = 0x0002'0000u;
inline constexpr std::uint32_t HighlightPulse      = 0x0004'0000u;

inline constexpr bool is_alpha_pass(std::uint32_t flags) {
    return (flags & AlphaBlend) != 0u;
}

inline constexpr bool requires_unpremultiplied_src(std::uint32_t flags) {
    return (flags & UnpremultipliedSrc) != 0u;
}

} // namespace SP::UI::PipelineFlags
