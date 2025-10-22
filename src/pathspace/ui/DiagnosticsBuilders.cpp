#include "BuildersDetail.hpp"

namespace SP::UI::Builders::Diagnostics {

using namespace Detail;

auto ReadTargetMetrics(PathSpace const& space,
                        ConcretePathView targetPath) -> SP::Expected<TargetMetrics> {
    TargetMetrics metrics{};

    auto base = std::string(targetPath.getPath()) + "/output/v1/common";

    if (auto value = read_value<uint64_t>(space, base + "/frameIndex"); value) {
        metrics.frame_index = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/revision"); value) {
        metrics.revision = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/renderMs"); value) {
        metrics.render_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/presentMs"); value) {
        metrics.present_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/gpuEncodeMs"); value) {
        metrics.gpu_encode_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/gpuPresentMs"); value) {
        metrics.gpu_present_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/progressiveCopyMs"); value) {
        metrics.progressive_copy_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/usedMetalTexture"); value) {
        metrics.used_metal_texture = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/presented"); value) {
        metrics.presented = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/bufferedFrameConsumed"); value) {
        metrics.buffered_frame_consumed = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/usedProgressive"); value) {
        metrics.used_progressive = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/stale"); value) {
        metrics.stale = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, base + "/backendKind"); value) {
        metrics.backend_kind = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, base + "/presentMode"); value) {
        metrics.present_mode = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/waitBudgetMs"); value) {
        metrics.wait_budget_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/stalenessBudgetMs"); value) {
        metrics.staleness_budget_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/frameTimeoutMs"); value) {
        metrics.frame_timeout_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/maxAgeFrames"); value) {
        metrics.max_age_frames = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/autoRenderOnPresent"); value) {
        metrics.auto_render_on_present = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/vsyncAlign"); value) {
        metrics.vsync_align = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/lastPresentSkipped"); value) {
        metrics.last_present_skipped = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/drawableCount"); value) {
        metrics.drawable_count = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/presentedAgeMs"); value) {
        metrics.frame_age_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/presentedAgeFrames"); value) {
        metrics.frame_age_frames = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveTilesUpdated"); value) {
        metrics.progressive_tiles_updated = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveBytesCopied"); value) {
        metrics.progressive_bytes_copied = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveTileSize"); value) {
        metrics.progressive_tile_size = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveWorkersUsed"); value) {
        metrics.progressive_workers_used = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveJobs"); value) {
        metrics.progressive_jobs = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/encodeWorkersUsed"); value) {
        metrics.encode_workers_used = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/encodeJobs"); value) {
        metrics.encode_jobs = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/progressiveTileDiagnosticsEnabled"); value) {
        metrics.progressive_tile_diagnostics_enabled = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveTilesCopied"); value) {
        metrics.progressive_tiles_copied = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveRectsCoalesced"); value) {
        metrics.progressive_rects_coalesced = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveSkipOddSeq"); value) {
        metrics.progressive_skip_seq_odd = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveRecopyAfterSeqChange"); value) {
        metrics.progressive_recopy_after_seq_change = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveTilesDirty"); value) {
        metrics.progressive_tiles_dirty = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveTilesTotal"); value) {
        metrics.progressive_tiles_total = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/progressiveTilesSkipped"); value) {
        metrics.progressive_tiles_skipped = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/materialCount"); value) {
        metrics.material_count = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto descriptors = read_optional<std::vector<MaterialDescriptor>>(space, base + "/materialDescriptors"); !descriptors) {
        return std::unexpected(descriptors.error());
    } else if (descriptors->has_value()) {
        metrics.materials = std::move(**descriptors);
        if (metrics.material_count == 0) {
            metrics.material_count = static_cast<uint64_t>(metrics.materials.size());
        }
    }

    if (auto value = read_value<uint64_t>(space, base + "/materialResourceCount"); value) {
        metrics.material_resource_count = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto resources = read_optional<std::vector<MaterialResourceResidency>>(space, base + "/materialResources"); !resources) {
        return std::unexpected(resources.error());
    } else if (resources->has_value()) {
        metrics.material_resources = std::move(**resources);
        if (metrics.material_resource_count == 0) {
            metrics.material_resource_count = static_cast<uint64_t>(metrics.material_resources.size());
        }
    }

    auto residencyBase = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuBytes"); value) {
        metrics.cpu_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuSoftBytes"); value) {
        metrics.cpu_soft_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuHardBytes"); value) {
        metrics.cpu_hard_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuBytes"); value) {
        metrics.gpu_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuSoftBytes"); value) {
        metrics.gpu_soft_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuHardBytes"); value) {
        metrics.gpu_hard_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/cpuSoftBudgetRatio"); value) {
        metrics.cpu_soft_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/cpuHardBudgetRatio"); value) {
        metrics.cpu_hard_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/gpuSoftBudgetRatio"); value) {
        metrics.gpu_soft_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/gpuHardBudgetRatio"); value) {
        metrics.gpu_hard_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/cpuSoftExceeded"); value) {
        metrics.cpu_soft_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/cpuHardExceeded"); value) {
        metrics.cpu_hard_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/gpuSoftExceeded"); value) {
        metrics.gpu_soft_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/gpuHardExceeded"); value) {
        metrics.gpu_hard_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, residencyBase + "/cpuStatus"); value) {
        metrics.cpu_residency_status = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, residencyBase + "/gpuStatus"); value) {
        metrics.gpu_residency_status = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, residencyBase + "/overallStatus"); value) {
        metrics.residency_overall_status = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    metrics.last_error.clear();
    metrics.last_error_code = 0;
    metrics.last_error_revision = 0;
    metrics.last_error_severity = PathSpaceError::Severity::Info;
    metrics.last_error_timestamp_ns = 0;
    metrics.last_error_detail.clear();

    auto diagPath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    if (auto errorValue = read_optional<PathSpaceError>(space, diagPath); !errorValue) {
        return std::unexpected(errorValue.error());
    } else if (errorValue->has_value() && !(*errorValue)->message.empty()) {
        metrics.last_error = (*errorValue)->message;
        metrics.last_error_code = (*errorValue)->code;
        metrics.last_error_revision = (*errorValue)->revision;
        metrics.last_error_severity = (*errorValue)->severity;
        metrics.last_error_timestamp_ns = (*errorValue)->timestamp_ns;
        metrics.last_error_detail = (*errorValue)->detail;
    } else {
        if (auto value = read_value<std::string>(space, base + "/lastError"); value) {
            metrics.last_error = *value;
        } else if (value.error().code != SP::Error::Code::NoObjectFound
                   && value.error().code != SP::Error::Code::NoSuchPath) {
            return std::unexpected(value.error());
        }
    }

    return metrics;
}

auto ClearTargetError(PathSpace& space,
                      ConcretePathView targetPath) -> SP::Expected<void> {
    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    PathSpaceError cleared{};
    if (auto status = replace_single<PathSpaceError>(space, livePath, cleared); !status) {
        return status;
    }
    auto lastErrorPath = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, lastErrorPath, std::string{});
}

auto WriteTargetError(PathSpace& space,
                      ConcretePathView targetPath,
                      PathSpaceError const& error) -> SP::Expected<void> {
    if (error.message.empty()) {
        return ClearTargetError(space, targetPath);
    }

    PathSpaceError stored = error;
    if (stored.path.empty()) {
        stored.path = std::string(targetPath.getPath());
    }
    if (stored.timestamp_ns == 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        stored.timestamp_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    if (auto status = replace_single<PathSpaceError>(space, livePath, stored); !status) {
        return status;
    }
    auto lastErrorPath = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, lastErrorPath, stored.message);
}

auto ReadTargetError(PathSpace const& space,
                     ConcretePathView targetPath) -> SP::Expected<std::optional<PathSpaceError>> {
    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    return read_optional<PathSpaceError>(space, livePath);
}

auto ReadSoftwareFramebuffer(PathSpace const& space,
                              ConcretePathView targetPath) -> SP::Expected<SoftwareFramebuffer> {
    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    return read_value<SoftwareFramebuffer>(space, framebufferPath);
}

auto write_present_metrics_to_base(PathSpace& space,
                                   std::string const& base,
                                   PathWindowPresentStats const& stats,
                                   PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    if (auto status = replace_single<uint64_t>(space, base + "/frameIndex", stats.frame.frame_index); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/revision", stats.frame.revision); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/renderMs", stats.frame.render_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/damageMs", stats.damage_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/encodeMs", stats.encode_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/progressiveCopyMs", stats.progressive_copy_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/publishMs", stats.publish_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/presentMs", stats.present_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuEncodeMs", stats.gpu_encode_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuPresentMs", stats.gpu_present_ms); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/lastPresentSkipped", stats.skipped); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/usedMetalTexture", stats.used_metal_texture); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/backendKind", stats.backend_kind); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/presented", stats.presented); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/bufferedFrameConsumed", stats.buffered_frame_consumed); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/usedProgressive", stats.used_progressive); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/presentedAgeMs", stats.frame_age_ms); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/presentedAgeFrames", stats.frame_age_frames); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/stale", stats.stale); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space,
                                                  base + "/presentMode",
                                                  present_mode_to_string(stats.mode)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/drawableCount", stats.drawable_count); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveTilesUpdated",
                                               stats.progressive_tiles_updated); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveBytesCopied",
                                               stats.progressive_bytes_copied); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveTileSize",
                                               stats.progressive_tile_size); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveWorkersUsed",
                                               stats.progressive_workers_used); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveJobs",
                                               stats.progressive_jobs); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/encodeWorkersUsed",
                                               stats.encode_workers_used); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/encodeJobs",
                                               stats.encode_jobs); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/progressiveTileDiagnosticsEnabled",
                                           stats.progressive_tile_diagnostics_enabled); !status) {
        return status;
    }
    auto progressive_tiles_copied = static_cast<uint64_t>(stats.progressive_tiles_copied);
    if (progressive_tiles_copied == 0) {
        auto existing_tiles = space.read<uint64_t>(base + "/progressiveTilesCopied");
        if (existing_tiles) {
            progressive_tiles_copied = *existing_tiles;
        }
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveTilesCopied",
                                               progressive_tiles_copied); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveRectsCoalesced",
                                               static_cast<uint64_t>(stats.progressive_rects_coalesced)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveSkipOddSeq",
                                               static_cast<uint64_t>(stats.progressive_skip_seq_odd)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveRecopyAfterSeqChange",
                                               static_cast<uint64_t>(stats.progressive_recopy_after_seq_change)); !status) {
        return status;
    }
    if (stats.progressive_tile_diagnostics_enabled) {
        if (auto status = replace_single<uint64_t>(space,
                                                   base + "/progressiveTilesDirty",
                                                   stats.progressive_tiles_dirty); !status) {
            return status;
        }
        if (auto status = replace_single<uint64_t>(space,
                                                   base + "/progressiveTilesTotal",
                                                   stats.progressive_tiles_total); !status) {
            return status;
        }
        if (auto status = replace_single<uint64_t>(space,
                                                   base + "/progressiveTilesSkipped",
                                                   stats.progressive_tiles_skipped); !status) {
            return status;
        }
    }
    if (auto status = replace_single<double>(space, base + "/waitBudgetMs", stats.wait_budget_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space,
                                             base + "/stalenessBudgetMs",
                                             policy.staleness_budget_ms_value); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space,
                                             base + "/frameTimeoutMs",
                                             policy.frame_timeout_ms_value); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/maxAgeFrames",
                                               static_cast<uint64_t>(policy.max_age_frames)); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/autoRenderOnPresent",
                                           policy.auto_render_on_present); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/vsyncAlign",
                                           policy.vsync_align); !status) {
        return status;
    }
    return {};
}

auto WritePresentMetrics(PathSpace& space,
                          ConcretePathView targetPath,
                          PathWindowPresentStats const& stats,
                          PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath()) + "/output/v1/common";
    if (auto status = write_present_metrics_to_base(space, base, stats, policy); !status) {
        return status;
    }

    if (!stats.error.empty()) {
        PathSpaceError error{};
        error.code = 3000;
        error.severity = PathSpaceError::Severity::Recoverable;
        error.message = stats.error;
        error.path = std::string(targetPath.getPath());
        error.revision = stats.frame.revision;
        if (auto status = WriteTargetError(space, targetPath, error); !status) {
            return status;
        }
    } else {
        if (auto status = ClearTargetError(space, targetPath); !status) {
            return status;
        }
    }
    return {};
}

auto WriteWindowPresentMetrics(PathSpace& space,
                               ConcretePathView windowPath,
                               std::string_view viewName,
                               PathWindowPresentStats const& stats,
                               PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    std::string base = std::string(windowPath.getPath()) + "/diagnostics/metrics/live/views/";
    base.append(viewName);
    base.append("/present");

    if (auto status = write_present_metrics_to_base(space, base, stats, policy); !status) {
        return status;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    if (auto status = replace_single<std::string>(space, base + "/lastError", stats.error); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/viewName", std::string(viewName)); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/timestampNs", timestamp_ns); !status) {
        return status;
    }
#if defined(__APPLE__)
    if (auto status = replace_single<bool>(space, base + "/usedIOSurface", stats.used_iosurface); !status) {
        return status;
    }
#endif
    return {};
}

auto WriteResidencyMetrics(PathSpace& space,
                           ConcretePathView targetPath,
                           std::uint64_t cpu_bytes,
                           std::uint64_t gpu_bytes,
                           std::uint64_t cpu_soft_bytes,
                           std::uint64_t cpu_hard_bytes,
                           std::uint64_t gpu_soft_bytes,
                           std::uint64_t gpu_hard_bytes) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuBytes", cpu_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuSoftBytes", cpu_soft_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuHardBytes", cpu_hard_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuBytes", gpu_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuSoftBytes", gpu_soft_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuHardBytes", gpu_hard_bytes); !status) {
        return status;
    }

    auto safe_ratio = [](std::uint64_t value, std::uint64_t limit) -> double {
        if (limit == 0) {
            return 0.0;
        }
        return static_cast<double>(value) / static_cast<double>(limit);
    };
    auto classify = [](std::uint64_t value, std::uint64_t soft, std::uint64_t hard) -> std::string {
        if (hard > 0 && value >= hard) {
            return "hard";
        }
        if (soft > 0 && value >= soft) {
            return "soft";
        }
        return "ok";
    };
    auto severity_rank = [](std::string_view status) {
        if (status == "hard") {
            return 2;
        }
        if (status == "soft") {
            return 1;
        }
        return 0;
    };

    const double cpu_soft_ratio = safe_ratio(cpu_bytes, cpu_soft_bytes);
    const double cpu_hard_ratio = safe_ratio(cpu_bytes, cpu_hard_bytes);
    const double gpu_soft_ratio = safe_ratio(gpu_bytes, gpu_soft_bytes);
    const double gpu_hard_ratio = safe_ratio(gpu_bytes, gpu_hard_bytes);

    const bool cpu_soft_exceeded = cpu_soft_bytes > 0 && cpu_bytes >= cpu_soft_bytes;
    const bool cpu_hard_exceeded = cpu_hard_bytes > 0 && cpu_bytes >= cpu_hard_bytes;
    const bool gpu_soft_exceeded = gpu_soft_bytes > 0 && gpu_bytes >= gpu_soft_bytes;
    const bool gpu_hard_exceeded = gpu_hard_bytes > 0 && gpu_bytes >= gpu_hard_bytes;

    const std::string cpu_status = classify(cpu_bytes, cpu_soft_bytes, cpu_hard_bytes);
    const std::string gpu_status = classify(gpu_bytes, gpu_soft_bytes, gpu_hard_bytes);
    const std::string overall_status =
        severity_rank(cpu_status) >= severity_rank(gpu_status) ? cpu_status : gpu_status;

    if (auto status = replace_single<double>(space, base + "/cpuSoftBudgetRatio", cpu_soft_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/cpuHardBudgetRatio", cpu_hard_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuSoftBudgetRatio", gpu_soft_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuHardBudgetRatio", gpu_hard_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/cpuSoftExceeded", cpu_soft_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/cpuHardExceeded", cpu_hard_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/gpuSoftExceeded", gpu_soft_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/gpuHardExceeded", gpu_hard_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/cpuStatus", cpu_status); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/gpuStatus", gpu_status); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/overallStatus", overall_status); !status) {
        return status;
    }

    return {};
}

} // namespace SP::UI::Builders::Diagnostics
