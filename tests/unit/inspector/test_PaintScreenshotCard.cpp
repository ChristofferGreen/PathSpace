#include "third_party/doctest.h"

#include "PathSpace.hpp"
#include "inspector/PaintScreenshotCard.hpp"

#include <filesystem>
#include <fstream>

using namespace SP;
using namespace SP::Inspector;

TEST_SUITE("inspector.paint.screenshot.card") {

TEST_CASE("classifies healthy run from diagnostics tree") {
    PathSpace space;
    auto const root = "/diagnostics/ui/paint_example/screenshot_baseline";
    REQUIRE(space.insert(std::string(root) + "/manifest_revision", static_cast<std::int64_t>(5)).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/tag", std::string("paint_1280")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/sha256", std::string("abc123")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/width", static_cast<std::int64_t>(1280)).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/height", static_cast<std::int64_t>(800)).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/renderer", std::string("metal")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/captured_at", std::string("2025-11-21T12:00:00Z")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/commit", std::string("abc")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/notes", std::string("smoke")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/tolerance", 0.0015).errors.empty());

    auto const last_run = std::string(root) + "/last_run";
    REQUIRE(space.insert(last_run + "/timestamp_ns", static_cast<std::int64_t>(1234)).errors.empty());
    REQUIRE(space.insert(last_run + "/status", std::string("match")).errors.empty());
    REQUIRE(space.insert(last_run + "/hardware_capture", true).errors.empty());
    REQUIRE(space.insert(last_run + "/require_present", true).errors.empty());
    REQUIRE(space.insert(last_run + "/mean_error", 0.0008).errors.empty());
    REQUIRE(space.insert(last_run + "/max_channel_delta", static_cast<std::int64_t>(2)).errors.empty());
    REQUIRE(space.insert(last_run + "/screenshot_path", std::string("build/artifacts/latest.png")).errors.empty());
    REQUIRE(space.insert(last_run + "/diff_path", std::string{}).errors.empty());

    auto card = BuildPaintScreenshotCard(space);
    REQUIRE(card);
    CHECK(card->severity == PaintScreenshotSeverity::Healthy);
    REQUIRE(card->last_run);
    CHECK(card->last_run->status == std::optional<std::string>{"match"});
}

TEST_CASE("loads fallback diagnostics json") {
    auto temp = std::filesystem::temp_directory_path() / "paint_card_test.json";
    std::ofstream stream(temp, std::ios::trunc);
    REQUIRE(stream.is_open());
    stream << R"({
  "schema_version": 1,
  "generated_at": "2025-11-21T12:12:00Z",
  "runCount": 1,
  "runs": [
    {
      "source": "build/artifacts/paint_example/paint_720_metrics.json",
      "timestamp_ns": 123,
      "timestamp_iso": "2025-11-21T12:12:00Z",
      "tag": "paint_720",
      "manifest_revision": 3,
      "sha256": "def",
      "renderer": "metal",
      "width": 1280,
      "height": 720,
      "status": "match",
      "hardware_capture": true,
      "require_present": true,
      "mean_error": 0.001,
      "max_channel_delta": 12,
      "screenshot_path": "docs/images/paint_example_720_baseline.png",
      "diff_path": "",
      "ok": true
    }
  ]
})";
    stream.close();

    auto runs = LoadPaintScreenshotRunsFromJson(temp, 5);
    REQUIRE(runs);
    CHECK(runs->size() == 1);

    PaintScreenshotCardOptions opts{};
    opts.max_runs = 5;
    auto card = BuildPaintScreenshotCardFromRuns(*runs, opts);
    CHECK(card.severity == PaintScreenshotSeverity::Healthy);
    REQUIRE(card.last_run);
    CHECK(card.last_run->mean_error);
    CHECK(*card.last_run->mean_error == doctest::Approx(0.001));

    std::filesystem::remove(temp);
}

TEST_CASE("serializes card to json") {
    PathSpace space;
    auto const root = "/diagnostics/ui/paint_example/screenshot_baseline";
    REQUIRE(space.insert(std::string(root) + "/manifest_revision", static_cast<std::int64_t>(7)).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/tag", std::string("paint_1280")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/sha256", std::string("abc")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/width", static_cast<std::int64_t>(1280)).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/height", static_cast<std::int64_t>(800)).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/renderer", std::string("metal")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/captured_at", std::string("2025-11-21T12:00:00Z")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/commit", std::string("deadbeef")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/notes", std::string("smoke")).errors.empty());
    REQUIRE(space.insert(std::string(root) + "/tolerance", 0.0015).errors.empty());

    auto const last_run = std::string(root) + "/last_run";
    REQUIRE(space.insert(last_run + "/timestamp_ns", static_cast<std::int64_t>(55)).errors.empty());
    REQUIRE(space.insert(last_run + "/status", std::string("captured")).errors.empty());
    REQUIRE(space.insert(last_run + "/hardware_capture", true).errors.empty());
    REQUIRE(space.insert(last_run + "/require_present", true).errors.empty());
    REQUIRE(space.insert(last_run + "/mean_error", 0.0004).errors.empty());
    REQUIRE(space.insert(last_run + "/max_channel_delta", static_cast<std::int64_t>(1)).errors.empty());
    REQUIRE(space.insert(last_run + "/screenshot_path", std::string("docs/images/paint_example_baseline.png")).errors.empty());
    REQUIRE(space.insert(last_run + "/diff_path", std::string{}).errors.empty());

    auto card = BuildPaintScreenshotCard(space);
    REQUIRE(card);
    auto json_text = SerializePaintScreenshotCard(*card, 0);
    CHECK(json_text.find("\"severity\":\"healthy\"") != std::string::npos);
    CHECK(json_text.find("\"revision\":7") != std::string::npos);
    CHECK(json_text.find("\"status\":\"captured\"") != std::string::npos);
}

} // TEST_SUITE
