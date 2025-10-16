#pragma once

#include <pathspace/ui/PathSurfaceSoftware.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SP::UI {

enum class PathWindowPresentMode {
    AlwaysFresh,
    PreferLatestCompleteWithBudget,
    AlwaysLatestComplete,
};

struct PathWindowPresentPolicy {
    PathWindowPresentMode mode = PathWindowPresentMode::PreferLatestCompleteWithBudget;
    std::chrono::milliseconds staleness_budget{8};
    std::uint32_t max_age_frames = 1;
    std::chrono::milliseconds frame_timeout{20};
};

struct PathWindowPresentRequest {
    std::chrono::steady_clock::time_point now{};
    std::chrono::steady_clock::time_point vsync_deadline{};
    std::span<std::uint8_t> framebuffer{};
    std::span<std::size_t const> dirty_tiles{};
};

struct PathWindowPresentStats {
    bool presented = false;
    bool skipped = false;
    bool buffered_frame_consumed = false;
    bool used_progressive = false;
    PathSurfaceSoftware::FrameInfo frame{};
    double wait_budget_ms = 0.0;
    double present_ms = 0.0;
    std::size_t progressive_tiles_copied = 0;
    std::size_t progressive_rects_coalesced = 0;
    std::size_t progressive_skip_seq_odd = 0;
    std::size_t progressive_recopy_after_seq_change = 0;
    std::string error;
};

class PathWindowView {
public:
    using PresentMode = PathWindowPresentMode;
    using PresentPolicy = PathWindowPresentPolicy;
    using PresentRequest = PathWindowPresentRequest;
    using PresentStats = PathWindowPresentStats;

    [[nodiscard]] auto present(PathSurfaceSoftware& surface,
                               PresentPolicy const& policy,
                               PresentRequest const& request) -> PresentStats;
};

} // namespace SP::UI
