#pragma once

#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace SP::UI::Runtime::Diagnostics {

inline auto severity_to_string(PathSpaceError::Severity severity) -> std::string_view {
    switch (severity) {
    case PathSpaceError::Severity::Info:
        return "info";
    case PathSpaceError::Severity::Warning:
        return "warning";
    case PathSpaceError::Severity::Recoverable:
        return "recoverable";
    case PathSpaceError::Severity::Fatal:
        return "fatal";
    }
    return "unknown";
}

inline auto pathspace_error_to_json(PathSpaceError const& error) -> nlohmann::json {
    return nlohmann::json{{"code", error.code},
                          {"severity", severity_to_string(error.severity)},
                          {"message", error.message},
                          {"path", error.path},
                          {"revision", error.revision},
                          {"timestamp_ns", error.timestamp_ns},
                          {"detail", error.detail}};
}

inline auto error_stats_to_json(ErrorStats const& stats) -> nlohmann::json {
    return nlohmann::json{{"total", stats.total},
                          {"cleared", stats.cleared},
                          {"info", stats.info},
                          {"warning", stats.warning},
                          {"recoverable", stats.recoverable},
                          {"fatal", stats.fatal},
                          {"last_code", stats.last_code},
                          {"last_severity", severity_to_string(stats.last_severity)},
                          {"last_timestamp_ns", stats.last_timestamp_ns},
                          {"last_revision", stats.last_revision}};
}

inline auto font_asset_to_json(SP::UI::Scene::FontAssetReference const& asset) -> nlohmann::json {
    return nlohmann::json{{"drawable_id", asset.drawable_id},
                          {"resource_root", asset.resource_root},
                          {"revision", asset.revision},
                          {"fingerprint", asset.fingerprint},
                          {"kind", asset.kind == SP::UI::Scene::FontAssetKind::Color ? "color" : "alpha"}};
}

inline auto material_descriptor_to_json(SP::UI::MaterialDescriptor const& material)
    -> nlohmann::json {
    return nlohmann::json{{"material_id", material.material_id},
                          {"pipeline_flags", material.pipeline_flags},
                          {"primary_draw_kind", material.primary_draw_kind},
                          {"command_count", material.command_count},
                          {"drawable_count", material.drawable_count},
                          {"color_rgba", material.color_rgba},
                          {"tint_rgba", material.tint_rgba},
                          {"resource_fingerprint", material.resource_fingerprint},
                          {"uses_image", material.uses_image}};
}

inline auto material_resource_to_json(SP::UI::MaterialResourceResidency const& resource)
    -> nlohmann::json {
    return nlohmann::json{{"fingerprint", resource.fingerprint},
                          {"cpu_bytes", resource.cpu_bytes},
                          {"gpu_bytes", resource.gpu_bytes},
                          {"width", resource.width},
                          {"height", resource.height},
                          {"uses_image", resource.uses_image},
                          {"uses_font_atlas", resource.uses_font_atlas}};
}

inline auto target_metrics_to_json(TargetMetrics const& metrics) -> nlohmann::json {
    using nlohmann::json;

    json summary{{"frame_index", metrics.frame_index},
                 {"revision", metrics.revision},
                 {"drawable_count", metrics.drawable_count}};

    json timings{{"render_ms", metrics.render_ms},
                 {"present_ms", metrics.present_ms},
                 {"gpu_encode_ms", metrics.gpu_encode_ms},
                 {"gpu_present_ms", metrics.gpu_present_ms},
                 {"progressive_copy_ms", metrics.progressive_copy_ms}};

    json presentation{{"backend_kind", metrics.backend_kind},
                      {"present_mode", metrics.present_mode},
                      {"used_metal_texture", metrics.used_metal_texture},
                      {"presented", metrics.presented},
                      {"buffered_frame_consumed", metrics.buffered_frame_consumed},
                      {"used_progressive", metrics.used_progressive},
                      {"stale", metrics.stale},
                      {"last_present_skipped", metrics.last_present_skipped},
                      {"auto_render_on_present", metrics.auto_render_on_present},
                      {"vsync_align", metrics.vsync_align},
                      {"max_age_frames", metrics.max_age_frames},
                      {"wait_budget_ms", metrics.wait_budget_ms},
                      {"staleness_budget_ms", metrics.staleness_budget_ms},
                      {"frame_timeout_ms", metrics.frame_timeout_ms},
                      {"frame_age_ms", metrics.frame_age_ms},
                      {"frame_age_frames", metrics.frame_age_frames}};

    json progressive{{"progressive_tiles_updated", metrics.progressive_tiles_updated},
                     {"progressive_bytes_copied", metrics.progressive_bytes_copied},
                     {"progressive_tile_size", metrics.progressive_tile_size},
                     {"progressive_workers_used", metrics.progressive_workers_used},
                     {"progressive_jobs", metrics.progressive_jobs},
                     {"progressive_tile_diagnostics_enabled", metrics.progressive_tile_diagnostics_enabled},
                     {"progressive_tiles_copied", metrics.progressive_tiles_copied},
                     {"progressive_tiles_dirty", metrics.progressive_tiles_dirty},
                     {"progressive_tiles_total", metrics.progressive_tiles_total},
                     {"progressive_tiles_skipped", metrics.progressive_tiles_skipped},
                     {"progressive_rects_coalesced", metrics.progressive_rects_coalesced},
                     {"progressive_skip_seq_odd", metrics.progressive_skip_seq_odd},
                     {"progressive_recopy_after_seq_change", metrics.progressive_recopy_after_seq_change}};

    json pipeline{{"encode_workers_used", metrics.encode_workers_used},
                  {"encode_jobs", metrics.encode_jobs}};

    json contention{{"encode_worker_stall_ms_total", metrics.encode_worker_stall_ms_total},
                    {"encode_worker_stall_ms_max", metrics.encode_worker_stall_ms_max},
                    {"encode_worker_stall_workers", metrics.encode_worker_stall_workers}};

    json materials{
        {"material_count", metrics.material_count},
        {"material_resource_count", metrics.material_resource_count},
    };

    json material_list = json::array();
    for (auto const& material : metrics.materials) {
        material_list.push_back(material_descriptor_to_json(material));
    }
    if (!material_list.empty()) {
        materials["materials"] = std::move(material_list);
    }

    json resource_list = json::array();
    for (auto const& resource : metrics.material_resources) {
        resource_list.push_back(material_resource_to_json(resource));
    }
    if (!resource_list.empty()) {
        materials["material_resources"] = std::move(resource_list);
    }

    json residency{{"cpu_bytes", metrics.cpu_bytes},
                   {"cpu_soft_bytes", metrics.cpu_soft_bytes},
                   {"cpu_hard_bytes", metrics.cpu_hard_bytes},
                   {"gpu_bytes", metrics.gpu_bytes},
                   {"gpu_soft_bytes", metrics.gpu_soft_bytes},
                   {"gpu_hard_bytes", metrics.gpu_hard_bytes},
                   {"cpu_soft_budget_ratio", metrics.cpu_soft_budget_ratio},
                   {"cpu_hard_budget_ratio", metrics.cpu_hard_budget_ratio},
                   {"gpu_soft_budget_ratio", metrics.gpu_soft_budget_ratio},
                   {"gpu_hard_budget_ratio", metrics.gpu_hard_budget_ratio},
                   {"cpu_soft_exceeded", metrics.cpu_soft_exceeded},
                   {"cpu_hard_exceeded", metrics.cpu_hard_exceeded},
                   {"gpu_soft_exceeded", metrics.gpu_soft_exceeded},
                   {"gpu_hard_exceeded", metrics.gpu_hard_exceeded},
                   {"cpu_status", metrics.cpu_residency_status},
                   {"gpu_status", metrics.gpu_residency_status},
                   {"overall_status", metrics.residency_overall_status}};

    json fonts_activity{{"font_active_count", metrics.font_active_count},
                        {"font_atlas_cpu_bytes", metrics.font_atlas_cpu_bytes},
                        {"font_atlas_gpu_bytes", metrics.font_atlas_gpu_bytes},
                        {"font_atlas_resource_count", metrics.font_atlas_resource_count}};

    json fonts_cache{{"font_registered_fonts", metrics.font_registered_fonts},
                     {"font_cache_hits", metrics.font_cache_hits},
                     {"font_cache_misses", metrics.font_cache_misses},
                     {"font_cache_evictions", metrics.font_cache_evictions},
                     {"font_cache_size", metrics.font_cache_size},
                     {"font_cache_capacity", metrics.font_cache_capacity},
                     {"font_cache_hard_capacity", metrics.font_cache_hard_capacity},
                     {"font_atlas_soft_bytes", metrics.font_atlas_soft_bytes},
                     {"font_atlas_hard_bytes", metrics.font_atlas_hard_bytes},
                     {"font_shaped_run_approx_bytes", metrics.font_shaped_run_approx_bytes}};

    json font_assets = json::array();
    for (auto const& asset : metrics.font_assets) {
        font_assets.push_back(font_asset_to_json(asset));
    }

    json fonts{{"activity", std::move(fonts_activity)}, {"cache", std::move(fonts_cache)}};
    if (!font_assets.empty()) {
        fonts["assets"] = std::move(font_assets);
    }

    json html{{"dom_node_count", metrics.html_dom_node_count},
              {"command_count", metrics.html_command_count},
              {"asset_count", metrics.html_asset_count},
              {"max_dom_nodes", metrics.html_max_dom_nodes},
              {"used_canvas_fallback", metrics.html_used_canvas_fallback},
              {"prefer_dom", metrics.html_prefer_dom},
              {"allow_canvas_fallback", metrics.html_allow_canvas_fallback},
              {"mode", metrics.html_mode}};

    json errors{{"last_error", metrics.last_error},
                {"last_error_code", metrics.last_error_code},
                {"last_error_revision", metrics.last_error_revision},
                {"last_error_severity", severity_to_string(metrics.last_error_severity)},
                {"last_error_timestamp_ns", metrics.last_error_timestamp_ns},
                {"last_error_detail", metrics.last_error_detail},
                {"error_total", metrics.error_total},
                {"error_cleared", metrics.error_cleared},
                {"error_info", metrics.error_info},
                {"error_warning", metrics.error_warning},
                {"error_recoverable", metrics.error_recoverable},
                {"error_fatal", metrics.error_fatal}};

    json root;
    root["summary"]      = std::move(summary);
    root["timings"]      = std::move(timings);
    root["presentation"] = std::move(presentation);
    root["progressive"]  = std::move(progressive);
    root["pipeline"]     = std::move(pipeline);
    root["work_contention"] = std::move(contention);
    root["materials"]    = std::move(materials);
    root["residency"]    = std::move(residency);
    root["fonts"]        = std::move(fonts);
    root["html"]         = std::move(html);
    root["errors"]       = std::move(errors);

    return root;
}

inline auto target_diagnostics_to_json(TargetDiagnosticsSummary const& summary) -> nlohmann::json {
    nlohmann::json json{{"path", summary.path},
                        {"renderer", summary.renderer},
                        {"target", summary.target},
                        {"metrics", target_metrics_to_json(summary.metrics)}};

    nlohmann::json errors = nlohmann::json::object();
    if (summary.live_error && !summary.live_error->message.empty()) {
        errors["live"] = pathspace_error_to_json(*summary.live_error);
    }
    errors["stats"] = error_stats_to_json(summary.error_stats);

    if (!errors.empty()) {
        json["errors"] = std::move(errors);
    }

    return json;
}

inline auto SerializeTargetDiagnostics(std::vector<TargetDiagnosticsSummary> const& summaries,
                                       std::string const& captured_at = {}) -> nlohmann::json {
    nlohmann::json targets = nlohmann::json::array();
    for (auto const& summary : summaries) {
        targets.push_back(target_diagnostics_to_json(summary));
    }

    nlohmann::json payload{{"target_count", summaries.size()}, {"targets", std::move(targets)}};
    if (!captured_at.empty()) {
        payload["captured_at"] = captured_at;
    }
    return payload;
}

} // namespace SP::UI::Runtime::Diagnostics
