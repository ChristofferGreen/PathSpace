#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Capabilities.hpp>

using namespace SP;
int aert = 5;

TEST_CASE("PathSpace Insert") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Construction") {
        CHECK(pspace.insert("/test", 54).nbrValuesInserted == 1);
    }

    /*SUBCASE("Simple PathSpace Construction JSON") {
        CHECK(pspace.insert("/test", 54).value_or(0) == 1);
        CHECK(pspace.toJSON(false) == R"({"PathSpace": {"value0": {"test": {"index": 0,"data": {"value0": {"container":
    [54,0,0,0]}}}}})" );
    }*/

    SUBCASE("Simple PathSpace Path Into Data") {
        CHECK(pspace.insert("/test", 54).nbrValuesInserted == 1);
        auto const val = pspace.insert("/test/data", 55);
        CHECK(val.nbrValuesInserted == 0);
        CHECK(val.errors.size() == 1);
        CHECK(val.errors[0].code == Error::Code::InvalidPathSubcomponent);
    }

    SUBCASE("PathSpace Multi-Component Path") {
        CHECK(pspace.insert("/test1/test2/data", 56).nbrValuesInserted == 1);
        // CHECK(pspace.toJSON(false) == R"({"PathSpace": {"value0": {"test3": {"index": 1,"data": {"ptr_wrapper":
        // {"valid": 1,"data": {"value0": {"test": {"index": 1,"data": {"ptr_wrapper": {"valid": 1,"data": {"value0":
        // {"data": {"index": 0,"data": {"value0": {"container": [56,0,0,0]}}}}}}}}}}}}}}})" );
    }

    SUBCASE("Simple PathSpace Glob Construction") {
        CHECK(pspace.insert("/test1", 1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test2", 2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/tast1", 3).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test*", 4).nbrValuesInserted == 2);
    }

    SUBCASE("Middle PathSpace Glob Construction") {
        CHECK(pspace.insert("/test1/test", 1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test2/test", 2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test3/test", 3).nbrValuesInserted == 1);
        CHECK(pspace.insert("/tast1", 4).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test*/moo", 5).nbrValuesInserted == 3);
    }

    SUBCASE("Simple PathSpace Insert Function Pointer") {
        int (*f)() = []() -> int { return 58; };
        CHECK(pspace.insert("/f", f).nbrValuesInserted == 1);
    }

    SUBCASE("Simple PathSpace Insert Lambda") {
        // CHECK(pspace.insert("/test1", [](ConcretePathString const &path, PathSpace &space, std::atomic<bool> &alive)
        // -> int { return 367; }).nbrValuesInserted == 1);
    }
}

// lambdas should come from a central database in order to support serialization to remote computer
/*TEST_CASE("PathSpace Insert Lambda") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Lambda Insert") {
        CHECK(pspace.insert("/test1", [](){}).nbrValuesInserted == 1);
    }
}*/
static PathSpace pspaceg;
TEST_CASE("PathSpace Read") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Read") {
        CHECK(pspace.insert("/test", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test", 58).nbrValuesInserted == 1);
        auto ret = pspace.read<int>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.read<int>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 56);
    }

    SUBCASE("Deeper PathSpace Read") {
        CHECK(pspace.insert("/test1/test2", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 58).nbrValuesInserted == 1);
        auto ret = pspace.read<int>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.read<int>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 56);
    }

    SUBCASE("Simple PathSpace Read Function Pointer Execution") {
        using TestFuncPtr = int (*)();
        TestFuncPtr f = []() -> int { return 58; };
        TestFuncPtr f2 = []() -> int { return 25; };
        CHECK(pspace.insert("/f", f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/f2", f2).nbrValuesInserted == 1);
        CHECK(pspace.read<int>("/f").value() == 58);
        CHECK(pspace.read<int>("/f").value() == 58);
        CHECK(pspace.read<int>("/f2").value() == 25);
    }

    SUBCASE("PathSpace Read Function Pointer Execution Blocking") {
        int (*f1)() = []() -> int { return pspaceg.read<int>("/f3").value() + 10; };
        int (*f2)() = []() -> int { return pspaceg.read<int>("/f3").value() + 10; };
        int (*f3)() = []() -> int { return 10; };

        CHECK(pspace.insert("/f1", f1).nbrValuesInserted == 1);
        CHECK(pspace.insert("/f2", f2).nbrValuesInserted == 1);
        CHECK(pspace.insert("/f3", f3).nbrValuesInserted == 1);
    }
}

TEST_CASE("PathSpace Extract") {
    SUBCASE("Simple PathSpace Extract") {
        PathSpace pspace;
        CHECK(pspace.insert("/test", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test", 58).nbrValuesInserted == 1);
        auto ret = pspace.extract<int>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.extract<int>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 58);
    }

    SUBCASE("Deeper PathSpace Extract") {
        PathSpace pspace;
        CHECK(pspace.insert("/test1/test2", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 58).nbrValuesInserted == 1);
        auto ret = pspace.extract<int>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.extract<int>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 58);
    }

    SUBCASE("Deeper PathSpace Extract Different Types") {
        PathSpace pspace;
        CHECK(pspace.insert("/test1/test2", 56.45f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 'a').nbrValuesInserted == 1);
        CHECK(pspace.insert("/test1/test2", 34.5f).nbrValuesInserted == 1);
        auto ret = pspace.extract<float>("/test1/test2");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56.45f);
        auto ret2 = pspace.extract<char>("/test1/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 'a');
        auto ret3 = pspace.extract<float>("/test1/test2");
        CHECK(ret3.has_value());
        CHECK(ret3.value() == 34.5f);
    }
}