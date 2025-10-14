#pragma once

#include "core/Error.hpp"
#include "path/ConcretePath.hpp"
#include "path/UnvalidatedPath.hpp"

#include <string>
#include <string_view>

namespace SP::App {

using AppRootPath = SP::ConcretePathString;
using AppRootPathView = SP::ConcretePathStringView;
using ConcretePath = SP::ConcretePathString;
using ConcretePathView = SP::ConcretePathStringView;

/**
 * Helpers for working with application roots and app-relative paths.
 *
 * Path expectations:
 * - Application roots are absolute, canonical paths without a trailing slash.
 * - App-relative paths have no leading slash and do not contain "." or ".." components.
 * - All returned paths are absolute and canonical.
 */

auto normalize_app_root(AppRootPathView root) -> SP::Expected<AppRootPath>;

auto is_app_relative(SP::UnvalidatedPathView candidate) -> bool;
inline auto is_app_relative(std::string_view candidate) -> bool {
    return is_app_relative(SP::UnvalidatedPathView{candidate});
}

auto resolve_app_relative(AppRootPathView root, SP::UnvalidatedPathView maybeRelative) -> SP::Expected<ConcretePath>;
inline auto resolve_app_relative(AppRootPathView root, std::string_view maybeRelative) -> SP::Expected<ConcretePath> {
    return resolve_app_relative(root, SP::UnvalidatedPathView{maybeRelative});
}

auto ensure_within_app(AppRootPathView root, ConcretePathView absolute) -> SP::Expected<void>;

inline auto require_same_app(AppRootPathView root, ConcretePathView absolute) -> SP::Expected<void> {
    return ensure_within_app(root, absolute);
}

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> SP::Expected<ConcretePath>;

auto derive_app_root(ConcretePathView absolutePath) -> SP::Expected<AppRootPath>;

} // namespace SP::App
