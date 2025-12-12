#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>

namespace SP::UI::Declarative {

enum class WidgetSurfaceKind : std::uint8_t {
    Software = 0,
    External = 1,
};

enum class WidgetSurfaceFlags : std::uint32_t {
    None              = 0,
    Opaque            = 1u << 0,
    AlphaPremultiplied = 1u << 1,
    StretchToFit      = 1u << 2,
};

[[nodiscard]] constexpr auto operator|(WidgetSurfaceFlags lhs, WidgetSurfaceFlags rhs)
    -> WidgetSurfaceFlags {
    return static_cast<WidgetSurfaceFlags>(static_cast<std::uint32_t>(lhs)
                                           | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr auto operator&(WidgetSurfaceFlags lhs, WidgetSurfaceFlags rhs)
    -> WidgetSurfaceFlags {
    return static_cast<WidgetSurfaceFlags>(static_cast<std::uint32_t>(lhs)
                                           & static_cast<std::uint32_t>(rhs));
}

inline constexpr auto operator|=(WidgetSurfaceFlags& lhs, WidgetSurfaceFlags rhs)
    -> WidgetSurfaceFlags& {
    lhs = lhs | rhs;
    return lhs;
}

inline constexpr auto operator&=(WidgetSurfaceFlags& lhs, WidgetSurfaceFlags rhs)
    -> WidgetSurfaceFlags& {
    lhs = lhs & rhs;
    return lhs;
}

struct WidgetSurface {
    WidgetSurfaceKind  kind = WidgetSurfaceKind::Software;
    WidgetSurfaceFlags flags = WidgetSurfaceFlags::None;
    std::uint32_t      width = 0;
    std::uint32_t      height = 0;
    std::uint64_t      fingerprint = 0;
    std::array<float, 4> logical_bounds{0.0f, 0.0f, 0.0f, 0.0f};
};

struct WidgetRenderPackage {
    std::uint64_t capsule_revision = 0;
    std::uint64_t render_sequence = 0;
    std::uint64_t content_hash = 0;
    SP::UI::Runtime::DirtyRectHint dirty_rect{};
    std::vector<std::uint32_t> command_kinds;
    std::vector<std::uint8_t> command_payload;
    std::vector<std::uint64_t> texture_fingerprints;
    std::vector<WidgetSurface> surfaces;
};

} // namespace SP::UI::Declarative

namespace SP {

struct SlidingBuffer;

template <typename T>
auto serialize(T const& value, SlidingBuffer& buffer) -> std::optional<Error>;

template <typename T>
auto deserialize(SlidingBuffer const& buffer) -> Expected<T>;

template <typename T>
auto deserialize_pop(SlidingBuffer& buffer) -> Expected<T>;

template <>
auto serialize(UI::Declarative::WidgetRenderPackage const& package,
               SlidingBuffer& buffer) -> std::optional<Error>;

template <>
auto deserialize<UI::Declarative::WidgetRenderPackage>(SlidingBuffer const& buffer)
    -> Expected<UI::Declarative::WidgetRenderPackage>;

template <>
auto deserialize_pop<UI::Declarative::WidgetRenderPackage>(SlidingBuffer& buffer)
    -> Expected<UI::Declarative::WidgetRenderPackage>;

} // namespace SP
