#include "PathRenderer2DDetail.hpp"
#include "PathRenderer2DInternal.hpp"

#include <pathspace/ui/ProgressiveSurfaceBuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <span>

namespace SP::UI::PathRenderer2DDetail {

auto ensure_linear_buffer_capacity(std::vector<float>& buffer,
                                   std::size_t pixel_count) -> bool {
    auto const required = pixel_count * 4u;
    if (buffer.size() == required) {
        return false;
    }
    buffer.clear();
    if (required > 0) {
        buffer.resize(required);
    }
    return true;
}

void clear_linear_buffer_for_damage(std::vector<float>& buffer,
                                    PathRenderer2DInternal::DamageRegion const& damage,
                                    LinearPremulColor const& clear_linear,
                                    int width,
                                    int height) {
    if (buffer.empty() || width <= 0 || height <= 0 || damage.empty()) {
        return;
    }

    auto const rects = damage.rectangles();
    if (rects.empty()) {
        return;
    }

    auto const row_stride = static_cast<std::size_t>(width) * 4u;

    auto clamp_x = [width](int value) {
        return std::clamp(value, 0, width);
    };
    auto clamp_y = [height](int value) {
        return std::clamp(value, 0, height);
    };

    for (auto const& rect : rects) {
        int const min_x = clamp_x(rect.min_x);
        int const max_x = clamp_x(rect.max_x);
        if (min_x >= max_x) {
            continue;
        }
        int const min_y = clamp_y(rect.min_y);
        int const max_y = clamp_y(rect.max_y);
        if (min_y >= max_y) {
            continue;
        }

        for (int y = min_y; y < max_y; ++y) {
            auto const base = static_cast<std::size_t>(y) * row_stride;
            for (int x = min_x; x < max_x; ++x) {
                auto* dest = buffer.data() + base + static_cast<std::size_t>(x) * 4u;
                dest[0] = clear_linear.r;
                dest[1] = clear_linear.g;
                dest[2] = clear_linear.b;
                dest[3] = clear_linear.a;
            }
        }
    }
}

auto build_encode_jobs(PathRenderer2DInternal::DamageRegion const& damage,
                       ProgressiveSurfaceBuffer const* progressive_buffer,
                       std::span<std::size_t const> progressive_dirty_tiles,
                       int width,
                       int height) -> std::vector<EncodeJob> {
    std::vector<EncodeJob> jobs;
    if (width <= 0 || height <= 0) {
        return jobs;
    }

    auto clamp_x = [width](int value) {
        return std::clamp(value, 0, width);
    };
    auto clamp_y = [height](int value) {
        return std::clamp(value, 0, height);
    };

    if (progressive_buffer && !progressive_dirty_tiles.empty()) {
        jobs.reserve(progressive_dirty_tiles.size());
        for (auto tile_index : progressive_dirty_tiles) {
            auto dims = progressive_buffer->tile_dimensions(tile_index);
            if (dims.width <= 0 || dims.height <= 0) {
                continue;
            }
            EncodeJob job{
                .min_x = clamp_x(dims.x),
                .max_x = clamp_x(dims.x + dims.width),
                .start_y = clamp_y(dims.y),
                .end_y = clamp_y(dims.y + dims.height),
            };
            if (!job.empty()) {
                jobs.push_back(job);
            }
        }
        return jobs;
    }

    constexpr int kEncodeRowChunk = 64;
    auto const rects = damage.rectangles();
    jobs.reserve(rects.size());
    for (auto const& rect : rects) {
        int const min_x = clamp_x(rect.min_x);
        int const max_x = clamp_x(rect.max_x);
        if (max_x <= min_x) {
            continue;
        }

        int const start_y = clamp_y(rect.min_y);
        int const end_y = clamp_y(rect.max_y);
        if (end_y <= start_y) {
            continue;
        }

        for (int row = start_y; row < end_y; row += kEncodeRowChunk) {
            int const chunk_end = std::min(row + kEncodeRowChunk, end_y);
            EncodeJob job{
                .min_x = min_x,
                .max_x = max_x,
                .start_y = row,
                .end_y = chunk_end,
            };
            if (!job.empty()) {
                jobs.push_back(job);
            }
        }
    }

    return jobs;
}

} // namespace SP::UI::PathRenderer2DDetail
