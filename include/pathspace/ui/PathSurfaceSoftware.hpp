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

#if defined(__APPLE__)
typedef struct __IOSurface* IOSurfaceRef;
#endif

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

#if defined(__APPLE__)
    class SharedIOSurface {
    public:
        SharedIOSurface() = default;
        SharedIOSurface(IOSurfaceRef surface,
                        int width,
                        int height,
                        std::size_t row_bytes);
        SharedIOSurface(SharedIOSurface const& other);
        SharedIOSurface& operator=(SharedIOSurface const& other);
        SharedIOSurface(SharedIOSurface&& other) noexcept;
        SharedIOSurface& operator=(SharedIOSurface&& other) noexcept;
        ~SharedIOSurface();

        [[nodiscard]] auto valid() const -> bool { return surface_ != nullptr; }
        [[nodiscard]] auto surface() const -> IOSurfaceRef { return surface_; }
        [[nodiscard]] auto width() const -> int { return width_; }
        [[nodiscard]] auto height() const -> int { return height_; }
        [[nodiscard]] auto row_bytes() const -> std::size_t { return row_bytes_; }
        [[nodiscard]] auto retain_for_external_use() const -> IOSurfaceRef;

    private:
        IOSurfaceRef surface_ = nullptr;
        int width_ = 0;
        int height_ = 0;
        std::size_t row_bytes_ = 0;
    };

    [[nodiscard]] auto front_iosurface() const -> std::optional<SharedIOSurface>;
#endif

private:
    void reallocate_buffers();
    void reset_progressive();
    void mark_staging_sync_needed();
    void clear_staging_sync();
#if defined(__APPLE__)
    bool copy_front_into_locked_staging(std::uint8_t* staging_base,
                                        std::size_t staging_stride,
                                        int width,
                                        int height);
#endif

    Builders::SurfaceDesc desc_{};
    Options options_{};

    std::size_t frame_bytes_ = 0;
    std::size_t row_stride_bytes_ = 0;

    std::unique_ptr<ProgressiveSurfaceBuffer> progressive_;

#if defined(__APPLE__)
    class IOSurfaceHolder {
    public:
        IOSurfaceHolder() = default;
        explicit IOSurfaceHolder(IOSurfaceRef surface);
        IOSurfaceHolder(IOSurfaceHolder const& other) = delete;
        IOSurfaceHolder& operator=(IOSurfaceHolder const& other) = delete;
        IOSurfaceHolder(IOSurfaceHolder&& other) noexcept;
        IOSurfaceHolder& operator=(IOSurfaceHolder&& other) noexcept;
        ~IOSurfaceHolder();

        [[nodiscard]] auto get() const -> IOSurfaceRef { return surface_; }
        void reset(IOSurfaceRef surface = nullptr);
        void swap(IOSurfaceHolder& other) noexcept;

    private:
        IOSurfaceRef surface_ = nullptr;
    };

    IOSurfaceHolder staging_surface_{};
    IOSurfaceHolder front_surface_{};
    mutable bool staging_locked_ = false;
#else
    std::vector<std::uint8_t> staging_;
    std::vector<std::uint8_t> front_;
#endif
    bool staging_dirty_ = false;
    std::vector<std::size_t> progressive_dirty_tiles_;

    std::atomic<std::uint64_t> buffered_epoch_{0};
    std::atomic<std::uint64_t> buffered_frame_index_{0};
    std::atomic<std::uint64_t> buffered_revision_{0};
    std::atomic<std::uint64_t> buffered_render_ns_{0};
    bool staging_sync_pending_ = false;
};

} // namespace SP::UI
