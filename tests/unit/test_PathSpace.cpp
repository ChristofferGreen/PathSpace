#include "ext/doctest.h"
#include <pathspace/core/Capabilities.hpp>
#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_CASE("PathSpace Construction") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Construction") {
        CHECK(pspace.insert("/test", 54).value_or(0) == 1);
    }

    /*SUBCASE("Simple PathSpace Construction JSON") {
        CHECK(pspace.insert("/test", 54).value_or(0) == 1);        
        CHECK(pspace.toJSON(false) == R"({"PathSpace": {"value0": {"test": {"index": 0,"data": {"value0": {"container": [54,0,0,0]}}}}})" );
    }*/

    SUBCASE("Simple PathSpace Path Into Data") {
        CHECK(pspace.insert("/test", 54).value_or(0) == 1);
        auto const val = pspace.insert("/test/data", 55);
        CHECK(!val.has_value());
        CHECK(val.error().code==Error::Code::InvalidPathSubcomponent);
    }

    SUBCASE("PathSpace Multi-Component Path") {
        CHECK(pspace.insert("/test1/test2/data", 56).value_or(0) == 1);
        //CHECK(pspace.toJSON(false) == R"({"PathSpace": {"value0": {"test3": {"index": 1,"data": {"ptr_wrapper": {"valid": 1,"data": {"value0": {"test": {"index": 1,"data": {"ptr_wrapper": {"valid": 1,"data": {"value0": {"data": {"index": 0,"data": {"value0": {"container": [56,0,0,0]}}}}}}}}}}}}}}})" );
    }
}

TEST_CASE("PathSpace Read") {
    SUBCASE("Simple PathSpace Read") {
        PathSpace pspace;
        pspace.insert("/test", 56);
        pspace.insert("/test", 58);
        auto ret = pspace.read<int>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value()==56);
        auto ret2 = pspace.read<int>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value()==56);
    }

    SUBCASE("Deeper PathSpace Read") {
        PathSpace pspace;
        pspace.insert("/test1/test2", 56);
        pspace.insert("/test1/test2", 58);
        auto ret = pspace.read<int>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value()==56);
        auto ret2 = pspace.read<int>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value()==56);
    }
}

TEST_CASE("PathSpace Grab") {
    SUBCASE("Simple PathSpace Grab") {
        PathSpace pspace;
        pspace.insert("/test", 56);
        pspace.insert("/test", 58);
        auto ret = pspace.grab<int>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value()==56);
        auto ret2 = pspace.grab<int>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value()==58);
    }

    SUBCASE("Deeper PathSpace Grab") {
        PathSpace pspace;
        pspace.insert("/test1/test2", 56);
        pspace.insert("/test1/test2", 58);
        auto ret = pspace.grab<int>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value()==56);
        auto ret2 = pspace.grab<int>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value()==58);
    }

    SUBCASE("Deeper PathSpace Grab Different Types") {
        PathSpace pspace;
        pspace.insert("/test1/test2", 56.45f);
        pspace.insert("/test1/test2", 'a');
        pspace.insert("/test1/test2", 34.5f);
        auto ret = pspace.grab<float>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value()==56.45f);
        auto ret2 = pspace.grab<char>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value()=='a');
        auto ret3 = pspace.grab<float>("/test1/test2");
        CHECK(ret3.has_value());
        CHECK(ret3.value()==34.5f);
    }
}