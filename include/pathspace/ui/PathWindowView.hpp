#pragma once

#include <pathspace/ui/PathSurfaceSoftware.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SP::UI {

class PathWindowView {
public:
    enum class PresentMode {
        AlwaysFresh,
        PreferLatestCompleteWithBudget,
        AlwaysLatestComplete,
    };

    struct PresentPolicy {
        PresentMode mode = PresentMode::PreferLatestCompleteWithBudget;
        std::chrono::milliseconds staleness_budget{8};
        std::uint32_t max_age_frames = 1;
        std::chrono::milliseconds frame_timeout{20};
    };

    struct PresentRequest {
        std::chrono::steady_clock::time_point now{};
        std::chrono::steady_clock::time_point vsync_deadline{};
        std::span<std::uint8_t> framebuffer{};
        std::span<std::size_t const> dirty_tiles{};
    };

    struct PresentStats {
        bool presented = false;
        bool skipped = false;
        bool buffered_frame_consumed = false;
        bool used_progressive = false;
        PathSurfaceSoftware::FrameInfo frame{};
        double wait_budget_ms = 0.0;
        std::size_t progressive_tiles_copied = 0;
        std::string error;
    };

    [[nodiscard]] auto present(PathSurfaceSoftware& surface,
                               PresentPolicy const& policy,
                               PresentRequest const& request) -> PresentStats;
};

} // namespace SP::UI
