#include <pathspace/ui/declarative/WidgetCapsule.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace SP::UI::Declarative {

namespace {

[[nodiscard]] auto env_flag(char const* name, bool default_value) -> bool {
    if (auto* raw = std::getenv(name)) {
        std::string_view view{raw};
        std::string lowered;
        lowered.reserve(view.size());
        for (auto ch : view) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no") {
            return false;
        }
        return true;
    }
    return default_value;
}

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

auto fnv_mix(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        auto byte = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

auto fnv_span(std::uint64_t hash, std::span<const std::uint8_t> bytes) -> std::uint64_t {
    for (auto byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] auto compute_bounds(SP::UI::Scene::DrawableBucketSnapshot const& bucket)
    -> std::optional<WidgetSurface> {
    if (bucket.bounds_boxes.empty()) {
        return std::nullopt;
    }

    auto min_x = std::numeric_limits<float>::max();
    auto min_y = std::numeric_limits<float>::max();
    auto max_x = std::numeric_limits<float>::lowest();
    auto max_y = std::numeric_limits<float>::lowest();
    bool has_bounds = false;

    auto valid_size = bucket.bounds_box_valid.size();
    for (std::size_t i = 0; i < bucket.bounds_boxes.size(); ++i) {
        bool valid = i < valid_size ? bucket.bounds_box_valid[i] != 0 : true;
        if (!valid) {
            continue;
        }
        auto const& box = bucket.bounds_boxes[i];
        min_x = std::min(min_x, box.min[0]);
        min_y = std::min(min_y, box.min[1]);
        max_x = std::max(max_x, box.max[0]);
        max_y = std::max(max_y, box.max[1]);
        has_bounds = true;
    }

    if (!has_bounds) {
        return std::nullopt;
    }

    auto width = std::max(0.0f, max_x - min_x);
    auto height = std::max(0.0f, max_y - min_y);

    WidgetSurface surface{};
    surface.kind = WidgetSurfaceKind::Software;
    surface.flags = WidgetSurfaceFlags::AlphaPremultiplied;
    surface.width = static_cast<std::uint32_t>(width);
    surface.height = static_cast<std::uint32_t>(height);
    surface.logical_bounds = {min_x, min_y, max_x, max_y};
    return surface;
}

[[nodiscard]] auto fallback_dirty_rect(std::optional<WidgetSurface> const& surface)
    -> SP::UI::Runtime::DirtyRectHint {
    if (!surface) {
        return {};
    }
    return SP::UI::Runtime::DirtyRectHint{
        surface->logical_bounds[0],
        surface->logical_bounds[1],
        surface->logical_bounds[2],
        surface->logical_bounds[3],
    };
}

} // namespace

auto CapsulesFeatureEnabled() -> bool {
    return env_flag("PATHSPACE_WIDGET_CAPSULES", true);
}

auto CapsulesOnlyRuntimeEnabled() -> bool {
    return env_flag("PATHSPACE_WIDGET_CAPSULES_ONLY", true) && CapsulesFeatureEnabled();
}

auto LoadWidgetCapsule(PathSpace& space,
                       std::string const& widget_root,
                       WidgetKind kind) -> SP::Expected<WidgetCapsule> {
    auto lambda = space.read<std::string, std::string>(
        SP::UI::Runtime::Widgets::WidgetSpacePath(widget_root, "/capsule/render/lambda"));
    if (!lambda) {
        return std::unexpected(lambda.error());
    }

    auto revision = space.read<std::uint64_t, std::string>(
        SP::UI::Runtime::Widgets::WidgetSpacePath(widget_root, "/render/dirty_version"));

    WidgetCapsule capsule{};
    capsule.kind = kind;
    capsule.widget_path = widget_root;
    capsule.render_lambda = *lambda;
    capsule.render_revision = revision.value_or(0);
    return capsule;
}

auto BuildRenderPackageFromBucket(WidgetCapsule const& capsule,
                                  SP::UI::Scene::DrawableBucketSnapshot const& bucket,
                                  std::vector<SP::UI::Runtime::DirtyRectHint> const& pending_dirty,
                                  std::uint64_t render_sequence) -> WidgetRenderPackage {
    WidgetRenderPackage package{};
    package.capsule_revision = capsule.render_revision;
    package.render_sequence = render_sequence;

    package.command_kinds = bucket.command_kinds;
    package.command_payload = bucket.command_payload;
    package.texture_fingerprints = bucket.drawable_fingerprints;

    auto surface = compute_bounds(bucket);
    if (surface) {
        package.surfaces.push_back(*surface);
    }

    if (!pending_dirty.empty()) {
        package.dirty_rect = pending_dirty.front();
    } else {
        package.dirty_rect = fallback_dirty_rect(surface);
    }

    auto hash = kFnvOffset;
    hash = fnv_mix(hash, package.capsule_revision);
    hash = fnv_mix(hash, package.render_sequence);
    if (surface) {
        for (auto value : surface->logical_bounds) {
            hash = fnv_mix(hash, static_cast<std::uint64_t>(std::lround(value * 1000.0f)));
        }
        hash = fnv_mix(hash, static_cast<std::uint64_t>(surface->width));
        hash = fnv_mix(hash, static_cast<std::uint64_t>(surface->height));
    }
    hash = fnv_span(hash, std::span<const std::uint8_t>{package.command_payload.data(),
                                                        package.command_payload.size()});
    auto kinds_bytes = std::span{reinterpret_cast<std::uint8_t const*>(package.command_kinds.data()),
                                 package.command_kinds.size() * sizeof(std::uint32_t)};
    hash = fnv_span(hash, kinds_bytes);
    for (auto texture_fp : package.texture_fingerprints) {
        hash = fnv_mix(hash, texture_fp);
    }
    package.content_hash = hash;

    if (!package.surfaces.empty()) {
        package.surfaces.front().fingerprint = hash;
    }

    return package;
}

} // namespace SP::UI::Declarative
