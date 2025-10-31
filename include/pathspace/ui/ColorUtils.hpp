#pragma once

#include <array>
#include <algorithm>

namespace SP::UI::Color {

inline auto Mix(std::array<float, 4> base,
                std::array<float, 4> target,
                float amount) -> std::array<float, 4> {
    amount = std::clamp(amount, 0.0f, 1.0f);
    std::array<float, 4> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = std::clamp(base[i] * (1.0f - amount) + target[i] * amount, 0.0f, 1.0f);
    }
    out[3] = std::clamp(base[3], 0.0f, 1.0f);
    return out;
}

inline auto Lighten(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return Mix(color, {1.0f, 1.0f, 1.0f, color[3]}, amount);
}

inline auto Desaturate(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    auto gray = std::array<float, 4>{0.5f, 0.5f, 0.5f, color[3]};
    return Mix(color, gray, amount);
}

} // namespace SP::UI::Color

