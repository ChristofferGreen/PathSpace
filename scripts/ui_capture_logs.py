#!/usr/bin/env python3
"""Summarize UI renderer diagnostics from a pathspace_dump_json snapshot.

This helper expects the JSON produced by ./build/pathspace_dump_json with
diagnostics enabled (the default). It scans every renderer target under
/renderers/*/targets/* and collects the `TargetMetrics` surface published by
the runtime: timings, present policy, progressive/copy counters, encode
contention data, residency budgets, and material descriptors. The summary also
captures the structured PathSpaceError payload plus the ErrorStats counters so
bug reports have a single JSON artifact.

Example:
  ./build/pathspace_dump_json --root /renderers --max-depth 8 --output out/renderers.json
  python3 scripts/ui_capture_logs.py --snapshot out/renderers.json --pretty --output out/ui_diagnostics.json
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any, Dict, Iterable, List, Optional, Tuple

SUMMARY_FIELDS = {
    "frame_index": "frameIndex",
    "revision": "revision",
    "drawable_count": "drawableCount",
}

TIMING_FIELDS = {
    "render_ms": "renderMs",
    "present_ms": "presentMs",
    "gpu_encode_ms": "gpuEncodeMs",
    "gpu_present_ms": "gpuPresentMs",
    "progressive_copy_ms": "progressiveCopyMs",
    "wait_budget_ms": "waitBudgetMs",
    "staleness_budget_ms": "stalenessBudgetMs",
    "frame_timeout_ms": "frameTimeoutMs",
}

PRESENTATION_FIELDS = {
    "backend_kind": "backendKind",
    "present_mode": "presentMode",
    "used_metal_texture": "usedMetalTexture",
    "presented": "presented",
    "buffered_frame_consumed": "bufferedFrameConsumed",
    "used_progressive": "usedProgressive",
    "stale": "stale",
    "last_present_skipped": "lastPresentSkipped",
    "auto_render_on_present": "autoRenderOnPresent",
    "vsync_align": "vsyncAlign",
    "max_age_frames": "maxAgeFrames",
    "frame_age_ms": "presentedAgeMs",
    "frame_age_frames": "presentedAgeFrames",
}

PROGRESSIVE_FIELDS = {
    "progressive_tiles_updated": "progressiveTilesUpdated",
    "progressive_bytes_copied": "progressiveBytesCopied",
    "progressive_tile_size": "progressiveTileSize",
    "progressive_workers_used": "progressiveWorkersUsed",
    "progressive_jobs": "progressiveJobs",
    "progressive_tile_diagnostics_enabled": "progressiveTileDiagnosticsEnabled",
    "progressive_tiles_copied": "progressiveTilesCopied",
    "progressive_rects_coalesced": "progressiveRectsCoalesced",
    "progressive_tiles_dirty": "progressiveTilesDirty",
    "progressive_tiles_total": "progressiveTilesTotal",
    "progressive_tiles_skipped": "progressiveTilesSkipped",
}

PIPELINE_FIELDS = {
    "encode_workers_used": "encodeWorkersUsed",
    "encode_jobs": "encodeJobs",
}

CONTENTIONS_FIELDS = {
    "encode_worker_stall_ms_total": "encodeWorkerStallMsTotal",
    "encode_worker_stall_ms_max": "encodeWorkerStallMsMax",
    "encode_worker_stall_workers": "encodeWorkerStallWorkers",
    "progressive_seq_retry_count": "progressiveTileSeqRetryCount",
    "progressive_seq_skip_count": "progressiveTileSeqSkipCount",
}

MATERIAL_FIELDS = {
    "material_count": "materialCount",
    "materials": "materialDescriptors",
    "material_resource_count": "materialResourceCount",
    "material_resources": "materialResources",
}

HTML_FIELDS = {
    "dom_node_count": "domNodeCount",
    "command_count": "commandCount",
    "asset_count": "assetCount",
    "used_canvas_fallback": "usedCanvasFallback",
    "mode": "mode",
}

FONT_ACTIVITY_FIELDS = {
    "font_active_count": "fontActiveCount",
    "font_atlas_cpu_bytes": "fontAtlasCpuBytes",
    "font_atlas_gpu_bytes": "fontAtlasGpuBytes",
    "font_atlas_resource_count": "fontAtlasResourceCount",
}

FONT_CACHE_FIELDS = {
    "font_registered_fonts": "fontRegisteredFonts",
    "font_cache_hits": "fontCacheHits",
    "font_cache_misses": "fontCacheMisses",
    "font_cache_evictions": "fontCacheEvictions",
    "font_cache_size": "fontCacheSize",
    "font_cache_capacity": "fontCacheCapacity",
    "font_cache_hard_capacity": "fontCacheHardCapacity",
    "font_atlas_soft_bytes": "fontAtlasSoftBytes",
    "font_atlas_hard_bytes": "fontAtlasHardBytes",
    "font_shaped_run_approx_bytes": "fontShapedRunApproxBytes",
}

RESIDENCY_FIELDS = {
    "cpu_bytes": "cpuBytes",
    "cpu_soft_bytes": "cpuSoftBytes",
    "cpu_hard_bytes": "cpuHardBytes",
    "gpu_bytes": "gpuBytes",
    "gpu_soft_bytes": "gpuSoftBytes",
    "gpu_hard_bytes": "gpuHardBytes",
    "cpu_soft_budget_ratio": "cpuSoftBudgetRatio",
    "cpu_hard_budget_ratio": "cpuHardBudgetRatio",
    "gpu_soft_budget_ratio": "gpuSoftBudgetRatio",
    "gpu_hard_budget_ratio": "gpuHardBudgetRatio",
    "cpu_soft_exceeded": "cpuSoftExceeded",
    "cpu_hard_exceeded": "cpuHardExceeded",
    "gpu_soft_exceeded": "gpuSoftExceeded",
    "gpu_hard_exceeded": "gpuHardExceeded",
    "cpu_status": "cpuStatus",
    "gpu_status": "gpuStatus",
    "overall_status": "overallStatus",
}


def build_node_map(nodes: Iterable[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    return {node.get("path", ""): node for node in nodes if "path" in node}


def extract_value(node: Dict[str, Any]) -> Any:
    for entry in node.get("values", []):
        if "value" in entry:
            return entry["value"]
    return None


def find_targets(node_map: Dict[str, Dict[str, Any]]) -> List[Tuple[str, str, str]]:
    targets: Dict[str, Tuple[str, str]] = {}
    for path in node_map.keys():
        if not path or "/renderers/" not in path or "/targets/" not in path:
            continue
        parts = [part for part in path.split('/') if part]
        if "renderers" not in parts or "targets" not in parts:
            continue
        renderer_index = parts.index("renderers")
        target_index = parts.index("targets")
        if renderer_index + 1 >= len(parts) or target_index + 1 >= len(parts):
            continue
        renderer_name = parts[renderer_index + 1]
        target_name = parts[target_index + 1]
        base_components = [''] + parts[:target_index + 2]
        base_path = "/" + "/".join(base_components[1:])
        targets[base_path] = (renderer_name, target_name)
    return sorted((base, names[0], names[1]) for base, names in targets.items())


def read_metric(node_map: Dict[str, Dict[str, Any]], path: str) -> Any:
    node = node_map.get(path)
    if not node:
        return None
    return extract_value(node)


def read_metrics_block(node_map: Dict[str, Dict[str, Any]],
                       base: str,
                       mapping: Dict[str, str]) -> Dict[str, Any]:
    block: Dict[str, Any] = {}
    for key, suffix in mapping.items():
        value = read_metric(node_map, f"{base}/{suffix}")
        if value is not None:
            block[key] = value
    return block


def read_first_metric(node_map: Dict[str, Dict[str, Any]], paths: List[str]) -> Any:
    for path in paths:
        value = read_metric(node_map, path)
        if value is not None:
            return value
    return None


def collect_target_summary(node_map: Dict[str, Dict[str, Any]], base: str,
                            renderer_name: str, target_name: str) -> Dict[str, Any]:
    output_common = f"{base}/output/v1/common"
    diag_metrics = f"{base}/diagnostics/metrics"
    diag_metrics_compat = f"{base}/output/v1/diagnostics/metrics"
    residency_base = f"{base}/diagnostics/metrics/residency"
    html_base = f"{base}/output/v1/html"

    summary_block = read_metrics_block(node_map, output_common, SUMMARY_FIELDS)
    timings = read_metrics_block(node_map, output_common, TIMING_FIELDS)
    presentation = read_metrics_block(node_map, output_common, PRESENTATION_FIELDS)
    progressive = read_metrics_block(node_map, output_common, PROGRESSIVE_FIELDS)
    pipeline = read_metrics_block(node_map, output_common, PIPELINE_FIELDS)
    contention = read_metrics_block(node_map, diag_metrics, CONTENTIONS_FIELDS)
    if not contention:
        contention = read_metrics_block(node_map, diag_metrics_compat, CONTENTIONS_FIELDS)
    materials = read_metrics_block(node_map, output_common, MATERIAL_FIELDS)
    residency = read_metrics_block(node_map, residency_base, RESIDENCY_FIELDS)
    font_activity = read_metrics_block(node_map, output_common, FONT_ACTIVITY_FIELDS)
    font_cache = read_metrics_block(node_map, output_common, FONT_CACHE_FIELDS)
    font_assets = read_metric(node_map, f"{output_common}/fontActiveAssets")
    html = read_metrics_block(node_map, html_base, HTML_FIELDS)

    max_dom_nodes = read_metric(node_map, f"{html_base}/options/maxDomNodes")
    if max_dom_nodes is not None:
        html["max_dom_nodes"] = max_dom_nodes

    prefer_dom = read_metric(node_map, f"{html_base}/options/preferDom")
    if prefer_dom is not None:
        html["prefer_dom"] = prefer_dom

    allow_canvas_fallback = read_metric(node_map, f"{html_base}/options/allowCanvasFallback")
    if allow_canvas_fallback is not None:
        html["allow_canvas_fallback"] = allow_canvas_fallback

    progressive_skip = read_first_metric(
        node_map,
        [
            f"{output_common}/progressiveSkipOddSeq",
            f"{diag_metrics}/progressiveTileSeqSkipCount",
            f"{diag_metrics_compat}/progressiveTileSeqSkipCount",
        ],
    )
    if progressive_skip is not None:
        progressive["progressive_skip_seq_odd"] = progressive_skip

    progressive_retry = read_first_metric(
        node_map,
        [
            f"{output_common}/progressiveRecopyAfterSeqChange",
            f"{diag_metrics}/progressiveTileSeqRetryCount",
            f"{diag_metrics_compat}/progressiveTileSeqRetryCount",
        ],
    )
    if progressive_retry is not None:
        progressive["progressive_recopy_after_seq_change"] = progressive_retry

    metrics: Dict[str, Any] = {}
    if summary_block:
        metrics["summary"] = summary_block
    if timings:
        metrics["timings"] = timings
    if presentation:
        metrics["presentation"] = presentation
    if progressive:
        metrics["progressive"] = progressive
    if pipeline:
        metrics["pipeline"] = pipeline
    if contention:
        metrics["work_contention"] = contention
    if materials:
        metrics["materials"] = materials
    if residency:
        metrics["residency"] = residency
    if html:
        metrics["html"] = html
    if font_assets is not None:
        font_activity.setdefault("active_assets", font_assets)
    if font_activity or font_cache:
        fonts_block: Dict[str, Any] = {}
        if font_activity:
            fonts_block["activity"] = font_activity
        if font_cache:
            fonts_block["cache"] = font_cache
        metrics["fonts"] = fonts_block

    common_last_error = read_metric(node_map, f"{output_common}/lastError")
    common_last_error_code = read_metric(node_map, f"{output_common}/lastErrorCode")
    common_last_error_severity = read_metric(node_map, f"{output_common}/lastErrorSeverity")
    common_last_error_revision = read_metric(node_map, f"{output_common}/lastErrorRevision")
    common_last_error_timestamp = read_metric(node_map, f"{output_common}/lastErrorTimestampNs")
    common_last_error_detail = read_metric(node_map, f"{output_common}/lastErrorDetail")
    live_error = read_metric(node_map, f"{base}/diagnostics/errors/live")
    error_stats = read_metric(node_map, f"{base}/diagnostics/errors/stats")

    errors: Dict[str, Any] = {}
    if common_last_error is not None:
        errors["last_message"] = common_last_error
    if live_error is not None:
        errors["live"] = live_error
    if common_last_error_code is not None:
        errors["last_code"] = common_last_error_code
    if common_last_error_severity is not None:
        errors["last_severity"] = common_last_error_severity
    if common_last_error_revision is not None:
        errors["last_revision"] = common_last_error_revision
    if common_last_error_timestamp is not None:
        errors["last_timestamp_ns"] = common_last_error_timestamp
    if common_last_error_detail is not None:
        errors["last_detail"] = common_last_error_detail
    if error_stats is not None:
        errors["stats"] = error_stats

    summary = {
        "path": base,
        "renderer": renderer_name,
        "target": target_name,
        "metrics": metrics,
    }
    if errors:
        summary["errors"] = errors
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture UI diagnostics from a PathSpace snapshot")
    parser.add_argument("--snapshot", required=True, help="path to pathspace_dump_json output")
    parser.add_argument("--output", help="optional output file; defaults to stdout")
    parser.add_argument("--target-filter", help="regex to filter target base paths")
    parser.add_argument("--pretty", action="store_true", help="pretty-print JSON output")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    snapshot_path = pathlib.Path(args.snapshot)
    with snapshot_path.open("r", encoding="utf-8") as handle:
        snapshot = json.load(handle)

    nodes = snapshot.get("nodes", [])
    node_map = build_node_map(nodes)

    targets = find_targets(node_map)
    if args.target_filter:
        pattern = re.compile(args.target_filter)
        targets = [entry for entry in targets if pattern.search(entry[0])]

    summaries = [collect_target_summary(node_map, base, renderer, target)
                 for base, renderer, target in targets]

    result = {
        "snapshot": str(snapshot_path),
        "target_count": len(summaries),
        "targets": summaries,
    }

    indent = 2 if args.pretty else None
    if args.output:
        out_path = pathlib.Path(args.output)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with out_path.open("w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=indent, ensure_ascii=True)
            handle.write("\n")
    else:
        json.dump(result, sys.stdout, indent=indent, ensure_ascii=True)
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
