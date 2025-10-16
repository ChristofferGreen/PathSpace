#pragma once

#include <pathspace/ui/ProgressiveSurfaceBuffer.hpp>
#include <pathspace/ui/SurfaceTypes.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace SP::UI {

class PathSurfaceSoftware {
public:
    struct Options {
        bool enable_progressive = true;
        bool enable_buffered = true;
        int progressive_tile_size_px = 64;
    };

    struct FrameInfo {
        std::uint64_t frame_index = 0;
        std::uint64_t revision = 0;
        double render_ms = 0.0;
    };

    struct BufferedFrameCopy {
        FrameInfo info{};
    };

    PathSurfaceSoftware(Builders::SurfaceDesc desc);
    PathSurfaceSoftware(Builders::SurfaceDesc desc, Options options);

    auto desc() const -> Builders::SurfaceDesc const& { return desc_; }
    auto options() const -> Options const& { return options_; }

    void resize(Builders::SurfaceDesc const& desc);

    [[nodiscard]] auto frame_bytes() const -> std::size_t { return frame_bytes_; }
    [[nodiscard]] auto row_stride_bytes() const -> std::size_t { return row_stride_bytes_; }

    [[nodiscard]] auto has_progressive() const -> bool { return static_cast<bool>(progressive_); }
    [[nodiscard]] auto progressive_buffer() -> ProgressiveSurfaceBuffer&;
    [[nodiscard]] auto progressive_buffer() const -> ProgressiveSurfaceBuffer const&;
    [[nodiscard]] auto begin_progressive_tile(std::size_t tile_index, TilePass pass)
        -> ProgressiveSurfaceBuffer::TileWriter;

    [[nodiscard]] auto has_buffered() const -> bool { return options_.enable_buffered && frame_bytes_ > 0; }
    [[nodiscard]] auto staging_span() -> std::span<std::uint8_t>;
    void publish_buffered_frame(FrameInfo info);
    void discard_staging();

    void record_frame_info(FrameInfo info);
    [[nodiscard]] auto latest_frame_info() const -> FrameInfo;

    void mark_progressive_dirty(std::size_t tile_index);
    [[nodiscard]] auto progressive_tile_count() const -> std::size_t;
    auto consume_progressive_dirty_tiles() -> std::vector<std::size_t>;

    [[nodiscard]] auto copy_buffered_frame(std::span<std::uint8_t> destination) const
        -> std::optional<BufferedFrameCopy>;

private:
    void reallocate_buffers();
    void reset_progressive();

    Builders::SurfaceDesc desc_{};
    Options options_{};

    std::size_t frame_bytes_ = 0;
    std::size_t row_stride_bytes_ = 0;

    std::unique_ptr<ProgressiveSurfaceBuffer> progressive_;

    std::vector<std::uint8_t> staging_;
    std::vector<std::uint8_t> front_;
    bool staging_dirty_ = false;
    std::vector<std::size_t> progressive_dirty_tiles_;

    std::atomic<std::uint64_t> buffered_epoch_{0};
    std::atomic<std::uint64_t> buffered_frame_index_{0};
    std::atomic<std::uint64_t> buffered_revision_{0};
    std::atomic<std::uint64_t> buffered_render_ns_{0};
};

} // namespace SP::UI
