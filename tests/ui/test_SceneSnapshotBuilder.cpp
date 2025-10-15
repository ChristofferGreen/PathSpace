#include "ext/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <atomic>
#include <chrono>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace SP;
using namespace SP::UI::Builders;
using namespace SP::UI::Scene;

struct SnapshotFixture {
    PathSpace   space;
    AppRootPath app_root{"/system/applications/test_app"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }
};

auto make_bucket(std::size_t drawables, std::size_t commands) -> DrawableBucketSnapshot {
    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids.resize(drawables);
    bucket.world_transforms.resize(drawables);
    bucket.bounds_spheres.resize(drawables);
    bucket.bounds_boxes.resize(drawables);
    bucket.bounds_box_valid.resize(drawables, 1);
    bucket.layers.resize(drawables);
    bucket.z_values.resize(drawables);
    bucket.material_ids.resize(drawables);
    bucket.pipeline_flags.resize(drawables);
    bucket.visibility.resize(drawables);
    bucket.command_offsets.resize(drawables);
    bucket.command_counts.resize(drawables);

    for (std::size_t i = 0; i < drawables; ++i) {
        bucket.drawable_ids[i] = static_cast<std::uint64_t>(100 + i);
        auto& transform = bucket.world_transforms[i];
        for (std::size_t j = 0; j < transform.elements.size(); ++j) {
            transform.elements[j] = (j % 5 == 0) ? 1.0f : 0.1f * static_cast<float>(i + j);
        }
        auto& sphere = bucket.bounds_spheres[i];
        sphere.center = {static_cast<float>(i), static_cast<float>(i + 1), static_cast<float>(i + 2)};
        sphere.radius = 10.0f + static_cast<float>(i);
        auto& box = bucket.bounds_boxes[i];
        box.min = {static_cast<float>(i), static_cast<float>(i) + 0.5f, static_cast<float>(i) + 1.0f};
        box.max = {static_cast<float>(i) + 2.0f, static_cast<float>(i) + 2.5f, static_cast<float>(i) + 3.0f};
        bucket.layers[i] = static_cast<std::uint32_t>(i % 4);
        bucket.z_values[i] = static_cast<float>(i) * 0.5f;
        bucket.material_ids[i] = static_cast<std::uint32_t>(200 + i);
        bucket.pipeline_flags[i] = static_cast<std::uint32_t>(300 + i);
        bucket.visibility[i] = static_cast<std::uint8_t>(i % 2);
        bucket.command_offsets[i] = static_cast<std::uint32_t>(i * 2);
        bucket.command_counts[i] = 1;
    }

    bucket.command_kinds.resize(commands);
    bucket.command_payload.resize(commands, 42);
    for (std::size_t i = 0; i < commands; ++i) {
        bucket.command_kinds[i] = static_cast<std::uint32_t>(i % 3);
    }

    bucket.opaque_indices = {0};
    bucket.alpha_indices  = {1};
    bucket.layer_indices.push_back(LayerIndices{.layer = 0, .indices = {0}});
    bucket.layer_indices.push_back(LayerIndices{.layer = 1, .indices = {1}});
    return bucket;
}

} // namespace

TEST_SUITE("SceneSnapshotBuilder") {

TEST_CASE("publish snapshot encodes bucket and metadata") {
    SnapshotFixture fx;

    SceneParams sceneParams{ .name = "main", .description = "Main scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    SnapshotRetentionPolicy policy{ .min_revisions = 2, .min_duration = std::chrono::milliseconds{0} };
    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene, policy};

    auto bucket = make_bucket(2, 3);

    SnapshotPublishOptions opts{};
    opts.metadata.author = "tester";
    opts.metadata.tool_version = "unit-test";
    opts.metadata.created_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{1234567}};
    opts.metadata.fingerprint_digests = {"atlas:abcd", "mesh:ef01"};

    auto revision = builder.publish(opts, bucket);
    REQUIRE(revision);
    CHECK(*revision == 1);

    auto revisionBase = std::string(scene->getPath()) + "/builds/0000000000000001";

    auto decodedBucket = SceneSnapshotBuilder::decode_bucket(fx.space, revisionBase);
    REQUIRE(decodedBucket);
    CHECK(decodedBucket->drawable_ids == bucket.drawable_ids);
    CHECK(decodedBucket->world_transforms.size() == bucket.world_transforms.size());
    CHECK(decodedBucket->world_transforms.front().elements == bucket.world_transforms.front().elements);
    CHECK(decodedBucket->bounds_spheres.front().radius == doctest::Approx(bucket.bounds_spheres.front().radius));
    CHECK(decodedBucket->bounds_boxes.front().min == bucket.bounds_boxes.front().min);
    CHECK(decodedBucket->bounds_box_valid == bucket.bounds_box_valid);
    CHECK(decodedBucket->material_ids == bucket.material_ids);
    CHECK(decodedBucket->pipeline_flags == bucket.pipeline_flags);
    CHECK(decodedBucket->command_offsets == bucket.command_offsets);
    CHECK(decodedBucket->command_kinds == bucket.command_kinds);

    auto rawMeta = fx.space.read<std::vector<std::uint8_t>>(revisionBase + "/metadata");
    REQUIRE(rawMeta);
    auto metaSpan = std::span<const std::byte>{reinterpret_cast<const std::byte*>(rawMeta->data()), rawMeta->size()};
    auto decodedMeta = SceneSnapshotBuilder::decode_metadata(metaSpan);
    REQUIRE(decodedMeta);
    CHECK(decodedMeta->author == "tester");
    CHECK(decodedMeta->tool_version == "unit-test");
    CHECK(decodedMeta->drawable_count == bucket.drawable_ids.size());
    CHECK(decodedMeta->command_count == bucket.command_kinds.size());
    CHECK(decodedMeta->fingerprint_digests == opts.metadata.fingerprint_digests);

    auto storedDrawables = fx.space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/drawables.bin");
    REQUIRE(storedDrawables);
    CHECK_FALSE(storedDrawables->empty());
}

TEST_CASE("publish enforces retention policy") {
    SnapshotFixture fx;

    SceneParams sceneParams{ .name = "main", .description = "Main scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    SnapshotRetentionPolicy policy{ .min_revisions = 2, .min_duration = std::chrono::milliseconds{0} };
    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene, policy};

    auto publish_with_time = [&](std::chrono::milliseconds timestamp) -> std::uint64_t {
        auto bucket = make_bucket(1, 1);
        SnapshotPublishOptions opts{};
        opts.metadata.author = "tester";
        opts.metadata.tool_version = "unit-test";
        opts.metadata.created_at = std::chrono::system_clock::time_point{timestamp};
        auto rev = builder.publish(opts, bucket);
        REQUIRE(rev);
        return *rev;
    };

    auto rev1 = publish_with_time(std::chrono::milliseconds{1000});
    auto rev2 = publish_with_time(std::chrono::milliseconds{2000});
    auto rev3 = publish_with_time(std::chrono::milliseconds{3000});

    CHECK(rev1 == 1);
    CHECK(rev2 == 2);
    CHECK(rev3 == 3);

    auto bucketRev1 = fx.space.read<std::vector<std::uint8_t>>(std::string(scene->getPath()) + "/builds/0000000000000001/drawable_bucket");
    CHECK_FALSE(bucketRev1);

    auto descRev2 = fx.space.read<std::vector<std::uint8_t>>(std::string(scene->getPath()) + "/builds/0000000000000002/drawable_bucket");
    REQUIRE(descRev2);

    auto records = builder.snapshot_records();
    REQUIRE(records);
    CHECK(records->size() == 2);
    CHECK(records->back().revision == 3);

    auto current = fx.space.read<std::uint64_t>(std::string(scene->getPath()) + "/current_revision");
    REQUIRE(current);
    CHECK(*current == 3);
}

TEST_CASE("rapid publishes maintain retention and latest revision") {
    SnapshotFixture fx;

    SceneParams sceneParams{ .name = "burst", .description = "Burst scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    SnapshotRetentionPolicy policy{ .min_revisions = 3, .min_duration = std::chrono::milliseconds{0} };
    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene, policy};

    constexpr int kThreads = 4;
    constexpr int kPublishesPerThread = 4;
    std::atomic<int> counter{0};

    auto publish_worker = [&](int thread_id) {
        for (int i = 0; i < kPublishesPerThread; ++i) {
            auto bucket = make_bucket(2, 2);
            SnapshotPublishOptions opts{};
            opts.metadata.author = "stress";
            opts.metadata.tool_version = "loop";
            auto seq = counter.fetch_add(1, std::memory_order_relaxed);
            opts.metadata.created_at = std::chrono::system_clock::time_point{
                std::chrono::milliseconds{1'000 + 5 * seq + thread_id}
            };
            auto rev = builder.publish(opts, bucket);
            REQUIRE(rev);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(publish_worker, t);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    auto pruneStatus = builder.prune();
    REQUIRE(pruneStatus);

    auto records = builder.snapshot_records();
    REQUIRE(records);
    CHECK(records->size() == 3);
    CHECK(records->back().revision == kThreads * kPublishesPerThread);

    // Earliest retained revision should reflect min revision retention (keep latest 3)
    CHECK(records->front().revision == kThreads * kPublishesPerThread - 2);

    auto metrics = fx.space.read<SnapshotGcMetrics>(std::string(scene->getPath()) + "/metrics/snapshots/state");
    REQUIRE(metrics);
    CHECK(metrics->retained == 3);
    CHECK(metrics->last_revision == kThreads * kPublishesPerThread);
    CHECK(metrics->total_fingerprint_count == 0);
}

TEST_CASE("long running publishes keep metrics stable over time") {
    SnapshotFixture fx;

    SceneParams sceneParams{ .name = "longrun", .description = "Long run scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    SnapshotRetentionPolicy policy{ .min_revisions = 2, .min_duration = std::chrono::milliseconds{0} };
    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene, policy};

    for (int i = 0; i < 10; ++i) {
        auto bucket = make_bucket(1, 1);
        SnapshotPublishOptions opts{};
        opts.metadata.author = "loop";
        opts.metadata.tool_version = "series";
        opts.metadata.created_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{1'500 + i}};
        opts.metadata.fingerprint_digests = {"atlas:" + std::to_string(i)};

        auto rev = builder.publish(opts, bucket);
        REQUIRE(rev);
        CHECK(*rev == static_cast<std::uint64_t>(i + 1));

        if ((i % 3) == 2) {
            auto pruneStatus = builder.prune();
            REQUIRE(pruneStatus);
        }

        auto metrics = fx.space.read<SnapshotGcMetrics>(std::string(scene->getPath()) + "/metrics/snapshots/state");
        REQUIRE(metrics);
        CHECK(metrics->last_revision == static_cast<std::uint64_t>(i + 1));
        CHECK(metrics->retained <= 2);
        CHECK(metrics->retained >= 1);
        CHECK(metrics->total_fingerprint_count >= metrics->retained);
        CHECK(metrics->total_fingerprint_count <= metrics->retained * 5);
    }
}

TEST_CASE("bucket validation catches mismatched arrays") {
    SnapshotFixture fx;

    SceneParams sceneParams{ .name = "main", .description = "Main scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene, SnapshotRetentionPolicy{}};

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {1, 2};
    bucket.layers = {10};
    bucket.z_values = {0.5f, 1.0f};
    bucket.visibility = {1, 0};

    SnapshotPublishOptions opts{};
    opts.metadata.author = "tester";
    opts.metadata.tool_version = "unit-test";

    auto result = builder.publish(opts, bucket);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidType);
}

} // TEST_SUITE
