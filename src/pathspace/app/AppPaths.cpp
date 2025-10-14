#include <pathspace/app/AppPaths.hpp>

#include "core/Error.hpp"

#include "path/UnvalidatedPath.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using SP::Error;
using SP::Expected;
using SP::UnvalidatedPathView;

constexpr auto make_path_error(std::string message) -> Error {
    return Error{Error::Code::InvalidPath, std::move(message)};
}

constexpr auto component_error(std::string message) -> Error {
    return Error{Error::Code::InvalidPathSubcomponent, std::move(message)};
}

auto build_absolute_from_components(std::span<std::string_view const> components) -> std::string {
    std::string out{"/"};
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (i > 0) {
            out.push_back('/');
        }
        out.append(components[i]);
    }
    return out;
}

} // namespace

namespace SP::App {

auto normalize_app_root(AppRootPathView root) -> Expected<AppRootPath> {
    UnvalidatedPathView raw{root.getPath()};
    auto canonical = raw.canonicalize_absolute();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }

    UnvalidatedPathView canonicalView{std::string_view{*canonical}};
    auto components = canonicalView.split_absolute_components();
    if (!components) {
        return std::unexpected(components.error());
    }
    if (components->size() < 2) {
        return std::unexpected(make_path_error("application root must contain at least two components"));
    }

    return AppRootPath{*canonical};
}

auto is_app_relative(UnvalidatedPathView candidate) -> bool {
    if (candidate.empty()) {
        return false;
    }
    if (candidate.is_absolute()) {
        return false;
    }
    return !candidate.contains_relative_tokens();
}

auto resolve_app_relative(AppRootPathView root, UnvalidatedPathView maybeRelative) -> Expected<ConcretePath> {
    if (maybeRelative.empty()) {
        return std::unexpected(make_path_error("path must not be empty"));
    }

    std::string absolute;

    if (maybeRelative.is_absolute()) {
        auto canonical = maybeRelative.canonicalize_absolute();
        if (!canonical) {
            return std::unexpected(canonical.error());
        }
        absolute = std::move(*canonical);
    } else {
        if (maybeRelative.contains_relative_tokens()) {
            return std::unexpected(component_error("relative path must not contain '.', '..', or empty components"));
        }
        absolute = std::string(root.getPath());
        absolute.push_back('/');
        absolute.append(maybeRelative.raw());
        auto canonical = UnvalidatedPathView{std::string_view{absolute}}.canonicalize_absolute();
        if (!canonical) {
            return std::unexpected(canonical.error());
        }
        absolute = std::move(*canonical);
    }

    auto ensured = ensure_within_app(root, ConcretePathView{absolute});
    if (!ensured) {
        return std::unexpected(ensured.error());
    }

    return ConcretePath{absolute};
}

auto ensure_within_app(AppRootPathView root, ConcretePathView absolute) -> Expected<void> {
    auto const& rootStr = root.getPath();
    auto const& absoluteStr = absolute.getPath();

    if (absoluteStr.size() < rootStr.size()) {
        return std::unexpected(make_path_error("path does not fall within the application root"));
    }
    if (!absoluteStr.starts_with(rootStr)) {
        return std::unexpected(make_path_error("path does not share the application root prefix"));
    }
    if (absoluteStr.size() > rootStr.size() && absoluteStr[rootStr.size()] != '/') {
        return std::unexpected(component_error("path diverges from application root on a partial component boundary"));
    }
    return {};
}

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> Expected<ConcretePath> {
    if (auto status = ensure_within_app(root, rendererPath); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_within_app(root, targetPath); !status) {
        return std::unexpected(status.error());
    }

    auto rendererComponents = UnvalidatedPathView{rendererPath.getPath()}.split_absolute_components();
    if (!rendererComponents) {
        return std::unexpected(rendererComponents.error());
    }
    auto targetComponents = UnvalidatedPathView{targetPath.getPath()}.split_absolute_components();
    if (!targetComponents) {
        return std::unexpected(targetComponents.error());
    }

    if (rendererComponents->size() >= targetComponents->size()) {
        return std::unexpected(make_path_error("target path must extend beyond the renderer path"));
    }
    if (!std::equal(rendererComponents->begin(),
                    rendererComponents->end(),
                    targetComponents->begin())) {
        return std::unexpected(make_path_error("target path must be nested under the renderer path"));
    }

    auto const baseIndex = rendererComponents->size();
    if (targetComponents->size() < baseIndex + 3) {
        return std::unexpected(make_path_error("target path must include 'targets/<kind>/<name>'"));
    }
    if ((*targetComponents)[baseIndex] != "targets") {
        return std::unexpected(make_path_error("target path must contain a 'targets' segment"));
    }

    auto const limit = baseIndex + 3;
    std::string result = build_absolute_from_components(
        std::span<std::string_view const>(*targetComponents).first(limit));
    return ConcretePath{result};
}

auto derive_app_root(ConcretePathView absolutePath) -> Expected<AppRootPath> {
    auto components = UnvalidatedPathView{absolutePath.getPath()}.split_absolute_components();
    if (!components) {
        return std::unexpected(components.error());
    }
    auto it = std::find(components->begin(), components->end(), "applications");
    if (it == components->end()) {
        return std::unexpected(make_path_error("path does not contain an applications segment"));
    }
    auto idx = static_cast<std::size_t>(std::distance(components->begin(), it));
    if (idx + 1 >= components->size()) {
        return std::unexpected(make_path_error("path missing application identifier after applications segment"));
    }

    auto count = idx + 2; // include the application name component
    std::string root = build_absolute_from_components(
        std::span<std::string_view const>(*components).first(count));
    return AppRootPath{root};
}

} // namespace SP::App
