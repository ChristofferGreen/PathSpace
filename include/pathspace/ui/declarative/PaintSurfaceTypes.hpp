#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace SP::UI::Declarative {

struct PaintStrokePoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct PaintStrokeMeta {
    float brush_size = 6.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    bool committed = false;
};

struct PaintStrokeRecord {
    std::uint64_t id = 0;
    PaintStrokeMeta meta{};
    std::vector<PaintStrokePoint> points;
};

struct PaintBufferMetrics {
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    float dpi = 96.0f;
};

struct PaintBufferViewport {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

} // namespace SP::UI::Declarative
