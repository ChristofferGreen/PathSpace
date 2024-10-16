#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Capabilities.hpp>

using namespace SP;
int aert = 5;

int fpause() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 46;
}

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
}

// lambdas should come from a central database in order to support serialization to remote computer
/*TEST_CASE("PathSpace Insert Lambda") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Lambda Insert") {
        CHECK(pspace.insert("/test1", [](){}).nbrValuesInserted == 1);
    }
}*/

TEST_CASE("PathSpace Function Insertion and Execution") {
    PathSpace pspace;

    SUBCASE("Simple Function Pointer Insertion and Execution") {
        int (*simpleFunc)() = []() -> int { return 42; };
        CHECK(pspace.insert("/simple", simpleFunc).errors.size() == 0);
        auto result = pspace.readBlock<int>("/simple");
        CHECK(result.has_value());
        CHECK(result.value() == 42);
    }

    SUBCASE("std::function Insertion and Execution") {
        std::function<int()> stdFunc = []() -> int { return 100; };
        CHECK(pspace.insert("/std", stdFunc).errors.size() == 0);
        auto result = pspace.readBlock<int>("/std");
        CHECK(result.has_value());
        CHECK(result.value() == 100);
    }

    SUBCASE("Nested Function Calls with Different Types") {
        auto const f1 = [&pspace]() -> double {
            auto const val = pspace.readBlock<int>("/f2").value();
            return val * 1.5;
        };
        auto const f2 = [&pspace]() -> int {
            auto const val = pspace.readBlock<std::string>("/f3").value();
            return std::stoi(val);
        };
        std::function<std::string()> const f3 = []() -> std::string { return "50"; };

        CHECK(pspace.insert("/f1", f1).errors.size() == 0);
        CHECK(pspace.insert("/f2", f2).errors.size() == 0);
        CHECK(pspace.insert("/f3", f3).errors.size() == 0);

        auto result = pspace.readBlock<double>("/f1");
        CHECK(result.has_value());
        CHECK(result.value() == 75.0);
    }

    SUBCASE("Function Overwriting") {
        /*int (*func1)() = []() -> int { return 1; };
        int (*func2)() = []() -> int { return 2; };

        CHECK(pspace.insert(SP::GlobPathStringView{"/overwrite"}, func1).errors.size() == 0);
        CHECK(pspace.insert(SP::GlobPathStringView{"/overwrite"}, func2).errors.size() == 0);

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/overwrite"});
        CHECK(result.has_value());
        CHECK(result.value() == 2);*/
    }

    SUBCASE("Circular Dependency Detection") {
        // ToDo : Implement circular dependency detection
        /*auto f1 = [&pspace]() -> int { return pspace.readBlock<int>(SP::ConcretePathStringView{"/f2"}).value() + 1; };
        auto f2 = [&pspace]() -> int { return pspace.readBlock<int>(SP::ConcretePathStringView{"/f1"}).value() + 1; };

        CHECK(pspace.insert(SP::GlobPathStringView{"/f1"}, f1).errors.size() == 0);
        CHECK(pspace.insert(SP::GlobPathStringView{"/f2"}, f2).errors.size() == 0);

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/f1"});
        // Expecting an error or timeout due to circular dependency
        CHECK(!result.has_value());*/
    }

    SUBCASE("Exception Handling in Functions") {
        /*auto throwingFunc = []() -> int { throw std::runtime_error("Test exception"); };

        CHECK(pspace.insert(SP::GlobPathStringView{"/throwing"}, throwingFunc).errors.size() == 0);

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/throwing"});
        CHECK(!result.has_value());*/
        // ToDo: Check for appropriate error handling
        // The exact error checking depends on how PathSpace handles exceptions
    }

    SUBCASE("Large Number of Nested Calls") {
        const int DEPTH = 1000;
        for (int i = 0; i < DEPTH; ++i) {
            auto func = [i, &pspace]() -> int {
                if (i == 0)
                    return 1;
                return pspace.readBlock<int>(SP::ConcretePathStringView{"/func" + std::to_string(i - 1)}).value() + 1;
            };
            CHECK(pspace.insert(SP::GlobPathStringView{"/func" + std::to_string(i)}, func).errors.size() == 0);
        }

        auto result = pspace.readBlock<int>(SP::ConcretePathStringView{"/func" + std::to_string(DEPTH - 1)});
        CHECK(result.has_value());
        CHECK(result.value() == DEPTH);
    }

    SUBCASE("Concurrent Function Execution") {
        std::atomic<int> counter(0);
        auto incrementFunc = [&counter]() -> int {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return ++counter;
        };

        for (int i = 0; i < 100; ++i) {
            CHECK(pspace.insert(SP::GlobPathStringView{"/concurrent" + std::to_string(i)}, incrementFunc).errors.size() == 0);
        }

        std::vector<std::thread> threads;
        for (int i = 0; i < 100; ++i) {
            threads.emplace_back([&pspace, i]() { pspace.readBlock<int>(SP::ConcretePathStringView{"/concurrent" + std::to_string(i)}); });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(counter == 100);
    }
}

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
        CHECK(pspace.insert("/f", f).nbrTasksCreated == 1);
        CHECK(pspace.insert("/f2", f2).nbrTasksCreated == 1);
        CHECK(pspace.readBlock<int>("/f").value() == 58);
        CHECK(pspace.readBlock<int>("/f").value() == 58);
        CHECK(pspace.readBlock<int>("/f2").value() == 25);
    }

    SUBCASE("Simple PathSpace Execution Non-Immidiate") {
        std::function<int()> f = []() -> int { return 58; };
        CHECK(pspace.insert("/f", f, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::OnReadOrExtract}})
                      .nbrTasksCreated
              == 1);
        CHECK(pspace.readBlock<int>("/f").value() == 58);
    }

    SUBCASE("PathSpace Read Function Pointer Execution Blocking Simple") {
        auto const f1 = [&pspace]() -> int { return pspace.readBlock<int>("/f2").value() + 11; };
        int (*f2)() = []() -> int { return 10; };

        CHECK(pspace.insert("/f1", f1).errors.size() == 0);
        CHECK(pspace.insert("/f2", f2).errors.size() == 0);

        auto const val = pspace.readBlock<int>("/f1");
        CHECK(val.has_value() == true);
        CHECK(val.value() == 21);
    }

    SUBCASE("PathSpace Read Function Pointer Execution Blocking") {
        auto const f1 = [&pspace]() -> int {
            auto val = pspace.readBlock<int>("/f2").value();
            log(std::format("f1 returning {} + 1 = {} from f2.", val, val + 1));
            return val + 1;
        };
        auto const f2 = [&pspace]() -> int {
            auto val = pspace.readBlock<int>("/f3").value();
            log(std::format("f2 returning {} + 10 = {} from f3.", val, val + 10));
            return val + 10;
        };
        int (*f3)() = []() -> int {
            log("f3 returning 100.");
            return 100;
        };

        CHECK(pspace.insert("/f1", f1).errors.size() == 0);
        CHECK(pspace.insert("/f2", f2).errors.size() == 0);
        CHECK(pspace.insert("/f3", f3).errors.size() == 0);

        auto const val = pspace.readBlock<int>("/f1");
        CHECK(val.has_value() == true);
        CHECK(val.value() == 111);
    }

    SUBCASE("PathSpace Read Block") {
        pspace.insert("/i", 46);
        auto const val = pspace.readBlock<int>("/i");
        CHECK(val.has_value());
        CHECK(val.value() == 46);
    }

    SUBCASE("PathSpace Read Block Delayed") {
        pspace.insert("/i", +[] { return 46; });
        auto const val = pspace.readBlock<int>("/i");
        CHECK(val.has_value());
        CHECK(val.value() == 46);
    }
}

TEST_CASE("PathSpace Read Std Datastructure") {
    PathSpace pspace;
    SUBCASE("PathSpace Read std::string") {
        pspace.insert("/i", std::string("hello"));
        auto const val = pspace.read<std::string>("/i");
        CHECK(val.has_value());
        CHECK(val.value() == "hello");
    }
}

TEST_CASE("PathSpace Extract") {
    PathSpace pspace;
    SUBCASE("Simple PathSpace Extract") {
        CHECK(pspace.insert("/test", 56).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test", 58).nbrValuesInserted == 1);
        auto ret = pspace.extract<int>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56);
        auto ret2 = pspace.extract<int>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == 58);
    }

    SUBCASE("Extract Different Types") {
        CHECK(pspace.insert("/test1", 56.45f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test2", std::string("hello")).nbrValuesInserted == 1);
        auto ret = pspace.extract<float>("/test1");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56.45f);
        auto ret2 = pspace.extract<std::string>("/test2");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == "hello");
    }

    SUBCASE("Extract Different Types Same Place") {
        CHECK(pspace.insert("/test", 56.45f).nbrValuesInserted == 1);
        CHECK(pspace.insert("/test", std::string("hello")).nbrValuesInserted == 1);
        auto ret = pspace.extract<float>("/test");
        CHECK(ret.has_value());
        CHECK(ret.value() == 56.45f);
        auto ret2 = pspace.extract<std::string>("/test");
        CHECK(ret2.has_value());
        CHECK(ret2.value() == "hello");
    }

    SUBCASE("Deeper PathSpace Extract") {
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

    SUBCASE("Simple PathSpace Execution Non-Immediate") {
        std::function<int()> f = []() -> int { return 58; };
        CHECK(pspace.insert("/f", f, InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::OnReadOrExtract}})
                      .nbrTasksCreated
              == 1);
        CHECK(pspace.extractBlock<int>("/f").value() == 58);
        CHECK(!pspace.extractBlock<int>("/f").has_value());
    }
}

TEST_CASE("Extended PathSpace Extract Tests") {
    PathSpace pspace;

    SUBCASE("Extract std::string") {
        pspace.insert("/str", std::string("hello world"));
        auto ret = pspace.extract<std::string>("/str");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == "hello world");
    }

    SUBCASE("Extract std::vector") {
        std::vector<int> vec = {1, 2, 3, 4, 5};
        pspace.insert("/vec", vec);
        auto ret = pspace.extract<std::vector<int>>("/vec");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == vec);
    }

    SUBCASE("Extract std::map") {
        std::map<std::string, int> map = {{"one", 1}, {"two", 2}, {"three", 3}};
        pspace.insert("/map", map);
        auto ret = pspace.extract<std::map<std::string, int>>("/map");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == map);
    }

    SUBCASE("Extract custom struct") {
        struct CustomStruct {
            int x;
            std::string y;
            bool operator==(const CustomStruct& other) const {
                return x == other.x && y == other.y;
            }
        };
        CustomStruct cs{42, "test"};
        pspace.insert("/custom", cs);
        auto ret = pspace.extract<CustomStruct>("/custom");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == cs);
    }

    /*SUBCASE("Extract from non-existent path") {
        auto ret = pspace.extract<int>("/non_existent");
        CHECK_FALSE(ret.has_value());
    }*/

    /*SUBCASE("Extract with type mismatch") {
        pspace.insert("/int", 42);
        auto ret = pspace.extract<std::string>("/int");
        CHECK_FALSE(ret.has_value());
    }*/

    SUBCASE("Extract multiple times") {
        pspace.insert("/multi", 1);
        pspace.insert("/multi", 2);
        pspace.insert("/multi", 3);

        auto ret1 = pspace.extract<int>("/multi");
        auto ret2 = pspace.extract<int>("/multi");
        auto ret3 = pspace.extract<int>("/multi");
        auto ret4 = pspace.extract<int>("/multi");

        REQUIRE(ret1.has_value());
        REQUIRE(ret2.has_value());
        REQUIRE(ret3.has_value());
        CHECK_FALSE(ret4.has_value());

        CHECK(ret1.value() == 1);
        CHECK(ret2.value() == 2);
        CHECK(ret3.value() == 3);
    }

    SUBCASE("Extract with deep path") {
        pspace.insert("/deep/nested/path", 42);
        auto ret = pspace.extract<int>("/deep/nested/path");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 42);
    }

    /*SUBCASE("Extract lambda function") {
        auto lambda = []() -> int { return 42; };
        pspace.insert("/lambda", lambda);
        auto ret = pspace.extract<std::function<int()>>("/lambda");
        REQUIRE(ret.has_value());
        CHECK(ret.value()() == 42);
    }*/

    SUBCASE("Extract with capabilities") {
        Capabilities caps;
        caps.addCapability("/test", Capabilities::Type::READ);

        pspace.insert("/test", 42);
        auto ret = pspace.extract<int>("/test", OutOptions{}, caps);
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 42);
    }

    /*SUBCASE("Extract with incorrect capabilities") {
        Capabilities caps;
        caps.addCapability("/other", Capabilities::Type::READ);

        pspace.insert("/test", 42);
        auto ret = pspace.extract<int>("/test", OutOptions{}, caps);
        CHECK_FALSE(ret.has_value());
    }*/

    SUBCASE("Extract with blocking") {
        std::thread t([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pspace.insert("/delayed", 42);
        });

        auto ret = pspace.extractBlock<int>("/delayed");
        REQUIRE(ret.has_value());
        CHECK(ret.value() == 42);

        t.join();
    }

    /*SUBCASE("Extract with timeout") {
        auto ret = pspace.extractBlock<int>(
                "/timeout",
                OutOptions{.block = BlockOptions{.behavior = BlockOptions::Behavior::Wait, .timeout = std::chrono::milliseconds(100)}});
        CHECK_FALSE(ret.has_value());
    }*/

    /*SUBCASE("Extract after clear") {
        pspace.insert("/clear_test", 42);
        pspace.clear();
        auto ret = pspace.extract<int>("/clear_test");
        CHECK_FALSE(ret.has_value());
    }*/

    /*SUBCASE("Extract with periodic execution") {
        int counter = 0;
        auto periodic_func = [&counter]() -> int { return ++counter; };
        pspace.insert("/periodic",
                      periodic_func,
                      InOptions{.execution = ExecutionOptions{.category = ExecutionOptions::Category::PeriodicOnRead,
                                                              .updateInterval = std::chrono::milliseconds(50)}});

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto ret1 = pspace.extract<int>("/periodic");
        auto ret2 = pspace.extract<int>("/periodic");

        REQUIRE(ret1.has_value());
        REQUIRE(ret2.has_value());
        CHECK(ret1.value() > 0);
        CHECK(ret2.value() > ret1.value());
    }*/
}