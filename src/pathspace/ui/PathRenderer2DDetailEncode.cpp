#include "PathRenderer2DDetail.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace SP::UI::PathRenderer2DDetail {
namespace {

void encode_rows(EncodeJob const& job, EncodeContext const& ctx) {
    if (job.empty() || !ctx.staging || !ctx.linear || !ctx.desc || ctx.width <= 0 || ctx.height <= 0) {
        return;
    }
    auto const& desc = *ctx.desc;
    auto const width_u = static_cast<std::size_t>(ctx.width);
    for (int row = job.start_y; row < job.end_y; ++row) {
        if (row < 0 || row >= ctx.height) {
            continue;
        }
        auto* row_ptr = ctx.staging + static_cast<std::size_t>(row) * ctx.row_stride_bytes;
        auto const* linear_row = ctx.linear + static_cast<std::size_t>(row) * width_u * 4u;
        for (int col = job.min_x; col < job.max_x; ++col) {
            if (col < 0 || col >= ctx.width) {
                continue;
            }
            auto pixel_index = static_cast<std::size_t>(col) * 4u;
            auto encoded = encode_pixel(linear_row + pixel_index, desc, ctx.encode_srgb);
            auto offset = static_cast<std::size_t>(col) * 4u;
            if (ctx.is_bgra) {
                row_ptr[offset + 0] = encoded[2];
                row_ptr[offset + 1] = encoded[1];
                row_ptr[offset + 2] = encoded[0];
            } else {
                row_ptr[offset + 0] = encoded[0];
                row_ptr[offset + 1] = encoded[1];
                row_ptr[offset + 2] = encoded[2];
            }
            row_ptr[offset + 3] = encoded[3];
        }
    }
}

} // namespace

auto run_encode_jobs(std::span<EncodeJob const> jobs, EncodeContext const& ctx) -> EncodeRunStats {
    EncodeRunStats stats{};
    stats.jobs = jobs.size();
    if (jobs.empty()) {
        return stats;
    }

    auto const hardware = std::max(1u, std::thread::hardware_concurrency());
    std::size_t worker_count = std::min<std::size_t>(jobs.size(),
                                                     static_cast<std::size_t>(hardware));
    constexpr std::size_t kMinJobsPerWorker = 4;
    if (worker_count <= 1 || (jobs.size() / worker_count) < kMinJobsPerWorker) {
        for (auto const& job : jobs) {
            encode_rows(job, ctx);
        }
        stats.workers_used = 1;
        return stats;
    }

    std::atomic<std::size_t> next{0};
    std::exception_ptr error;
    std::mutex error_mutex;

    auto worker = [&]() {
        try {
            while (true) {
                auto idx = next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= jobs.size()) {
                    break;
                }
                encode_rows(jobs[idx], ctx);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock{error_mutex};
            if (!error) {
                error = std::current_exception();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (error) {
        std::rethrow_exception(error);
    }

    stats.workers_used = worker_count;
    return stats;
}

} // namespace SP::UI::PathRenderer2DDetail
