#include "BuildersDetail.hpp"

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
    paths.manifest = ConcretePath{base + "/manifest.json"};
    paths.active_revision = ConcretePath{base + "/active"};
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

    auto const meta_base = std::string(paths->root.getPath()) + "/meta";
    auto family_path = meta_base + "/family";
    if (auto status = replace_single<std::string>(space, family_path, params.family); !status) {
        return std::unexpected(status.error());
    }

    auto style_path = meta_base + "/style";
    if (auto status = replace_single<std::string>(space, style_path, params.style); !status) {
        return std::unexpected(status.error());
    }

    auto active_path = std::string(paths->active_revision.getPath());
    if (auto status = replace_single<std::uint64_t>(space, active_path, 0ull); !status) {
        return std::unexpected(status.error());
    }

    if (params.manifest_json) {
        auto manifest_path = std::string(paths->manifest.getPath());
        if (auto status = replace_single<std::string>(space, manifest_path, *params.manifest_json); !status) {
            return std::unexpected(status.error());
        }
    }

    return *paths;
}

} // namespace SP::UI::Builders::Resources::Fonts

