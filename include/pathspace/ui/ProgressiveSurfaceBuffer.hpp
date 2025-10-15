#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace SP::UI {

enum class TilePass : std::uint32_t {
    None = 0,
    OpaqueInProgress = 1,
    OpaqueDone = 2,
    AlphaInProgress = 3,
    AlphaDone = 4,
};

struct TileDimensions {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct TilePixels {
    std::uint8_t* data = nullptr;
    std::size_t stride_bytes = 0;
    TileDimensions dims{};
};

struct TileCopyResult {
    TilePass pass = TilePass::None;
    std::uint64_t epoch = 0;
};

class ProgressiveSurfaceBuffer {
public:
    ProgressiveSurfaceBuffer(int width_px, int height_px, int tile_size_px);

    auto width() const -> int { return width_px_; }
    auto height() const -> int { return height_px_; }
    auto tile_size() const -> int { return tile_size_px_; }
    auto tiles_x() const -> int { return tiles_x_; }
    auto tiles_y() const -> int { return tiles_y_; }
    auto tile_count() const -> std::size_t { return metadata_.size(); }
    auto stride_bytes() const -> std::size_t { return static_cast<std::size_t>(width_px_) * 4u; }

    auto tile_dimensions(std::size_t tile_index) const -> TileDimensions;

    class TileWriter {
    public:
        TileWriter() = default;
        TileWriter(ProgressiveSurfaceBuffer& buffer, std::size_t tile_index, TilePass pass);
        TileWriter(TileWriter&& other) noexcept;
        TileWriter& operator=(TileWriter&& other) noexcept;
        TileWriter(TileWriter const&) = delete;
        TileWriter& operator=(TileWriter const&) = delete;
        ~TileWriter();

        auto pixels() -> TilePixels;
        void commit(TilePass completed_pass, std::uint64_t epoch);
        void abort();

    private:
        ProgressiveSurfaceBuffer* buffer_ = nullptr;
        std::size_t tile_index_ = 0;
        bool active_ = false;
    };

    auto begin_tile_write(std::size_t tile_index, TilePass pass) -> TileWriter;

    auto copy_tile(std::size_t tile_index, std::span<std::uint8_t> destination) const
        -> std::optional<TileCopyResult>;

private:
    struct TileMetadata {
        std::atomic<std::uint32_t> seq{0};
        std::atomic<std::uint32_t> pass{0};
        std::atomic<std::uint64_t> epoch{0};
    };

    int width_px_ = 0;
    int height_px_ = 0;
    int tile_size_px_ = 0;
    int tiles_x_ = 0;
    int tiles_y_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::vector<TileMetadata> metadata_;

    auto ensure_tile_index(std::size_t tile_index) const -> void;
    auto tile_rect(std::size_t tile_index) const -> TileDimensions;
    auto byte_offset(TileDimensions const& dims) const -> std::size_t;
};

} // namespace SP::UI
