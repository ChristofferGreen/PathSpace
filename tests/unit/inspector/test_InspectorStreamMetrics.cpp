#include "PathSpace.hpp"
#include "inspector/InspectorStreamMetrics.hpp"

#include "third_party/doctest.h"

#include <cstdint>
#include <string>

TEST_CASE("Inspector stream metrics publish counters") {
    SP::PathSpace space;
    SP::Inspector::StreamMetricsRecorder recorder(space);

    recorder.record_session_started();
    auto snapshot = recorder.snapshot();
    CHECK(snapshot.active_sessions == 1);
    CHECK(snapshot.total_sessions == 1);

    auto read_metric = [&](std::string const& path) -> std::uint64_t {
        auto metric = space.read<std::uint64_t>(path);
        REQUIRE(metric.has_value());
        return metric.value();
    };

    recorder.record_queue_depth(5);
    CHECK(read_metric("/inspector/metrics/stream/queue_depth") == 5);

    recorder.record_drop(3);
    CHECK(read_metric("/inspector/metrics/stream/dropped") == 3);

    recorder.record_snapshot_resent();
    CHECK(read_metric("/inspector/metrics/stream/resent") == 1);

    recorder.record_session_ended(SP::Inspector::StreamDisconnectReason::Backpressure);
    snapshot = recorder.snapshot();
    CHECK(snapshot.active_sessions == 0);
    CHECK(snapshot.disconnect_backpressure == 1);

    CHECK(read_metric("/inspector/metrics/stream/disconnect/backpressure") == 1);
}
