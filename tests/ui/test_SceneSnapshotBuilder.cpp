#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/RendererSnapshotStore.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace {

using namespace SP;
using namespace SP::UI::Runtime;
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
        bucket.command_offsets[i] = static_cast<std::uint32_t>(i);
        bucket.command_counts[i] = 1;
    }

    auto rect_size = sizeof(RectCommand);
    bucket.command_kinds.resize(commands);
    bucket.command_payload.resize(commands * rect_size);
    for (std::size_t i = 0; i < commands; ++i) {
        bucket.command_kinds[i] = static_cast<std::uint32_t>(DrawCommandKind::Rect);
        RectCommand rect{};
        rect.min_x = static_cast<float>(i);
        rect.min_y = static_cast<float>(i + 1);
        rect.max_x = static_cast<float>(i + 10);
        rect.max_y = static_cast<float>(i + 20);
        auto* dest = bucket.command_payload.data() + i * rect_size;
        std::memcpy(dest, &rect, rect_size);
    }

    bucket.opaque_indices = {0};
    bucket.alpha_indices  = {1};
    bucket.layer_indices.push_back(LayerIndices{.layer = 0, .indices = {0}});
    bucket.layer_indices.push_back(LayerIndices{.layer = 1, .indices = {1}});

    bucket.clip_nodes.push_back(ClipNode{
        .type = ClipNodeType::Rect,
        .next = -1,
        .rect = ClipRect{.min_x = 0.0f, .min_y = 0.0f, .max_x = 100.0f, .max_y = 50.0f},
        .path = {},
    });
    if (drawables > 1) {
        bucket.clip_nodes.push_back(ClipNode{
            .type = ClipNodeType::Path,
            .next = -1,
            .rect = {},
            .path = ClipPathReference{
                .command_offset = bucket.command_offsets[1],
                .command_count  = bucket.command_counts[1],
            },
        });
    }

    bucket.clip_head_indices.assign(drawables, -1);
    if (drawables > 0) {
        bucket.clip_head_indices[0] = 0;
    }
    if (drawables > 1) {
        bucket.clip_head_indices[1] = (bucket.clip_nodes.size() > 1) ? 1 : -1;
    }

    bucket.authoring_map.resize(drawables);
    for (std::size_t i = 0; i < drawables; ++i) {
        bucket.authoring_map[i].drawable_id = bucket.drawable_ids[i];
        bucket.authoring_map[i].authoring_node_id = "node-" + std::to_string(i);
        bucket.authoring_map[i].drawable_index_within_node = static_cast<std::uint32_t>(i);
        bucket.authoring_map[i].generation = 1;
    }

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
    CHECK(decodedBucket->clip_head_indices == bucket.clip_head_indices);
    REQUIRE(decodedBucket->clip_nodes.size() == bucket.clip_nodes.size());
    CHECK(decodedBucket->clip_nodes[0].type == bucket.clip_nodes[0].type);
    CHECK(decodedBucket->clip_nodes[0].rect.max_x == doctest::Approx(bucket.clip_nodes[0].rect.max_x));
    if (decodedBucket->clip_nodes.size() > 1) {
        CHECK(decodedBucket->clip_nodes[1].type == ClipNodeType::Path);
        CHECK(decodedBucket->clip_nodes[1].path.command_count == bucket.clip_nodes[1].path.command_count);
    }
    CHECK(decodedBucket->authoring_map.size() == bucket.authoring_map.size());
    CHECK(decodedBucket->authoring_map[0].authoring_node_id == bucket.authoring_map[0].authoring_node_id);
    CHECK(decodedBucket->authoring_map[0].drawable_index_within_node == bucket.authoring_map[0].drawable_index_within_node);
    CHECK(decodedBucket->drawable_fingerprints.size() == bucket.drawable_ids.size());
    for (auto fingerprint : decodedBucket->drawable_fingerprints) {
        CHECK(fingerprint != 0);
    }

    auto storedMeta = RendererSnapshotStore::instance().get_metadata(scene->getPath(), *revision);
    REQUIRE(storedMeta);
    CHECK(storedMeta->author == "tester");
    CHECK(storedMeta->tool_version == "unit-test");
    CHECK(storedMeta->drawable_count == bucket.drawable_ids.size());
    CHECK(storedMeta->command_count == bucket.command_kinds.size());
    CHECK(storedMeta->fingerprint_digests == opts.metadata.fingerprint_digests);

    auto storedBucket = RendererSnapshotStore::instance().get_bucket(scene->getPath(), *revision);
    REQUIRE(storedBucket);
    CHECK(storedBucket->drawable_fingerprints.size() == bucket.drawable_ids.size());

    // Renderer snapshots are no longer mirrored into PathSpace.
    auto storedDrawables = fx.space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/drawables.bin");
    CHECK_FALSE(storedDrawables);
    auto storedManifest = fx.space.read<std::vector<std::uint8_t>>(revisionBase + "/drawable_bucket");
    CHECK_FALSE(storedManifest);
    auto storedMetadata = fx.space.read<std::vector<std::uint8_t>>(revisionBase + "/metadata");
    CHECK_FALSE(storedMetadata);
}

TEST_CASE("drawable fingerprints remain stable when drawable id changes") {
    SnapshotFixture fx;

    SceneParams sceneParams{ .name = "fingerprint", .description = "Fingerprint stability" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene};

    auto format_revision = [](std::uint64_t revision) {
        std::ostringstream oss;
        oss << std::setw(16) << std::setfill('0') << revision;
        return oss.str();
    };

    auto base_bucket = make_bucket(1, 1);
    base_bucket.drawable_ids[0] = 1234;
    base_bucket.authoring_map[0].drawable_id = base_bucket.drawable_ids[0];

    SnapshotPublishOptions opts{};
    opts.metadata.author = "tests";
    opts.metadata.tool_version = "tests";
    opts.metadata.created_at = std::chrono::system_clock::now();
    opts.metadata.drawable_count = base_bucket.drawable_ids.size();
    opts.metadata.command_count = base_bucket.command_kinds.size();

    auto first_revision = builder.publish(opts, base_bucket);
    if (!first_revision) {
        auto const& err = first_revision.error();
        INFO("first publish failed: code=" << static_cast<int>(err.code)
             << " message=" << err.message.value_or("none"));
    }
    REQUIRE(first_revision);

    auto first_base = std::string(scene->getPath()) + "/builds/" + format_revision(*first_revision);
    auto decoded_first = SceneSnapshotBuilder::decode_bucket(fx.space, first_base);
    REQUIRE(decoded_first);
    REQUIRE(decoded_first->drawable_fingerprints.size() == base_bucket.drawable_ids.size());

    auto renamed_bucket = base_bucket;
    renamed_bucket.drawable_ids[0] = 5678;
    renamed_bucket.authoring_map[0].drawable_id = renamed_bucket.drawable_ids[0];

    opts.metadata.created_at += std::chrono::milliseconds{1};
    auto second_revision = builder.publish(opts, renamed_bucket);
    if (!second_revision) {
        auto const& err = second_revision.error();
        INFO("second publish failed: code=" << static_cast<int>(err.code)
             << " message=" << err.message.value_or("none"));
    }
    REQUIRE(second_revision);

    auto second_base = std::string(scene->getPath()) + "/builds/" + format_revision(*second_revision);
    auto decoded_second = SceneSnapshotBuilder::decode_bucket(fx.space, second_base);
    REQUIRE(decoded_second);
    REQUIRE(decoded_second->drawable_fingerprints.size() == renamed_bucket.drawable_ids.size());

    CHECK(decoded_first->drawable_ids != decoded_second->drawable_ids);
    CHECK(decoded_first->drawable_fingerprints == decoded_second->drawable_fingerprints);
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
    auto bucketRev2 = fx.space.read<std::vector<std::uint8_t>>(std::string(scene->getPath()) + "/builds/0000000000000002/drawable_bucket");
    CHECK_FALSE(bucketRev2);

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
    std::atomic<bool> publish_failed{false};
    std::mutex publish_error_mutex;
    std::vector<std::string> publish_errors;

    auto append_publish_error = [&](SP::Error const& error,
                                    int thread_id,
                                    int iteration) {
        std::ostringstream oss;
        oss << "publish error thread=" << thread_id
            << " iteration=" << iteration
            << " code=" << static_cast<int>(error.code)
            << " message=";
        if (error.message) {
            oss << *error.message;
        } else {
            oss << "<none>";
        }
        std::lock_guard<std::mutex> lock{publish_error_mutex};
        publish_errors.push_back(oss.str());
    };

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
            if (!rev) {
                publish_failed.store(true, std::memory_order_relaxed);
                append_publish_error(rev.error(), thread_id, i);
                return;
            }
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

    if (publish_failed.load(std::memory_order_relaxed)) {
        std::string merged;
        for (auto const& err : publish_errors) {
            merged.append(err);
            merged.push_back('\n');
        }
        REQUIRE_MESSAGE(false, merged);
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
    CHECK(metrics->total_fingerprint_count >= metrics->retained);
    CHECK(metrics->total_fingerprint_count <= metrics->retained * 4);
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
