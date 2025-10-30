#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/FontAtlas.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SP::UI {

class FontAtlasCache {
public:
    auto load(PathSpace& space,
              std::string const& atlas_path,
              std::uint64_t fingerprint) -> SP::Expected<std::shared_ptr<FontAtlasData const>>;

    void clear();
    auto resident_bytes() const -> std::size_t;

private:
    auto decode(std::vector<std::uint8_t> const& bytes) const
        -> SP::Expected<std::shared_ptr<FontAtlasData const>>;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<FontAtlasData const>> cache_;
};

} // namespace SP::UI

