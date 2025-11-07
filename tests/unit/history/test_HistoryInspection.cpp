#include "history/UndoHistoryInspection.hpp"

#include "history/CowSubtreePrototype.hpp"
#include "core/NodeData.hpp"
#include "type/InputData.hpp"
#include "third_party/doctest.h"

using namespace SP;
using namespace SP::History;

namespace {

auto makePayload(std::string const& value) -> std::vector<std::byte> {
    InputData input(value);
    NodeData  node(input);
    auto      bytes = node.serializeSnapshot();
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

} // namespace

TEST_SUITE("UndoHistoryInspection") {

TEST_CASE("decodeSnapshot decodes serialized payloads") {
    CowSubtreePrototype prototype;
    auto base = prototype.emptySnapshot();

    CowSubtreePrototype::Mutation mut;
    mut.components = {"title"};
    mut.payload    = CowSubtreePrototype::Payload(makePayload("alpha"));

    auto snapshot = prototype.apply(base, mut);

    auto summary = Inspection::decodeSnapshot(snapshot, "/doc");
    REQUIRE(summary.values.size() == 1);
    auto const& value = summary.values.front();
    INFO("typeName=" << value.typeName);
    INFO("category=" << value.category);
    CHECK(value.path == "/doc/title");
    CHECK(value.summary == "\"alpha\"");
    CHECK(value.bytes > 0);
    CHECK(!value.typeName.empty());
}

TEST_CASE("diffSnapshots reports modified payloads") {
    CowSubtreePrototype prototype;
    auto base = prototype.emptySnapshot();

    CowSubtreePrototype::Mutation mut;
    mut.components = {"title"};
    mut.payload    = CowSubtreePrototype::Payload(makePayload("alpha"));
    auto snapshotA = prototype.apply(base, mut);

    CowSubtreePrototype::Mutation mut2;
    mut2.components = {"title"};
    mut2.payload    = CowSubtreePrototype::Payload(makePayload("beta"));
    auto snapshotB  = prototype.apply(snapshotA, mut2);

    auto diff = Inspection::diffSnapshots(snapshotA, snapshotB, "/doc");
    CHECK(diff.added.empty());
    CHECK(diff.removed.empty());
    REQUIRE(diff.modified.size() == 1);
    auto const& change = diff.modified.front();
    INFO("before.typeName=" << change.before.typeName);
    INFO("after.typeName=" << change.after.typeName);
    CHECK(change.before.path == "/doc/title");
    CHECK(change.after.summary == "\"beta\"");
    CHECK(change.before.summary == "\"alpha\"");
}

TEST_CASE("historyStatsToJson and lastOperationToJson") {
    HistoryStats stats;
    stats.counts.undo                 = 2;
    stats.counts.redo                 = 1;
    stats.bytes.undo                  = 1024;
    stats.bytes.redo                  = 512;
    stats.bytes.live                  = 128;
    stats.bytes.total                 = 1664;
    stats.bytes.disk                  = 2048;
    stats.counts.manualGarbageCollect = true;
    stats.trim.operationCount         = 3;
    stats.trim.entries                = 4;
    stats.trim.bytes                  = 256;
    stats.trim.lastTimestampMs        = 123456;
    stats.counts.diskEntries          = 7;
    stats.counts.cachedUndo           = 2;
    stats.counts.cachedRedo           = 1;

    auto statsJson = Inspection::historyStatsToJson(stats);
    CHECK(statsJson.find("\"undoCount\": 2") != std::string::npos);
    CHECK(statsJson.find("\"manualGcEnabled\": true") != std::string::npos);
    CHECK(statsJson.find("\"diskBytes\": 2048") != std::string::npos);

    HistoryLastOperation op;
    op.type            = "insert";
    op.timestampMs     = 42;
    op.durationMs      = 17;
    op.success         = true;
    op.undoCountBefore = 1;
    op.undoCountAfter  = 2;
    op.redoCountBefore = 0;
    op.redoCountAfter  = 0;
    op.bytesBefore     = 256;
    op.bytesAfter      = 512;
    op.message         = "ok";

    auto opJson = Inspection::lastOperationToJson(op);
    CHECK(opJson.find("\"type\":\"insert\"") != std::string::npos);
    CHECK(opJson.find("\"success\":true") != std::string::npos);
    CHECK(opJson.find("\"bytesAfter\":512") != std::string::npos);
}

} // TEST_SUITE
