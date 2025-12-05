#include "RuntimeDetail.hpp"

namespace SP::UI::Runtime::Surface {

using namespace Detail;

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             SurfaceParams const& params) -> SP::Expected<SurfacePath> {
    if (auto status = ensure_identifier(params.name, "surface name"); !status) {
        return std::unexpected(status.error());
    }

    auto surfacePath = combine_relative(appRoot, std::string("surfaces/") + params.name);
    if (!surfacePath) {
        return std::unexpected(surfacePath.error());
    }

    auto rendererPath = resolve_renderer_spec(appRoot, params.renderer);
    if (!rendererPath) {
        return std::unexpected(rendererPath.error());
    }

    if (auto status = ensure_contains_segment(ConcretePathView{surfacePath->getPath()}, kSurfacesSegment); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_contains_segment(ConcretePathView{rendererPath->getPath()}, kRenderersSegment); !status) {
        return std::unexpected(status.error());
    }

    auto metaBase = std::string(surfacePath->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return SurfacePath{surfacePath->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }

    auto descPath = std::string(surfacePath->getPath()) + "/desc";
    if (auto status = store_desc(space, descPath, params.desc); !status) {
        return std::unexpected(status.error());
    }

    auto rendererRelative = relative_to_root(appRoot, ConcretePathView{rendererPath->getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    auto rendererField = std::string(surfacePath->getPath()) + "/renderer";
    if (auto status = replace_single<std::string>(space, rendererField, *rendererRelative); !status) {
        return std::unexpected(status.error());
    }

    auto targetSpec = std::string("targets/surfaces/") + params.name;
    auto targetBase = Renderer::ResolveTargetBase(space, appRoot, *rendererPath, targetSpec);
    if (!targetBase) {
        return std::unexpected(targetBase.error());
    }

    auto targetRelative = relative_to_root(appRoot, ConcretePathView{targetBase->getPath()});
    if (!targetRelative) {
        return std::unexpected(targetRelative.error());
    }

    if (auto status = store_desc(space, std::string(targetBase->getPath()) + "/desc", params.desc); !status) {
        return std::unexpected(status.error());
    }

    auto targetField = std::string(surfacePath->getPath()) + "/target";
    if (auto status = replace_single<std::string>(space, targetField, *targetRelative); !status) {
        return std::unexpected(status.error());
    }

    return SurfacePath{surfacePath->getPath()};
}

auto SetScene(PathSpace& space,
               SurfacePath const& surfacePath,
               ScenePath const& scenePath) -> SP::Expected<void> {
    auto surfaceRoot = derive_app_root_for(ConcretePathView{surfacePath.getPath()});
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }
    auto sceneRoot = derive_app_root_for(ConcretePathView{scenePath.getPath()});
    if (!sceneRoot) {
        return std::unexpected(sceneRoot.error());
    }
    if (surfaceRoot->getPath() != sceneRoot->getPath()) {
        return std::unexpected(make_error("surface and scene belong to different applications",
                                          SP::Error::Code::InvalidPath));
    }

    auto sceneRelative = relative_to_root(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                          ConcretePathView{scenePath.getPath()});
    if (!sceneRelative) {
        return std::unexpected(sceneRelative.error());
    }

    auto sceneField = std::string(surfacePath.getPath()) + "/scene";
    if (auto status = replace_single<std::string>(space, sceneField, *sceneRelative); !status) {
        return status;
    }

    auto targetField = std::string(surfacePath.getPath()) + "/target";
    auto targetRelative = read_value<std::string>(space, targetField);
    if (!targetRelative) {
        if (targetRelative.error().code == SP::Error::Code::NoObjectFound) {
            return std::unexpected(make_error("surface missing target binding",
                                              SP::Error::Code::InvalidPath));
        }
        return std::unexpected(targetRelative.error());
    }

    auto targetAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                                        *targetRelative);
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto targetScenePath = targetAbsolute->getPath() + "/scene";
    return replace_single<std::string>(space, targetScenePath, *sceneRelative);
}

auto RenderOnce(PathSpace& space,
                 SurfacePath const& surfacePath,
                 std::optional<RenderSettings> settingsOverride) -> SP::Expected<SP::FutureAny> {
    auto context = prepare_surface_render_context(space, surfacePath, settingsOverride);
    if (!context) {
        return std::unexpected(context.error());
    }

    auto target_key = std::string(context->target_path.getPath());
    if (auto watch = ensure_surface_cache_watch(space, target_key); !watch) {
        return std::unexpected(watch.error());
    }
    auto& surface = acquire_surface(target_key, context->target_desc);
#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    if (context->renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(target_key, context->target_desc);
    }
    auto stats = render_into_target(space,
                                    *context,
                                    surface,
                                    metal_surface);
#else
    auto stats = render_into_target(space, *context, surface);
#endif
    if (!stats) {
        return std::unexpected(stats.error());
    }

    auto state = std::make_shared<SP::SharedState<bool>>();
    state->set_value(true);
    return SP::FutureT<bool>{state}.to_any();
}

} // namespace SP::UI::Runtime::Surface
