#include "PathSpaceVisitTestSupport.hpp"

namespace {

class StubPodPayload final : public PodPayloadBase {
public:
    StubPodPayload(InputMetadata metadata,
                   std::vector<int> values,
                   std::optional<Error> readError,
                   std::optional<Error> spanError,
                   bool callSpan,
                   std::vector<std::size_t> spanCounts = {})
        : metadata_(std::move(metadata))
        , values_(std::move(values))
        , readError_(std::move(readError))
        , spanError_(std::move(spanError))
        , callSpan_(callSpan)
        , spanCounts_(std::move(spanCounts)) {}

    std::type_info const& type() const noexcept override { return typeid(int); }
    bool matches(std::type_info const& ti) const noexcept override { return ti == typeid(int); }
    std::size_t size() const noexcept override { return values_.size(); }
    std::size_t elementSize() const noexcept override { return sizeof(int); }
    std::size_t headIndex() const noexcept override { return 0; }
    std::size_t publishedTail() const noexcept override { return values_.size(); }
    InputMetadata const& podMetadata() const noexcept override { return metadata_; }

    bool pushValue(void const*) override { return false; }

    std::optional<Error> readTo(void* out) const override {
        if (readError_) {
            return readError_;
        }
        if (values_.empty()) {
            return Error{Error::Code::NoObjectFound, "No data available"};
        }
        *static_cast<int*>(out) = values_.front();
        return std::nullopt;
    }

    std::optional<Error> takeTo(void* out) override {
        return readTo(out);
    }

    std::optional<Error> withSpanRaw(std::function<void(void const*, std::size_t)> const& fn) const override {
        if (spanError_) {
            return spanError_;
        }
        if (!spanCounts_.empty()) {
            auto index = std::min(spanCallIndex_, spanCounts_.size() - 1);
            fn(values_.data(), spanCounts_[index]);
            ++spanCallIndex_;
        } else if (callSpan_) {
            fn(values_.data(), values_.size());
        }
        return std::nullopt;
    }

    std::optional<Error> withSpanMutableRaw(std::function<void(void*, std::size_t)> const& fn) override {
        if (callSpan_) {
            fn(values_.data(), values_.size());
        }
        return std::nullopt;
    }

    std::optional<Error> withSpanRawFrom(std::size_t startIndex,
                                         std::function<void(void const*, std::size_t)> const& fn) const override {
        if (spanError_) {
            return spanError_;
        }
        if (startIndex < values_.size() && callSpan_) {
            fn(values_.data() + startIndex, values_.size() - startIndex);
        }
        return std::nullopt;
    }

    std::optional<Error> withSpanMutableRawFrom(std::size_t startIndex,
                                                std::function<void(void*, std::size_t)> const& fn) override {
        if (startIndex < values_.size() && callSpan_) {
            fn(values_.data() + startIndex, values_.size() - startIndex);
        }
        return std::nullopt;
    }

    std::optional<Error> popCount(std::size_t) override { return std::nullopt; }
    bool freezeForUpgrade() override { return false; }
    std::optional<Reservation> reserveOne() override { return std::nullopt; }
    void publishOne(std::size_t) override {}
    void rollbackOne(std::size_t) override {}
    std::optional<ReservationSpan> reserveSpan(std::size_t) override { return std::nullopt; }
    void publishSpan(std::size_t, std::size_t) override {}
    void rollbackSpan(std::size_t, std::size_t) override {}
    std::optional<std::size_t> packSpanStart() const noexcept override { return std::nullopt; }
    void markPackSpanStart(std::size_t) noexcept override {}

private:
    InputMetadata             metadata_;
    std::vector<int>          values_;
    std::optional<Error>      readError_;
    std::optional<Error>      spanError_;
    bool                      callSpan_ = false;
    std::vector<std::size_t>  spanCounts_;
    mutable std::size_t       spanCallIndex_ = 0;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.visit.value_handle");

TEST_CASE("ValueHandle reports empty snapshot and queue depth for empty nodes") {
    PathSpace space;

    VisitOptions options;
    options.includeValues = true;

    bool sawRoot = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/") {
                sawRoot = true;
                CHECK(handle.queueDepth() == 0);
                auto snapshot = handle.snapshot();
                REQUIRE(snapshot);
                CHECK(snapshot->queueDepth == 0);
                CHECK(snapshot->types.empty());
                CHECK_FALSE(snapshot->hasExecutionPayload);
                CHECK_FALSE(snapshot->hasSerializedPayload);
                CHECK(snapshot->rawBufferBytes == 0);
            }
            return VisitControl::Continue;
        },
        options);
    CHECK(result);
    CHECK(sawRoot);
}

TEST_CASE("ValueHandle can read POD fast-path payloads during visit") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 10).errors.empty());
    REQUIRE(space.insert("/pod", 20).errors.empty());

    auto ok = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/pod") {
                return VisitControl::Continue;
            }
            auto snapshot = handle.snapshot();
            REQUIRE(snapshot);
            CHECK_FALSE(snapshot->hasSerializedPayload); // stays on POD fast path
            CHECK(snapshot->queueDepth == 2);

            auto value = handle.read<int>();
            CHECK(value);
            CHECK(*value == 10);
            return VisitControl::Stop;
        });
    REQUIRE(ok);

    auto first = space.take<int>("/pod");
    REQUIRE(first.has_value());
    CHECK(*first == 10);
    auto second = space.take<int>("/pod");
    REQUIRE(second.has_value());
    CHECK(*second == 20);
}

TEST_CASE("ValueHandle reports type mismatch for POD payload reads") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 10).errors.empty());

    VisitOptions options;
    options.includeValues = true;

    bool sawPod = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/pod") {
                sawPod = true;
                auto bad = handle.read<float>();
                CHECK_FALSE(bad.has_value());
                CHECK(bad.error().code == Error::Code::TypeMismatch);
            }
            return VisitControl::Continue;
        },
        options);
    CHECK(result);
    CHECK(sawPod);
}

TEST_CASE("ValueHandle snapshot reflects POD fast path upgrade to generic") {
    PathSpace space;
    REQUIRE(space.insert("/queue", 1).errors.empty());
    REQUIRE(space.insert("/queue", 2).errors.empty());

    ValueSnapshot before{};
    auto preVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/queue") {
                auto snapshot = handle.snapshot();
                REQUIRE(snapshot);
                before = *snapshot;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(preVisit);
    CHECK(before.queueDepth == 2);
    REQUIRE(before.types.size() == 2);
    CHECK(before.types[0].typeInfo == &typeid(int));
    CHECK(before.types[1].typeInfo == &typeid(int));

    // Upgrade the node by inserting a non-POD type.
    REQUIRE(space.insert("/queue", std::string("tail")).errors.empty());

    ValueSnapshot after{};
    auto postVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/queue") {
                auto snapshot = handle.snapshot();
                REQUIRE(snapshot);
                after = *snapshot;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(postVisit);
    CHECK(after.queueDepth >= 2);
    REQUIRE(after.types.size() >= 2);
    CHECK(after.hasSerializedPayload); // migrated off POD fast path
    bool hasInt = false;
    bool hasString = false;
    for (auto const& t : after.types) {
        hasInt |= (t.typeInfo == &typeid(int));
        hasString |= (t.typeInfo == &typeid(std::string));
    }
    CHECK(hasInt);
    CHECK(hasString);

    // Validate queue contents preserved in order after upgrade.
    auto first = space.take<int>("/queue");
    REQUIRE(first.has_value());
    CHECK(*first == 1);
    auto second = space.take<int>("/queue");
    REQUIRE(second.has_value());
    CHECK(*second == 2);
    auto tail = space.take<std::string>("/queue");
    REQUIRE(tail.has_value());
    CHECK(*tail == "tail");
}

TEST_CASE("ValueHandle snapshot handles nodes without payload") {
    PathSpace space;
    // Create a subtree so root has children but no payload.
    REQUIRE(space.insert("/root/child", 1).errors.empty());

    VisitOptions opts;
    opts.includeValues = true;
    bool sawRoot       = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/") {
                sawRoot = true;
                auto snap = handle.snapshot();
                REQUIRE(snap);
                CHECK(snap->queueDepth == 0);
                CHECK_FALSE(snap->hasSerializedPayload);
                CHECK_FALSE(snap->hasExecutionPayload);
            }
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(result);
    CHECK(sawRoot);
}

TEST_CASE("ValueHandle surfaces type mismatch for POD payloads") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 7).errors.empty());

    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/pod") {
                return VisitControl::Continue;
            }
            auto wrongType = handle.read<double>();
            CHECK_FALSE(wrongType);
            CHECK(wrongType.error().code == Error::Code::TypeMismatch);
            return VisitControl::Stop;
        });
    REQUIRE(visitResult);
}

TEST_CASE("ValueHandle read reports missing payload when node has no value") {
    PathSpace space;
    // Root has a child but no payload of its own.
    REQUIRE(space.insert("/root/child", 1).errors.empty());

    bool sawRoot = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/") {
                return VisitControl::Continue;
            }
            sawRoot = true;
            auto missing = handle.read<int>();
            CHECK_FALSE(missing);
            CHECK(missing.error().code == Error::Code::NoObjectFound);
            return VisitControl::Stop;
        });
    REQUIRE(result);
    CHECK(sawRoot);
}

TEST_CASE("ValueHandle read handles empty POD queues via snapshot fallback") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 1).errors.empty());

    // Drain the queue so the POD payload exists but holds no elements.
    auto first = space.take<int>("/pod");
    REQUIRE(first.has_value());
    auto second = space.take<int>("/pod");
    CHECK_FALSE(second.has_value());

    bool visitedPod = false;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/pod") {
                return VisitControl::Continue;
            }
            visitedPod = true;
            auto empty = handle.read<int>();
            CHECK_FALSE(empty);
            CHECK(empty.error().code == Error::Code::NoObjectFound);
            return VisitControl::Stop;
        });
    REQUIRE(visitResult);
    CHECK(visitedPod);
}

TEST_CASE("VisitDetail Access SerializeNodeData drops POD snapshot on serialization errors") {
    PathSpace space;
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);

    InputMetadata metadata{};
    metadata.typeInfo     = &typeid(int);
    metadata.dataCategory = DataCategory::Fundamental;
    auto payload = std::make_shared<StubPodPayload>(
        metadata,
        std::vector<int>{7},
        std::nullopt,
        std::nullopt,
        true);
    {
        std::lock_guard<std::mutex> guard(root->payloadMutex);
        root->podPayload = std::move(payload);
    }

    auto handle   = VisitDetail::Access::MakeHandle(space, *root, "/", true);
    auto snapshot = VisitDetail::Access::SerializeNodeData(handle);
    CHECK_FALSE(snapshot.has_value());
}

TEST_CASE("ValueHandle readInto falls back to span copy when read fails") {
    PathSpace space;
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);

    Node& node = root->getOrCreateChild("stub");
    InputMetadata metadata{InputMetadataT<int>{}};
    auto payload = std::make_shared<StubPodPayload>(
        metadata,
        std::vector<int>{42},
        Error{Error::Code::UnknownError, "forced read failure"},
        std::nullopt,
        true);
    {
        std::lock_guard<std::mutex> guard(node.payloadMutex);
        node.podPayload = std::move(payload);
    }

    std::optional<Expected<int>> result;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/stub") {
                result = handle.read<int>();
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });

    REQUIRE(visitResult);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->has_value());
    CHECK(result->error().code == Error::Code::UnknownError);
}

TEST_CASE("ValueHandle readInto surfaces span errors during fallback") {
    PathSpace space;
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);

    Node& node = root->getOrCreateChild("spanfail");
    InputMetadata metadata{InputMetadataT<int>{}};
    auto payload = std::make_shared<StubPodPayload>(
        metadata,
        std::vector<int>{7},
        Error{Error::Code::UnknownError, "forced read failure"},
        Error{Error::Code::UnknownError, "forced span failure"},
        false);
    {
        std::lock_guard<std::mutex> guard(node.payloadMutex);
        node.podPayload = std::move(payload);
    }

    std::optional<Expected<int>> result;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/spanfail") {
                result = handle.read<int>();
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });

    REQUIRE(visitResult);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->has_value());
    CHECK(result->error().code == Error::Code::UnknownError);
}

TEST_CASE("ValueHandle readInto reports deserialize failure after span fallback") {
    PathSpace space;
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);

    Node& node = root->getOrCreateChild("spanempty");
    InputMetadata metadata{};
    metadata.typeInfo     = &typeid(int);
    metadata.dataCategory = DataCategory::Fundamental;

    auto payload = std::make_shared<StubPodPayload>(
        metadata,
        std::vector<int>{9},
        Error{Error::Code::UnknownError, "forced read failure"},
        std::nullopt,
        false,
        std::vector<std::size_t>{0, 1});
    {
        std::lock_guard<std::mutex> guard(node.payloadMutex);
        node.podPayload = std::move(payload);
    }

    std::optional<Expected<int>> result;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/spanempty") {
                result = handle.read<int>();
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });

    REQUIRE(visitResult);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->has_value());
    CHECK(result->error().code == Error::Code::NoObjectFound);
}

TEST_CASE("ValueHandle readInto succeeds via snapshot fallback after read errors") {
    PathSpace space;
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);

    Node& node = root->getOrCreateChild("snapfallback");
    InputMetadata metadata{InputMetadataT<int>{}};
    auto payload = std::make_shared<StubPodPayload>(
        metadata,
        std::vector<int>{42},
        Error{Error::Code::UnknownError, "forced read failure"},
        std::nullopt,
        false,
        std::vector<std::size_t>{0, 1});
    {
        std::lock_guard<std::mutex> guard(node.payloadMutex);
        node.podPayload = std::move(payload);
    }

    std::optional<Expected<int>> result;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/snapfallback") {
                result = handle.read<int>();
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });

    REQUIRE(visitResult);
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(result->value() == 42);
}

TEST_CASE("ValueHandle queueDepth handles data, pod, and empty handles") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 5).errors.empty());
    REQUIRE(space.insert("/data", std::string{"value"}).errors.empty());

    std::size_t podDepth  = 0;
    std::size_t dataDepth = 0;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/pod") {
                podDepth = handle.queueDepth();
            } else if (entry.path == "/data") {
                dataDepth = handle.queueDepth();
            }
            return VisitControl::Continue;
        });
    REQUIRE(visitResult);

    CHECK(podDepth == 1);
    CHECK(dataDepth == 1);

    ValueHandle empty{};
    CHECK(empty.queueDepth() == 0);
}

TEST_CASE("ValueHandle readInto surfaces permission and missing-node errors") {
    PathSpace space;
    REQUIRE(space.insert("/root/value", 9).errors.empty());

    VisitOptions opts;
    opts.includeValues = false;
    int dest = 0;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/root/value") {
                auto err = handle.read<int>();
                REQUIRE_FALSE(err);
                CHECK(err.error().code == Error::Code::NotSupported);
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(visitResult);

    // Moved-from handles keep includeValues=true but lose backing node.
    std::optional<Expected<int>> movedErr;
    auto capture = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/root/value") {
                ValueHandle moved = std::move(handle);
                movedErr = handle.read<int>(); // moved-from handle
                (void)moved;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(capture);
    REQUIRE(movedErr.has_value());
    CHECK_FALSE(*movedErr);
    CHECK(movedErr->error().code == Error::Code::NotSupported);
}

TEST_CASE("SerializeNodeData snapshots node data and POD payloads") {
    PathSpace space;

    // Case 1: Serialize NodeData-backed payload.
    REQUIRE(space.insert("/nodes/alpha", std::string{"alpha"}).errors.empty());
    ValueHandle nodeDataHandle;
    auto nodeDataVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/nodes/alpha") {
                nodeDataHandle = handle;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(nodeDataVisit);
    auto nodeDataBytes = VisitDetail::Access::SerializeNodeData(nodeDataHandle);
    REQUIRE(nodeDataBytes.has_value());
    auto nodeDataSnapshot = NodeData::deserializeSnapshot(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(nodeDataBytes->data()), nodeDataBytes->size()});
    REQUIRE(nodeDataSnapshot);
    std::string recovered{};
    InputMetadata strMeta{InputMetadataT<std::string>{}};
    auto nodeDataErr = nodeDataSnapshot->deserialize(&recovered, strMeta);
    CHECK_FALSE(nodeDataErr);
    CHECK(recovered == "alpha");

    // Case 2: Serialize POD fast-path payload.
    REQUIRE(space.insert("/pod", 11).errors.empty());
    REQUIRE(space.insert("/pod", 22).errors.empty());
    ValueHandle podHandle;
    auto podVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/pod") {
                podHandle = handle;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(podVisit);
    auto podBytes = VisitDetail::Access::SerializeNodeData(podHandle);
    REQUIRE(podBytes.has_value());
    auto podSnapshot = NodeData::deserializeSnapshot(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(podBytes->data()), podBytes->size()});
    REQUIRE(podSnapshot);
    int frontValue = 0;
    InputMetadata intMeta{InputMetadataT<int>{}};
    auto podErr = podSnapshot->deserialize(&frontValue, intMeta);
    CHECK_FALSE(podErr);
    CHECK((frontValue == 11 || frontValue == 22));

    // Case 3: Invalid handle returns no snapshot.
    ValueHandle emptyHandle{};
    CHECK_FALSE(VisitDetail::Access::SerializeNodeData(emptyHandle).has_value());
}

TEST_SUITE_END();
