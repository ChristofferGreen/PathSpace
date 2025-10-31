#include <pathspace/ui/ImageCache.hpp>

#include <pathspace/core/Error.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include <third_party/stb_image.h>

namespace SP::UI {

namespace {

auto srgb_to_linear(float value) -> float {
    value = std::clamp(value, 0.0f, 1.0f);
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

auto make_decode_error(std::string message) -> SP::Error {
    return SP::Error{SP::Error::Code::InvalidType, std::move(message)};
}

auto fingerprint_hex(std::uint64_t fingerprint) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << std::nouppercase << fingerprint;
    return oss.str();
}

auto canonical_image_path(std::string const& primary_path,
                          std::uint64_t fingerprint) -> std::optional<std::string> {
    constexpr std::string_view kBuilds{"/builds/"};
    auto pos = primary_path.find(kBuilds);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::string base = primary_path.substr(0, pos);
    base += "/assets/images/";
    base += fingerprint_hex(fingerprint);
    base += ".png";
    return base;
}

} // namespace

auto ImageCache::load(PathSpace& space,
                      std::string const& image_path,
                      std::uint64_t fingerprint) -> SP::Expected<std::shared_ptr<ImageData const>> {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(fingerprint);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    auto bytes = space.read<std::vector<std::uint8_t>>(image_path);
    if (!bytes) {
        auto const& error = bytes.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            if (auto fallback = canonical_image_path(image_path, fingerprint)) {
                auto fallback_bytes = space.read<std::vector<std::uint8_t>>(*fallback);
                if (fallback_bytes) {
                    bytes = std::move(fallback_bytes);
                } else {
                    bytes = std::unexpected(fallback_bytes.error());
                }
            }
        }
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
    }

    auto decoded = decode_png(*bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    auto shared = std::move(*decoded);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.insert_or_assign(fingerprint, shared);
    }
    return shared;
}

void ImageCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

auto ImageCache::resident_bytes() const -> std::size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (auto const& entry : cache_) {
        auto const& image = entry.second;
        if (image) {
            total += image->pixels.size() * sizeof(float);
        }
    }
    return total;
}

auto ImageCache::decode_png(std::vector<std::uint8_t> const& png_bytes) const -> SP::Expected<std::shared_ptr<ImageData const>> {
    int width = 0;
    int height = 0;
    int channels = 0;

    unsigned char* decoded = stbi_load_from_memory(
        png_bytes.data(),
        static_cast<int>(png_bytes.size()),
        &width,
        &height,
        &channels,
        STBI_rgb_alpha);

    if (!decoded || width <= 0 || height <= 0) {
        if (decoded) {
            stbi_image_free(decoded);
        }
        return std::unexpected(make_decode_error("failed to decode png image"));
    }

    std::unique_ptr<unsigned char, void(*)(void*)> pixels(decoded, stbi_image_free);
    auto image = std::make_shared<ImageData>();
    image->width = static_cast<std::uint32_t>(width);
    image->height = static_cast<std::uint32_t>(height);
    image->pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);

    for (std::size_t i = 0; i < image->pixels.size() / 4u; ++i) {
        auto r = static_cast<float>(pixels.get()[i * 4u + 0]) / 255.0f;
        auto g = static_cast<float>(pixels.get()[i * 4u + 1]) / 255.0f;
        auto b = static_cast<float>(pixels.get()[i * 4u + 2]) / 255.0f;
        auto a = static_cast<float>(pixels.get()[i * 4u + 3]) / 255.0f;

        image->pixels[i * 4u + 0] = srgb_to_linear(r);
        image->pixels[i * 4u + 1] = srgb_to_linear(g);
        image->pixels[i * 4u + 2] = srgb_to_linear(b);
        image->pixels[i * 4u + 3] = std::clamp(a, 0.0f, 1.0f);
    }

    return image;
}

} // namespace SP::UI
