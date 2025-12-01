#include "third_party/doctest.h"

#include <chrono>
#include <pathspace/inspector/InspectorRemoteMount.hpp>

using namespace SP::Inspector;

namespace {

auto make_remote_snapshot(std::string const& root) -> InspectorSnapshot {
    InspectorSnapshot snapshot;
    snapshot.options.root           = root;
    snapshot.options.max_children   = 8;
    snapshot.options.max_depth      = 3;
    snapshot.options.include_values = true;
    snapshot.root.path              = root;
    snapshot.root.value_type        = "object";
    snapshot.root.child_count       = 1;

    InspectorNodeSummary child;
    child.path          = root == "/" ? std::string{"/value"} : root + "/value";
    child.value_type    = "string";
    child.value_summary = "demo";
    child.child_count   = 0;

    snapshot.root.children.push_back(child);
    return snapshot;
}

InspectorSnapshot make_local_snapshot() {
    InspectorSnapshot snapshot;
    snapshot.options.root   = "/";
    snapshot.root.path      = "/";
    snapshot.root.value_type = "object";
    snapshot.root.child_count = 0;
    return snapshot;
}

} // namespace

TEST_CASE("RemoteMountRegistry augments local snapshot with placeholders") {
    RemoteMountOptions options;
    options.alias       = "alpha";
    options.access_hint = "Corp VPN required";
    RemoteMountRegistry registry({options});

    auto local = make_local_snapshot();
    registry.augmentLocalSnapshot(local);

    REQUIRE_FALSE(local.root.children.empty());
    auto remote_root = std::find_if(local.root.children.begin(), local.root.children.end(),
                                    [](InspectorNodeSummary const& node) {
                                        return node.path == "/remote";
                                    });
    REQUIRE(remote_root != local.root.children.end());
    REQUIRE_FALSE(remote_root->children.empty());
    CHECK_EQ(remote_root->children.front().path, "/remote/alpha");
    CHECK_EQ(remote_root->children.front().value_type, "remote");
    CHECK_NE(remote_root->children.front().value_summary.find("Corp VPN required"), std::string::npos);
}

TEST_CASE("RemoteMountRegistry builds remote snapshots with prefixing") {
    RemoteMountOptions options;
    options.alias = "alpha";
    options.root  = "/demo";
    RemoteMountRegistry registry({options});
    registry.updateSnapshot("alpha", make_remote_snapshot(options.root), std::chrono::milliseconds{5});

    InspectorSnapshotOptions request;
    request.root           = "/remote/alpha";
    request.max_depth      = 3;
    request.max_children   = 8;
    request.include_values = true;

    auto snapshot_or = registry.buildRemoteSnapshot(request);
    REQUIRE(snapshot_or.has_value());
    auto snapshot = snapshot_or->value();
    CHECK_EQ(snapshot.root.path, "/remote/alpha");
    REQUIRE_FALSE(snapshot.root.children.empty());
    CHECK_EQ(snapshot.root.children.front().path, "/remote/alpha/value");
    CHECK_EQ(snapshot.root.children.front().value_summary, "demo");
}

TEST_CASE("RemoteMountRegistry reports status metadata") {
    RemoteMountOptions options;
    options.alias       = "beta";
    options.access_hint = "Prod auth scope";
    RemoteMountRegistry registry({options});

    auto statuses = registry.statuses();
    REQUIRE_EQ(statuses.size(), 1U);
    CHECK_EQ(statuses.front().alias, "beta");
    CHECK_EQ(statuses.front().path, "/remote/beta");
    CHECK_EQ(statuses.front().access_hint, "Prod auth scope");
    CHECK_FALSE(statuses.front().connected);
}
