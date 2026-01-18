#include "PathSpace.hpp"
#include "inspector/InspectorSearchMetrics.hpp"

#include "third_party/doctest.h"

#include <cstdint>
#include <string>

using SP::Inspector::SearchMetricsRecorder;
using SP::Inspector::SearchQueryEvent;
using SP::Inspector::SearchWatchlistEvent;

TEST_SUITE("inspector.searchmetrics") {
TEST_CASE("Inspector search metrics publish counters") {
    SP::PathSpace        space;
    SearchMetricsRecorder recorder(space);

    SearchQueryEvent query{
        .latency_ms     = 12,
        .match_count    = 250,
        .returned_count = 200,
    };
    recorder.record_query(query);

    auto read_metric = [&](std::string const& path) -> std::uint64_t {
        auto metric = space.read<std::uint64_t>(path);
        REQUIRE(metric.has_value());
        return metric.value();
    };

    CHECK(read_metric("/inspector/metrics/search/queries/total") == 1);
    CHECK(read_metric("/inspector/metrics/search/queries/truncated_queries") == 1);

    auto snapshot = recorder.snapshot();
    CHECK(snapshot.queries.last_match_count == 250);
    CHECK(snapshot.queries.last_returned_count == 200);
    CHECK(snapshot.queries.truncated_results_total == 50);

    SearchWatchlistEvent watch{
        .live         = 2,
        .missing      = 1,
        .truncated    = 1,
        .out_of_scope = 1,
        .unknown      = 0,
    };
    recorder.record_watchlist(watch);

    CHECK(read_metric("/inspector/metrics/search/watch/live") == 2);
    CHECK(read_metric("/inspector/metrics/search/watch/total") == 4);

    auto snapshot_after = recorder.snapshot();
    CHECK(snapshot_after.watch.live == 2);
    CHECK(snapshot_after.watch.total == 4);
}
}
