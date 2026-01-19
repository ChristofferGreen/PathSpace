#include "core/InsertReturn.hpp"
#include "core/Error.hpp"
#include "third_party/doctest.h"

TEST_SUITE("core.insertreturn") {

TEST_CASE("InsertReturn defaults to zeroed counters") {
    SP::InsertReturn result{};
    CHECK(result.nbrValuesInserted == 0);
    CHECK(result.nbrSpacesInserted == 0);
    CHECK(result.nbrTasksInserted == 0);
    CHECK(result.nbrValuesSuppressed == 0);
    CHECK(result.retargets.empty());
    CHECK(result.errors.empty());
}

TEST_CASE("InsertReturn captures retarget requests and errors") {
    SP::InsertReturn result{};

    SP::InsertReturn::RetargetRequest request{};
    request.space       = reinterpret_cast<SP::PathSpaceBase*>(0x1); // non-owning pointer
    request.mountPrefix = "/nested";
    result.retargets.push_back(request);

    result.errors.push_back(SP::Error{SP::Error::Code::UnknownError, "probe"});

    CHECK(result.retargets.size() == 1);
    CHECK(result.retargets.front().space == request.space);
    CHECK(result.retargets.front().mountPrefix == "/nested");

    CHECK(result.errors.size() == 1);
    CHECK(result.errors.front().code == SP::Error::Code::UnknownError);
    CHECK(result.errors.front().message.has_value());
    CHECK(result.errors.front().message.value() == "probe");
}

TEST_CASE("InsertReturn aggregates counters and preserves multiple retargets/errors") {
    SP::InsertReturn accumulator{};
    accumulator.nbrValuesInserted  = 1;
    accumulator.nbrSpacesInserted  = 2;
    accumulator.nbrTasksInserted   = 3;
    accumulator.nbrValuesSuppressed = 4;

    SP::InsertReturn::RetargetRequest first{};
    first.space       = reinterpret_cast<SP::PathSpaceBase*>(0x2);
    first.mountPrefix = "/alpha";
    SP::InsertReturn::RetargetRequest second{};
    second.space       = reinterpret_cast<SP::PathSpaceBase*>(0x3);
    second.mountPrefix = "/beta";
    accumulator.retargets.push_back(first);
    accumulator.retargets.push_back(second);

    accumulator.errors.push_back(SP::Error{SP::Error::Code::InvalidType, "bad-type"});
    accumulator.errors.push_back(SP::Error{SP::Error::Code::InvalidPath, "bad-path"});

    CHECK(accumulator.nbrValuesInserted == 1);
    CHECK(accumulator.nbrSpacesInserted == 2);
    CHECK(accumulator.nbrTasksInserted == 3);
    CHECK(accumulator.nbrValuesSuppressed == 4);
    CHECK(accumulator.retargets.size() == 2);
    CHECK(accumulator.errors.size() == 2);
    CHECK(accumulator.retargets.back().mountPrefix == "/beta");
    CHECK(accumulator.errors.front().code == SP::Error::Code::InvalidType);
}

} // TEST_SUITE
