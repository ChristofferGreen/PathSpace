#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <pathspace/ui/runtime/RenderSettings.hpp>

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

enum class PaintGpuState : std::uint8_t {
    Idle = 0,
    DirtyPartial,
    DirtyFull,
    Uploading,
    Ready,
    Error,
};

inline auto PaintGpuStateToString(PaintGpuState state) -> std::string_view {
    switch (state) {
    case PaintGpuState::Idle: return "Idle";
    case PaintGpuState::DirtyPartial: return "DirtyPartial";
    case PaintGpuState::DirtyFull: return "DirtyFull";
    case PaintGpuState::Uploading: return "Uploading";
    case PaintGpuState::Ready: return "Ready";
    case PaintGpuState::Error: return "Error";
    }
    return "Idle";
}

inline auto PaintGpuStateFromString(std::string_view value) -> PaintGpuState {
    if (value == "DirtyPartial") {
        return PaintGpuState::DirtyPartial;
    }
    if (value == "DirtyFull") {
        return PaintGpuState::DirtyFull;
    }
    if (value == "Uploading") {
        return PaintGpuState::Uploading;
    }
    if (value == "Ready") {
        return PaintGpuState::Ready;
    }
    if (value == "Error") {
        return PaintGpuState::Error;
    }
    return PaintGpuState::Idle;
}

struct PaintGpuStats {
    std::uint64_t uploads_total = 0;
    std::uint64_t partial_uploads = 0;
    std::uint64_t full_uploads = 0;
    std::uint64_t failures_total = 0;
    std::uint64_t last_upload_bytes = 0;
    std::uint64_t last_upload_duration_ns = 0;
    std::uint64_t last_revision = 0;
};

struct PaintTexturePayload {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint64_t revision = 0;
    std::vector<std::uint8_t> pixels;
};

struct PaintDirtyBatch {
    std::vector<SP::UI::Runtime::DirtyRectHint> rects;
};

} // namespace SP::UI::Declarative
