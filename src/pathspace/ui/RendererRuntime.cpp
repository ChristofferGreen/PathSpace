#include "RuntimeDetail.hpp"

namespace SP::UI::Runtime::Renderer {

using namespace Detail;

auto Create(PathSpace& space,
            AppRootPathView appRoot,
            RendererParams const& params) -> SP::Expected<RendererPath> {
    if (auto status = ensure_identifier(params.name, "renderer name"); !status) {
        return std::unexpected(status.error());
    }

    auto resolved = combine_relative(appRoot, std::string("renderers/") + params.name);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto metaBase = std::string(resolved->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        auto ensureDescription = read_optional<std::string>(space, metaBase + "/description");
        if (!ensureDescription) {
            return std::unexpected(ensureDescription.error());
        }
        if (!ensureDescription->has_value()) {
            auto descStatus = replace_single<std::string>(space, metaBase + "/description", params.description);
            if (!descStatus) {
                return std::unexpected(descStatus.error());
            }
        }

        auto kindStatus = store_renderer_kind(space, metaBase + "/kind", params.kind);
        if (!kindStatus) {
            return std::unexpected(kindStatus.error());
        }

        return RendererPath{resolved->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/description", params.description); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = store_renderer_kind(space, metaBase + "/kind", params.kind); !status) {
        return std::unexpected(status.error());
    }

    return RendererPath{resolved->getPath()};
}

auto CreateHtmlTarget(PathSpace& space,
                      AppRootPathView appRoot,
                      RendererPath const& rendererPath,
                      HtmlTargetParams const& params) -> SP::Expected<HtmlTargetPath> {
    if (auto status = ensure_identifier(params.name, "html target name"); !status) {
        return std::unexpected(status.error());
    }
    if (params.scene.empty()) {
        return std::unexpected(make_error("html target scene must not be empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto rendererRoot = derive_app_root_for(ConcretePathView{rendererPath.getPath()});
    if (!rendererRoot) {
        return std::unexpected(rendererRoot.error());
    }

    auto sceneAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{rendererRoot->getPath()},
                                                       params.scene);
    if (!sceneAbsolute) {
        return std::unexpected(sceneAbsolute.error());
    }

    if (auto status = same_app(ConcretePathView{sceneAbsolute->getPath()},
                               ConcretePathView{rendererPath.getPath()}); !status) {
        return std::unexpected(status.error());
    }

    auto rendererView = SP::App::AppRootPathView{rendererRoot->getPath()};
    auto rendererRelative = relative_to_root(rendererView, ConcretePathView{rendererPath.getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    std::string targetRelative = *rendererRelative;
    if (!targetRelative.empty()) {
        targetRelative.push_back('/');
    }
    targetRelative.append("targets/html/");
    targetRelative.append(params.name);

    auto targetAbsolute = combine_relative(rendererView, std::move(targetRelative));
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto base = targetAbsolute->getPath();
    if (auto status = replace_single<HtmlTargetDesc>(space, base + "/desc", params.desc); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, base + "/scene", params.scene); !status) {
        return std::unexpected(status.error());
    }

    return HtmlTargetPath{base};
}

auto ResolveTargetBase(PathSpace const& space,
                        AppRootPathView appRoot,
                        RendererPath const& rendererPath,
                        std::string_view targetSpec) -> SP::Expected<ConcretePath> {
    if (auto status = ensure_non_empty(targetSpec, "target spec"); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = SP::App::ensure_within_app(appRoot, ConcretePathView{rendererPath.getPath()}); !status) {
        return std::unexpected(status.error());
    }

    std::string spec{targetSpec};
    if (!spec.empty() && spec.front() == '/') {
        auto resolved = combine_relative(appRoot, std::move(spec));
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        return *resolved;
    }

    auto rendererRelative = relative_to_root(appRoot, ConcretePathView{rendererPath.getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    std::string combined = *rendererRelative;
    if (!combined.empty()) {
        combined.push_back('/');
    }
    combined.append(spec);

    auto resolved = combine_relative(appRoot, std::move(combined));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return *resolved;
}

auto UpdateSettings(PathSpace& space,
                     ConcretePathView targetPath,
                     RenderSettings const& settings) -> SP::Expected<void> {
    auto settingsPath = std::string(targetPath.getPath()) + "/settings";
    return replace_single<RenderSettings>(space, settingsPath, settings);
}

auto ReadSettings(PathSpace const& space,
                   ConcretePathView targetPath) -> SP::Expected<RenderSettings> {
    auto settingsPath = std::string(targetPath.getPath()) + "/settings";
    return read_value<RenderSettings>(space, settingsPath);
}

auto rectangles_touch_or_overlap(DirtyRectHint const& a, DirtyRectHint const& b) -> bool {
    auto overlaps_axis = [](float min_a, float max_a, float min_b, float max_b) {
        return !(max_a < min_b || min_a > max_b);
    };
    return overlaps_axis(a.min_x, a.max_x, b.min_x, b.max_x)
        && overlaps_axis(a.min_y, a.max_y, b.min_y, b.max_y);
}

auto merge_hints(std::vector<DirtyRectHint>& hints,
                 float tile_size,
                 float width,
                 float height) -> void {
    if (hints.empty()) {
        return;
    }
    auto fallback_to_full_surface = [&]() {
        hints.clear();
        if (width <= 0.0f || height <= 0.0f) {
            return;
        }
        hints.push_back(DirtyRectHint{
            .min_x = 0.0f,
            .min_y = 0.0f,
            .max_x = width,
            .max_y = height,
        });
    };
    if (width <= 0.0f || height <= 0.0f) {
        hints.clear();
        return;
    }
    bool merged_any = true;
    while (merged_any) {
        merged_any = false;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            for (std::size_t j = i + 1; j < hints.size(); ++j) {
                if (rectangles_touch_or_overlap(hints[i], hints[j])) {
                    hints[i].min_x = std::min(hints[i].min_x, hints[j].min_x);
                    hints[i].min_y = std::min(hints[i].min_y, hints[j].min_y);
                    hints[i].max_x = std::max(hints[i].max_x, hints[j].max_x);
                    hints[i].max_y = std::max(hints[i].max_y, hints[j].max_y);
                    hints.erase(hints.begin() + static_cast<std::ptrdiff_t>(j));
                    merged_any = true;
                    break;
                }
            }
            if (merged_any) {
                break;
            }
        }
    }
    constexpr std::size_t kMaxStoredHints = 128;
    if (hints.size() > kMaxStoredHints) {
        fallback_to_full_surface();
        return;
    }
    double total_area = 0.0;
    for (auto const& rect : hints) {
        auto const width_px = std::max(0.0f, rect.max_x - rect.min_x);
        auto const height_px = std::max(0.0f, rect.max_y - rect.min_y);
        total_area += static_cast<double>(width_px) * static_cast<double>(height_px);
    }
    auto const surface_area = static_cast<double>(width) * static_cast<double>(height);
    if (surface_area > 0.0 && total_area >= surface_area * 0.9) {
        fallback_to_full_surface();
        return;
    }

    auto approximately = [tile_size](float a, float b) {
        auto const epsilon = std::max(tile_size * 0.001f, 1e-5f);
        return std::fabs(a - b) <= epsilon;
    };

    for (auto& rect : hints) {
        if (approximately(rect.min_x, 0.0f)) {
            rect.min_x = 0.0f;
        }
        if (approximately(rect.min_y, 0.0f)) {
            rect.min_y = 0.0f;
        }
        if (approximately(rect.max_x, width)) {
            rect.max_x = width;
        }
        if (approximately(rect.max_y, height)) {
            rect.max_y = height;
        }
    }
    std::sort(hints.begin(),
              hints.end(),
              [](DirtyRectHint const& lhs, DirtyRectHint const& rhs) {
                  if (lhs.min_y == rhs.min_y) {
                      return lhs.min_x < rhs.min_x;
                  }
                  return lhs.min_y < rhs.min_y;
              });
}

auto snap_hint_to_tiles(DirtyRectHint hint, float tile_size) -> DirtyRectHint {
    if (tile_size <= 1.0f) {
        return hint;
    }
    auto align_down = [tile_size](float value) {
        return std::floor(value / tile_size) * tile_size;
    };
    auto align_up = [tile_size](float value) {
        return std::ceil(value / tile_size) * tile_size;
    };
    DirtyRectHint snapped{};
    snapped.min_x = align_down(hint.min_x);
    snapped.min_y = align_down(hint.min_y);
    snapped.max_x = align_up(hint.max_x);
    snapped.max_y = align_up(hint.max_y);
    if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
        return {};
    }
    return snapped;
}

auto SubmitDirtyRects(PathSpace& space,
                      ConcretePathView targetPath,
                      std::span<DirtyRectHint const> rects) -> SP::Expected<void> {
    if (rects.empty()) {
        return SP::Expected<void>{};
    }
    auto hintsPath = std::string(targetPath.getPath()) + "/hints/dirtyRects";

    auto descPath = std::string(targetPath.getPath()) + "/desc";
    auto desc = read_value<SurfaceDesc>(space, descPath);
    if (!desc) {
        return std::unexpected(desc.error());
    }
    auto const tile_size = static_cast<float>(std::max(1, (*desc).progressive_tile_size_px));
    auto const width = static_cast<float>(std::max(0, (*desc).size_px.width));
    auto const height = static_cast<float>(std::max(0, (*desc).size_px.height));

    std::vector<DirtyRectHint> stored;
    auto existing = read_optional<std::vector<DirtyRectHint>>(space, hintsPath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        stored = std::move(**existing);
    }
    stored.reserve(stored.size() + rects.size());
    for (auto const& hint : rects) {
        auto snapped = snap_hint_to_tiles(hint, tile_size);
        if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
            continue;
        }
        snapped.min_x = std::clamp(snapped.min_x, 0.0f, width);
        snapped.min_y = std::clamp(snapped.min_y, 0.0f, height);
        snapped.max_x = std::clamp(snapped.max_x, 0.0f, width);
        snapped.max_y = std::clamp(snapped.max_y, 0.0f, height);
        if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
            continue;
        }
        stored.push_back(snapped);
    }
    merge_hints(stored, tile_size, width, height);
    return replace_single<std::vector<DirtyRectHint>>(space, hintsPath, stored);
}

auto TriggerRender(PathSpace& space,
                   ConcretePathView targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny> {
    auto descPath = std::string(targetPath.getPath()) + "/desc";
    auto surfaceDesc = read_value<SurfaceDesc>(space, descPath);
    if (!surfaceDesc) {
        return std::unexpected(surfaceDesc.error());
    }

    auto const targetStr = std::string(targetPath.getPath());
    auto targetsPos = targetStr.find("/targets/");
    if (targetsPos == std::string::npos) {
        return std::unexpected(make_error("target path '" + targetStr + "' missing /targets/ segment",
                                          SP::Error::Code::InvalidPath));
    }
    auto rendererPathStr = targetStr.substr(0, targetsPos);
    if (rendererPathStr.empty()) {
        return std::unexpected(make_error("renderer path derived from target is empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto rendererKind = read_renderer_kind(space, rendererPathStr + "/meta/kind");
    if (!rendererKind) {
        return std::unexpected(rendererKind.error());
    }

    auto effectiveKind = *rendererKind;
#if !PATHSPACE_UI_METAL
    if (effectiveKind == RendererKind::Metal2D) {
        effectiveKind = RendererKind::Software2D;
    }
#else
    if (effectiveKind == RendererKind::Metal2D
        && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        effectiveKind = RendererKind::Software2D;
    }
#endif

    SurfaceRenderContext context{
        .target_path = SP::ConcretePathString{std::string{targetPath.getPath()}},
        .renderer_path = SP::ConcretePathString{rendererPathStr},
        .target_desc = *surfaceDesc,
        .settings = settings,
        .renderer_kind = effectiveKind,
    };

    auto surface_key = std::string(context.target_path.getPath());
    if (auto watch = ensure_surface_cache_watch(space, surface_key); !watch) {
        return std::unexpected(watch.error());
    }
    auto& surface = acquire_surface(surface_key, context.target_desc);

#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    if (context.renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(surface_key, context.target_desc);
    }
    auto stats = render_into_target(space, context, surface, metal_surface);
#else
    auto stats = render_into_target(space, context, surface);
#endif
    if (!stats) {
        return std::unexpected(stats.error());
    }

    auto state = std::make_shared<SP::SharedState<bool>>();
    state->set_value(true);
    return SP::FutureT<bool>{state}.to_any();
}

auto RenderHtml(PathSpace& space,
                ConcretePathView targetPath) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath());
    uint64_t rendered_revision = 0;

    auto report_error = [&](SP::Error const& error, std::string detail = std::string{}) -> SP::Expected<void> {
        Diagnostics::PathSpaceError diag{};
        diag.code = static_cast<std::int32_t>(error.code);
        diag.severity = Diagnostics::PathSpaceError::Severity::Recoverable;
        diag.message = error.message.value_or("RenderHtml failed");
        diag.detail = std::move(detail);
        diag.path = base;
        diag.revision = rendered_revision;
        (void)Diagnostics::WriteTargetError(space, targetPath, diag);
        return std::unexpected(error);
    };

    auto targetRoot = derive_app_root_for(targetPath);
    if (!targetRoot) {
        return report_error(targetRoot.error(), "derive_app_root_for");
    }

    auto descPath = base + "/desc";
    auto desc = read_value<HtmlTargetDesc>(space, descPath);
    if (!desc) {
        return report_error(desc.error(), "read html desc");
    }

    auto sceneRel = read_value<std::string>(space, base + "/scene");
    if (!sceneRel) {
        return report_error(sceneRel.error(), "read html scene binding");
    }

    auto sceneAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{targetRoot->getPath()},
                                                       *sceneRel);
    if (!sceneAbsolute) {
        return report_error(sceneAbsolute.error(), "resolve scene path");
    }

    auto sceneRevision = Scene::ReadCurrentRevision(space, ScenePath{sceneAbsolute->getPath()});
    if (!sceneRevision) {
        return report_error(sceneRevision.error(), "read current scene revision");
    }
    rendered_revision = sceneRevision->revision;

    auto revisionBase = std::string(sceneAbsolute->getPath()) + "/builds/" + format_revision(sceneRevision->revision);
    auto bucket = SP::UI::Scene::SceneSnapshotBuilder::decode_bucket(space, revisionBase);
    if (!bucket) {
        return report_error(bucket.error(), "decode scene snapshot");
    }

    Html::EmitOptions options{};
    options.max_dom_nodes = desc->max_dom_nodes;
    options.prefer_dom = desc->prefer_dom;
    options.allow_canvas_fallback = desc->allow_canvas_fallback;
    options.resolve_asset =
        [&](std::string_view logical_path,
            std::uint64_t /*fingerprint*/,
            Html::AssetKind /*kind*/) -> SP::Expected<Html::Asset> {
            Html::Asset asset{};
            asset.logical_path = std::string(logical_path);

            if (!is_safe_asset_path(asset.logical_path)) {
                return std::unexpected(make_error("html asset logical path unsafe: " + asset.logical_path,
                                                  SP::Error::Code::InvalidPath));
            }

            std::string full_path = revisionBase;
            if (asset.logical_path.rfind("assets/", 0) == 0) {
                full_path.append("/").append(asset.logical_path);
            } else {
                full_path.append("/assets/").append(asset.logical_path);
            }

            auto bytes = space.read<std::vector<std::uint8_t>>(full_path);
            if (!bytes) {
                auto const error = bytes.error();
                std::string message = "read html asset '" + asset.logical_path + "'";
                if (error.message) {
                    message.append(": ").append(*error.message);
                }
                return std::unexpected(make_error(std::move(message), error.code));
            }

            asset.bytes = std::move(*bytes);
            asset.mime_type = guess_mime_type(asset.logical_path);
            if (asset.mime_type.empty()) {
                asset.mime_type = "application/octet-stream";
            }
            return asset;
        };

    {
        auto fontManifestPath = revisionBase + "/assets/font-manifest";
        auto fontsValue = read_optional<std::vector<Html::Asset>>(space, fontManifestPath);
        if (!fontsValue) {
            return report_error(fontsValue.error(), "read html font manifest");
        }
        if (fontsValue->has_value()) {
            std::unordered_set<std::string> unique_fonts;
            for (auto const& font : **fontsValue) {
                if (font.logical_path.empty()) {
                    continue;
                }
                if (unique_fonts.insert(font.logical_path).second) {
                    options.font_logical_paths.push_back(font.logical_path);
                }
            }
        }
    }

    Html::Adapter adapter;
    auto emitted = adapter.emit(*bucket, options);
    if (!emitted) {
        return report_error(emitted.error(), "emit html adapter output");
    }

    if (auto status = hydrate_html_assets(space, revisionBase, emitted->assets); !status) {
        return report_error(status.error(), "hydrate html assets");
    }

    auto htmlBase = base + "/output/v1/html";

    // Track existing asset manifest to clear stale blobs/metadata.
    std::vector<std::string> previous_asset_manifest;
    auto manifestPath = htmlBase + "/assets/manifest";
    if (auto existingManifest = read_optional<std::vector<std::string>>(space, manifestPath); !existingManifest) {
        return report_error(existingManifest.error(), "read html asset manifest");
    } else if (existingManifest->has_value()) {
        previous_asset_manifest = std::move(**existingManifest);
    }

    std::vector<std::string> current_manifest;
    current_manifest.reserve(emitted->assets.size());
    for (auto const& asset : emitted->assets) {
        current_manifest.push_back(asset.logical_path);
    }

    std::unordered_set<std::string> current_asset_set{current_manifest.begin(), current_manifest.end()};
    std::unordered_set<std::string> previous_asset_set{previous_asset_manifest.begin(), previous_asset_manifest.end()};

    auto assetsDataBase = htmlBase + "/assets/data";
    auto assetsMetaBase = htmlBase + "/assets/meta";

    // Remove stale asset payloads.
    for (auto const& logical : previous_asset_set) {
        if (current_asset_set.find(logical) != current_asset_set.end()) {
            continue;
        }
        auto const dataPath = assetsDataBase + "/" + logical;
        if (auto status = drain_queue<std::vector<std::uint8_t>>(space, dataPath); !status) {
            return report_error(status.error(), "clear stale html asset bytes");
        }
        auto const mimePath = assetsMetaBase + "/" + logical;
        if (auto status = drain_queue<std::string>(space, mimePath); !status) {
            return report_error(status.error(), "clear stale html asset mime");
        }
    }

    for (auto const& asset : emitted->assets) {
        auto const dataPath = assetsDataBase + "/" + asset.logical_path;
        if (auto status = replace_single<std::vector<std::uint8_t>>(space, dataPath, asset.bytes); !status) {
            return report_error(status.error(), "write html asset bytes");
        }
        auto const mimePath = assetsMetaBase + "/" + asset.logical_path;
        if (auto status = replace_single<std::string>(space, mimePath, asset.mime_type); !status) {
            return report_error(status.error(), "write html asset mime");
        }
    }

    if (current_manifest.empty()) {
        if (auto status = drain_queue<std::vector<std::string>>(space, manifestPath); !status) {
            return report_error(status.error(), "clear html asset manifest");
        }
    } else {
        if (auto status = replace_single<std::vector<std::string>>(space, manifestPath, current_manifest); !status) {
            return report_error(status.error(), "write html asset manifest");
        }
    }

    if (auto status = replace_single<uint64_t>(space, htmlBase + "/revision", sceneRevision->revision); !status) {
        return report_error(status.error(), "write html revision");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/dom", emitted->dom); !status) {
        return report_error(status.error(), "write dom");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/css", emitted->css); !status) {
        return report_error(status.error(), "write css");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/commands", emitted->canvas_commands); !status) {
        return report_error(status.error(), "write canvas commands");
    }
    if (auto status = replace_single<bool>(space, htmlBase + "/usedCanvasFallback", emitted->used_canvas_fallback); !status) {
        return report_error(status.error(), "write canvas fallback flag");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/commandCount",
                                               static_cast<uint64_t>(emitted->canvas_replay_commands.size())); !status) {
        return report_error(status.error(), "write command count");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/domNodeCount",
                                               static_cast<uint64_t>(bucket->drawable_ids.size())); !status) {
        return report_error(status.error(), "write dom node count");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/assetCount",
                                               static_cast<uint64_t>(emitted->assets.size())); !status) {
        return report_error(status.error(), "write asset count");
    }
    if (auto status = replace_single<std::vector<Html::Asset>>(space,
                                                               htmlBase + "/assets",
                                                               emitted->assets); !status) {
        return report_error(status.error(), "write assets");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/options/maxDomNodes",
                                               static_cast<uint64_t>(desc->max_dom_nodes)); !status) {
        return report_error(status.error(), "write maxDomNodes");
    }
    if (auto status = replace_single<bool>(space, htmlBase + "/options/preferDom", desc->prefer_dom); !status) {
        return report_error(status.error(), "write preferDom");
    }
    if (auto status = replace_single<bool>(space, htmlBase + "/options/allowCanvasFallback", desc->allow_canvas_fallback); !status) {
        return report_error(status.error(), "write allowCanvasFallback");
    }
    auto mode = emitted->used_canvas_fallback ? std::string{"canvas"} : std::string{"dom"};
    if (auto status = replace_single<std::string>(space, htmlBase + "/mode", mode); !status) {
        return report_error(status.error(), "write mode");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/metadata/activeMode", mode); !status) {
        return report_error(status.error(), "write active mode metadata");
    }

    if (auto status = Diagnostics::ClearTargetError(space, targetPath); !status) {
        return status;
    }
    return {};
}

} // namespace SP::UI::Runtime::Renderer
