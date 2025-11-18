#pragma once

#include <pathspace/PathSpace.hpp>

#include <chrono>
#include <string>

namespace SP::UI::Declarative {

struct PaintSurfaceUploaderOptions {
    std::chrono::milliseconds poll_interval{std::chrono::milliseconds{16}};
    std::string metrics_root = "/system/widgets/runtime/paint_gpu/metrics";
    std::string log_root = "/system/widgets/runtime/paint_gpu/log/errors/queue";
    std::string state_path = "/system/widgets/runtime/paint_gpu/state/running";
};

auto CreatePaintSurfaceUploader(PathSpace& space,
                                PaintSurfaceUploaderOptions const& options = {})
    -> SP::Expected<bool>;

auto ShutdownPaintSurfaceUploader(PathSpace& space) -> void;

} // namespace SP::UI::Declarative
