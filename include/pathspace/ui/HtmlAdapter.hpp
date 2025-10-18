#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SP::UI::Html {

struct EmitOptions {
    bool prefer_dom = true;
    bool allow_clip_path = true;
    std::size_t max_dom_nodes = 10'000;
    bool allow_canvas_fallback = true;
};

struct Asset {
    std::string logical_path;
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
};

struct EmitResult {
    std::string dom;
    std::string css;
    std::string canvas_commands;
    bool used_canvas_fallback = false;
    std::vector<Asset> assets;
};

class Adapter {
public:
    Adapter() = default;
    ~Adapter() = default;

    Adapter(Adapter const&) = delete;
    Adapter& operator=(Adapter const&) = delete;
    Adapter(Adapter&&) noexcept = delete;
    Adapter& operator=(Adapter&&) noexcept = delete;

    [[nodiscard]] auto emit(Scene::DrawableBucketSnapshot const& snapshot,
                            EmitOptions const& options) -> SP::Expected<EmitResult>;
};

} // namespace SP::UI::Html
