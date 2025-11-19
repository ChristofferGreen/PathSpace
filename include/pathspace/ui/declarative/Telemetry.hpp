#pragma once

#include <pathspace/PathSpace.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace SP::UI::Declarative::Telemetry {

struct SchemaSample {
    std::string widget_path;
    std::string widget_kind;
    bool success = true;
    std::uint64_t duration_ns = 0;
    std::string error;
};

void RecordSchemaSample(PathSpace& space, SchemaSample const& sample);

struct FocusTransitionSample {
    std::string scene_path;
    std::string window_component;
    std::string previous_widget;
    std::string next_widget;
    bool wrapped = false;
    bool disabled_skip = false;
};

void RecordFocusTransition(PathSpace& space, FocusTransitionSample const& sample);
void RecordFocusDisabledSkip(PathSpace& space, std::string const& scene_path);
void IncrementFocusOwnership(PathSpace& space, std::string const& widget_path, bool acquired);

struct InputLatencySample {
    std::uint64_t latency_ns = 0;
    std::size_t backlog = 0;
};

void RecordInputLatency(PathSpace& space, InputLatencySample const& sample);

void AppendWidgetLog(PathSpace& space, std::string const& widget_path, std::string const& message);

struct RenderDirtySample {
    std::string scene_path;
    std::string widget_path;
    std::uint64_t duration_ns = 0;
};

void RecordRenderDirtySample(PathSpace& space, RenderDirtySample const& sample);

struct RenderPublishSample {
    std::string scene_path;
    std::uint64_t duration_ns = 0;
};

void RecordRenderPublishSample(PathSpace& space, RenderPublishSample const& sample);

struct RenderCompareSample {
    std::string scene_path;
    bool parity_ok = true;
    std::optional<float> diff_percent;
};

void RecordRenderCompareSample(PathSpace& space, RenderCompareSample const& sample);
void AppendRenderCompareLog(PathSpace& space, std::string const& scene_path, std::string const& message);

} // namespace SP::UI::Declarative::Telemetry

