#include "BuildersDetail.hpp"

#include <unordered_set>
#include <vector>

namespace SP::UI::Builders::Resources::Fonts {

using namespace Detail;

namespace {

auto make_paths(AppRootPathView appRoot,
                std::string_view family,
                std::string_view style) -> SP::Expected<FontResourcePaths> {
    if (auto status = ensure_identifier(family, "font family"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_identifier(style, "font style"); !status) {
        return std::unexpected(status.error());
    }

    auto root = SP::App::resolve_resource(appRoot, {"fonts", family, style});
    if (!root) {
        return std::unexpected(root.error());
    }

    FontResourcePaths paths{};
    paths.root = *root;
    auto const& base = root->getPath();
    paths.meta = ConcretePath{base + "/meta"};
    paths.active_revision = ConcretePath{base + "/meta/active_revision"};
    paths.builds = ConcretePath{base + "/builds"};
    paths.inbox = ConcretePath{base + "/inbox"};
    return paths;
}

} // namespace

auto Resolve(AppRootPathView appRoot,
             std::string_view family,
             std::string_view style) -> SP::Expected<FontResourcePaths> {
    return make_paths(appRoot, family, style);
}

auto Register(PathSpace& space,
              AppRootPathView appRoot,
              RegisterFontParams const& params) -> SP::Expected<FontResourcePaths> {
    auto paths = make_paths(appRoot, params.family, params.style);
    if (!paths) {
        return std::unexpected(paths.error());
    }

    auto const meta_base = std::string(paths->meta.getPath());
    auto family_path = meta_base + "/family";
    if (auto status = replace_single<std::string>(space, family_path, params.family); !status) {
        return std::unexpected(status.error());
    }

    auto style_path = meta_base + "/style";
    if (auto status = replace_single<std::string>(space, style_path, params.style); !status) {
        return std::unexpected(status.error());
    }

    auto weight = params.weight.empty() ? std::string{"400"} : params.weight;
    auto weight_path = meta_base + "/weight";
    if (auto status = replace_single<std::string>(space, weight_path, weight); !status) {
        return std::unexpected(status.error());
    }

    std::vector<std::string> sanitized_fallbacks;
    sanitized_fallbacks.reserve(params.fallback_families.size());
    std::unordered_set<std::string> seen{};
    for (auto const& entry : params.fallback_families) {
        if (entry.empty()) {
            continue;
        }
        if (entry == params.family) {
            continue;
        }
        if (seen.insert(entry).second) {
            sanitized_fallbacks.emplace_back(entry);
        }
    }
    if (sanitized_fallbacks.empty()) {
        sanitized_fallbacks.emplace_back("system-ui");
    }

    auto fallbacks_path = meta_base + "/fallbacks";
    if (auto status = replace_single<std::vector<std::string>>(space, fallbacks_path, sanitized_fallbacks); !status) {
        return std::unexpected(status.error());
    }

    auto atlas_base = meta_base + "/atlas";
    auto soft_path = atlas_base + "/softBytes";
    if (auto status = replace_single<std::uint64_t>(space, soft_path, params.atlas_soft_bytes); !status) {
        return std::unexpected(status.error());
    }

    auto hard_path = atlas_base + "/hardBytes";
    if (auto status = replace_single<std::uint64_t>(space, hard_path, params.atlas_hard_bytes); !status) {
        return std::unexpected(status.error());
    }

    auto run_bytes_path = atlas_base + "/shapedRunApproxBytes";
    if (auto status = replace_single<std::uint64_t>(space, run_bytes_path, params.shaped_run_approx_bytes); !status) {
        return std::unexpected(status.error());
    }

    auto active_path = std::string(paths->active_revision.getPath());
    if (auto status = replace_single<std::uint64_t>(space, active_path, params.initial_revision); !status) {
        return std::unexpected(status.error());
    }

    return *paths;
}

} // namespace SP::UI::Builders::Resources::Fonts
