#pragma once

#include <pathspace/core/Error.hpp>
#include <pathspace/PathSpace.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SP::UI {

class ImageCache {
public:
    struct ImageData {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        // Straight (non-premultiplied) linear RGBA floats in row-major order.
        std::vector<float> pixels; // size = width * height * 4
    };

    ImageCache() = default;

    auto load(PathSpace& space,
              std::string const& image_path,
              std::uint64_t fingerprint) -> SP::Expected<std::shared_ptr<ImageData const>>;

    void clear();

private:
    SP::Expected<std::shared_ptr<ImageData const>> decode_png(std::vector<std::uint8_t> const& png_bytes) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<ImageData const>> cache_;
};

} // namespace SP::UI
