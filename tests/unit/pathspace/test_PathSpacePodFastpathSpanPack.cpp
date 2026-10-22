#include "PathSpacePodFastpathTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.pod.fastpath.span_pack");

TEST_CASE("Compile-time mutable span take updates POD queue without pop") {
    PathSpace space;
    CHECK(space.insert("/ints", 7).errors.empty());
    CHECK(space.insert("/ints", 8).errors.empty());

    auto ret = space.take<"/ints">([&](std::span<int> ints) {
        REQUIRE(ints.size() == 2);
        ints[0] = 70;
        ints[1] = 80;
    });
    REQUIRE(ret.has_value());

    auto first = space.take<int>("/ints");
    REQUIRE(first.has_value());
    CHECK(first.value() == 70);
    auto second = space.take<int>("/ints");
    REQUIRE(second.has_value());
    CHECK(second.value() == 80);
}

TEST_CASE("Mutable span take can update POD queue without popping") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).errors.empty());
    CHECK(space.insert("/ints", 2).errors.empty());
    CHECK(space.insert("/ints", 3).errors.empty());

    auto ret = space.take("/ints", [&](std::span<int> ints) {
        REQUIRE(ints.size() == 3);
        ints[1] = 42;
        ints[2] = 99;
    });
    REQUIRE(ret.has_value());

    // Queue reflects the mutated values while preserving FIFO (span take does not pop).
    auto t1 = space.take<int>("/ints");
    REQUIRE(t1.has_value());
    CHECK(t1.value() == 1);
    auto t2 = space.take<int>("/ints");
    REQUIRE(t2.has_value());
    CHECK(t2.value() == 42);
    auto t3 = space.take<int>("/ints");
    REQUIRE(t3.has_value());
    CHECK(t3.value() == 99);
}

TEST_CASE("Span pack take pops when callback returns true") {
    PathSpace space;
    auto ins = space.insert<"/pair/a","/pair/b">(1, 2);
    REQUIRE(ins.errors.empty());

    auto take = space.take<"a","b">("/pair",
                                    [&](std::span<int> a, std::span<int> b) {
                                        REQUIRE(a.size() == 1);
                                        REQUIRE(b.size() == 1);
                                        CHECK(a[0] == 1);
                                        CHECK(b[0] == 2);
                                        return true; // request pop
                                    });
    if (!take) {
        CAPTURE(take.error().code);
        if (take.error().message) CAPTURE(*take.error().message);
    }
    REQUIRE(take.has_value());

    auto next = space.take<int>("/pair/a");
    CHECK_FALSE(next.has_value());
    CHECK(next.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("Span pack take keeps data when callback returns void/false") {
    PathSpace space;
    auto ins = space.insert<"/keep/a","/keep/b">(3, 4);
    REQUIRE(ins.errors.empty());

    auto takeVoid = space.take<"a","b">("/keep",
                                        [&](std::span<int> a, std::span<int> b) {
                                            CHECK(a.size() == 1);
                                            CHECK(b.size() == 1);
                                            a[0] = 30;
                                            b[0] = 40;
                                        });
    REQUIRE(takeVoid.has_value());

    auto a = space.take<int>("/keep/a");
    auto b = space.take<int>("/keep/b");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a.value() == 30);
    CHECK(b.value() == 40);

    auto takeFalse = space.insert<"/keep/a","/keep/b">(5, 6);
    REQUIRE(takeFalse.errors.empty());
    auto noop = space.take<"a","b">("/keep",
                                    [&](std::span<int> aSpan, std::span<int> bSpan) {
                                        CHECK(aSpan[0] == 5);
                                        CHECK(bSpan[0] == 6);
                                        return false; // explicit no-pop
                                    });
    REQUIRE(noop.has_value());
    auto a2 = space.take<int>("/keep/a");
    auto b2 = space.take<int>("/keep/b");
    REQUIRE(a2.has_value());
    REQUIRE(b2.has_value());
    CHECK(a2.value() == 5);
    CHECK(b2.value() == 6);
}

TEST_CASE("Span pack take blocks until data available") {
    PathSpace space;
    auto start = std::chrono::steady_clock::now();

    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto ins = space.insert<"/block/a","/block/b">(7, 8);
        REQUIRE(ins.errors.empty());
    });

    auto res = space.take<"a","b">("/block",
                                   [&](std::span<int> a, std::span<int> b) {
                                       CHECK(a.size() == 1);
                                       CHECK(b.size() == 1);
                                       return true;
                                   },
                                   Out{} & Block{std::chrono::milliseconds(200)});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (!res) {
        CAPTURE(res.error().code);
        if (res.error().message) CAPTURE(*res.error().message);
    }
    REQUIRE(res.has_value());
    CHECK(elapsed.count() >= 40); // verify it actually waited

    producer.join();
}

TEST_CASE("Span pack take pops without Pop flag when callback returns true") {
    PathSpace space;
    auto ins = space.insert<"/nopop/a","/nopop/b">(11, 22);
    REQUIRE(ins.errors.empty());

    auto res = space.take<"a","b">("/nopop",
                                   [&](std::span<int> a, std::span<int> b) {
                                       REQUIRE(a.size() == 1);
                                       REQUIRE(b.size() == 1);
                                       return true; // pop even without Pop{}
                                   });
    REQUIRE(res.has_value());

    auto a = space.take<int>("/nopop/a");
    auto b = space.take<int>("/nopop/b");
    CHECK_FALSE(a.has_value());
    CHECK_FALSE(b.has_value());
}

TEST_CASE("Span pack take blocks until paths materialize") {
    PathSpace space;
    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto ins = space.insert<"/appear/a","/appear/b">(9, 10);
        REQUIRE(ins.errors.empty());
    });

    auto res = space.take<"a","b">("/appear",
                                   [&](std::span<int> a, std::span<int> b) {
                                       REQUIRE(a.size() == 1);
                                       REQUIRE(b.size() == 1);
                                       return true;
                                   },
                                   Out{} & Block{std::chrono::milliseconds(200)});
    if (!res) {
        CAPTURE(res.error().code);
        if (res.error().message) CAPTURE(*res.error().message);
    }
    REQUIRE(res.has_value());
    producer.join();
}

TEST_CASE("Span pack pop rejected on minimal misaligned window") {
    PathSpace space;
    REQUIRE(space.insert("/mis/x", 1).errors.empty());
    REQUIRE(space.insert("/mis/y", 2).errors.empty());
    // New pack begins after existing head, so minimal window will start past head.
    REQUIRE(space.insert<"/mis/x","/mis/y">(3, 4).errors.empty());

    auto take = space.take<"x","y">("/mis",
                                    [&](std::span<int>, std::span<int>) {
                                        return true; // request pop
                                    },
                                    Out{} & Minimal{});
    CHECK_FALSE(take.has_value());
    CHECK(take.error().code == Error::Code::InvalidType);
}

TEST_CASE("Span pack read returns aligned spans") {
    PathSpace space;
    std::vector<float> xs{1.f, 2.f, 3.f};
    std::vector<float> ys{4.f, 5.f, 6.f};
    std::vector<float> zs{7.f, 8.f, 9.f};
    for (std::size_t i = 0; i < xs.size(); ++i) {
        CHECK(space.insert("/ints/values/x", xs[i]).errors.empty());
        CHECK(space.insert("/ints/values/y", ys[i]).errors.empty());
        CHECK(space.insert("/ints/values/z", zs[i]).errors.empty());
    }
    auto ret = space.read<"x","y","z">("/ints/values", [&](std::span<const float> x,
                                                           std::span<const float> y,
                                                           std::span<const float> z) {
        CHECK(x.size() == xs.size());
        CHECK(y.size() == ys.size());
        CHECK(z.size() == zs.size());
        for (std::size_t i = 0; i < xs.size(); ++i) {
            CHECK(x[i] == doctest::Approx(xs[i]));
            CHECK(y[i] == doctest::Approx(ys[i]));
            CHECK(z[i] == doctest::Approx(zs[i]));
        }
    });
    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
        std::cerr << "span_pack_read_error=" << static_cast<int>(ret.error().code) << std::endl;
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack read rejects length mismatch") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/x", 2.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 10.f).errors.empty());
    CHECK(space.insert("/ints/values/z", 20.f).errors.empty());
    CHECK(space.insert("/ints/values/z", 30.f).errors.empty());

    auto ret = space.read<"x","y","z">("/ints/values", [&](std::span<const float>, std::span<const float>, std::span<const float>) {});
    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);
}

TEST_CASE("Span pack mutable take edits in place") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/x", 2.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 3.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 4.f).errors.empty());

    auto mut = space.take<"x","y">("/ints/values", [&](std::span<float> x, std::span<float> y) {
        REQUIRE(x.size() == y.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] += 10.f;
            y[i] += 20.f;
        }
    });
    if (!mut) {
        CAPTURE(mut.error().code);
        if (mut.error().message) CAPTURE(*mut.error().message);
    }
    REQUIRE(mut.has_value());

    auto check = space.read<"x","y">("/ints/values", [&](std::span<const float> x, std::span<const float> y) {
        REQUIRE(x.size() == 2);
        CHECK(x[0] == doctest::Approx(11.f));
        CHECK(x[1] == doctest::Approx(12.f));
        CHECK(y[0] == doctest::Approx(23.f));
        CHECK(y[1] == doctest::Approx(24.f));
    });
    REQUIRE(check.has_value());
}

TEST_CASE("Span pack read supports empty queues") {
    PathSpace space;
    // Create empty POD queues by inserting then consuming
    CHECK(space.insert("/ints/values/x", 0.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 0.f).errors.empty());
    CHECK(space.take<float>("/ints/values/x").has_value());
    CHECK(space.take<float>("/ints/values/y").has_value());

    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const float> x, std::span<const float> y) {
        CHECK(x.empty());
        CHECK(y.empty());
    });
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack rejects mixed POD types") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 5).errors.empty());
    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);
}

TEST_CASE("Span pack mutable take length mismatch keeps data unchanged") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 2.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 3.f).errors.empty());

    auto mut = space.take<"x","y">("/ints/values", [&](std::span<float> x, std::span<float> y) {
        (void)x;
        (void)y;
    });
    CHECK_FALSE(mut.has_value());
    CHECK(mut.error().code == Error::Code::InvalidType);

    auto xFront = space.read<float>("/ints/values/x");
    REQUIRE(xFront.has_value());
    CHECK(xFront.value() == doctest::Approx(1.f));
    auto yVals = space.read<"y">("/ints/values", [&](std::span<const float> y) {
        REQUIRE(y.size() == 2);
        CHECK(y[0] == doctest::Approx(2.f));
        CHECK(y[1] == doctest::Approx(3.f));
    });
    REQUIRE(yVals.has_value());
}

TEST_CASE("Span pack read rejects blocking; take allows optional pop") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 2.f).errors.empty());
    auto block = space.read<"x","y">("/ints/values",
                                     [&](std::span<const float>, std::span<const float>) {},
                                     Out{} & Block{std::chrono::milliseconds(5)});
    CHECK_FALSE(block.has_value());
    CHECK(block.error().code == Error::Code::NotSupported);

    auto pop = space.take<"x","y">("/ints/values",
                                   [&](std::span<float> xs, std::span<float> ys) {
                                       REQUIRE(xs.size() == 1);
                                       REQUIRE(ys.size() == 1);
                                       return true;
                                   },
                                   Out{} & Pop{});
    REQUIRE(pop.has_value());
    auto x = space.take<float>("/ints/values/x");
    auto y = space.take<float>("/ints/values/y");
    CHECK_FALSE(x.has_value());
    CHECK_FALSE(y.has_value());
}

TEST_CASE("Span pack rejects glob and indexed paths") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 2.f).errors.empty());

    auto glob = space.read<"x","y">("/ints/*", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(glob.has_value());
    CHECK(glob.error().code == Error::Code::InvalidPath);

    auto indexed = space.read<"x","y">("/ints/values[0]", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(indexed.has_value());
    CHECK(indexed.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Span pack fails fast on non-POD payloads") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", std::string("hello")).errors.empty());
    CHECK(space.insert("/ints/values/y", std::string("world")).errors.empty());
    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(ret.has_value());
    CHECK((ret.error().code == Error::Code::NotSupported || ret.error().code == Error::Code::InvalidType));
}

TEST_CASE("Span pack handles larger arity") {
    PathSpace space;
    for (int i = 0; i < 3; ++i) {
        CHECK(space.insert("/ints/values/a", i).errors.empty());
        CHECK(space.insert("/ints/values/b", i + 10).errors.empty());
        CHECK(space.insert("/ints/values/c", i + 20).errors.empty());
        CHECK(space.insert("/ints/values/d", i + 30).errors.empty());
    }
    auto ret = space.read<"a","b","c","d">("/ints/values",
                                           [&](std::span<const int> a,
                                               std::span<const int> b,
                                               std::span<const int> c,
                                               std::span<const int> d) {
                                               REQUIRE(a.size() == 3);
                                               for (int i = 0; i < 3; ++i) {
                                                   CHECK(a[i] == i);
                                                   CHECK(b[i] == i + 10);
                                                   CHECK(c[i] == i + 20);
                                               CHECK(d[i] == i + 30);
                                           }
                                           });
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack keeps POD buffers alive during callback") {
    PathSpace space;
    constexpr std::size_t FillCount = 1025;
    for (std::size_t i = 0; i < FillCount; ++i) {
        CHECK(space.insert("/ints/values/x", static_cast<int>(i)).errors.empty());
        CHECK(space.insert("/ints/values/y", static_cast<int>(1000 + i)).errors.empty());
    }

    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == FillCount);
        REQUIRE(ys.size() == FillCount);

        // Force both payloads to resize and free their original buffers.
        CHECK(space.insert("/ints/values/x", 999999).errors.empty());
        CHECK(space.insert("/ints/values/y", 888888).errors.empty());

        // Allocate similarly sized buffers to encourage reuse of the freed memory.
        std::vector<int> clobberA(FillCount, 42);
        std::vector<int> clobberB(FillCount, 84);
        (void)clobberA;
        (void)clobberB;

        for (std::size_t i = 0; i < FillCount; ++i) {
            CHECK(xs[i] == static_cast<int>(i));
            CHECK(ys[i] == static_cast<int>(1000 + i));
        }
    });

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack traits reject arity mismatch at compile time") {
    using BadFn = decltype([](std::span<const float>, std::span<const float>, int) {});
    static_assert(!SP::Detail::SpanPackTraits<BadFn>::isSpanPack);
}

TEST_CASE("Span pack minimal take falls back to head when no pack markers exist") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 10).errors.empty());
    REQUIRE(space.insert("/ints/y", 20).errors.empty());

    bool invoked = false;
    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int> xs, std::span<int> ys) {
                                       invoked = true;
                                       REQUIRE(xs.size() == 1);
                                       REQUIRE(ys.size() == 1);
                                       CHECK(xs[0] == 10);
                                       CHECK(ys[0] == 20);
                                   },
                                   Out{} & Minimal{});

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
    CHECK(invoked);
}

TEST_CASE("Minimal span take rejects drift after mixed history") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 1).errors.empty());
    REQUIRE(space.insert<"/ints/x","/ints/y">(2, 2).errors.empty());
    REQUIRE(space.insert("/ints/x", 999).errors.empty()); // drift lane x only

    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int>, std::span<int>) {},
                                   Out{} & Minimal{});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);

    auto xSpan = space.read<"x">("/ints", [&](std::span<const int> xs) {
        REQUIRE(xs.size() == 3);
        CHECK(xs[0] == 1);
        CHECK(xs[1] == 2);
        CHECK(xs[2] == 999);
    });
    REQUIRE(xSpan.has_value());
    auto ySpan = space.read<"y">("/ints", [&](std::span<const int> ys) {
        REQUIRE(ys.size() == 2);
        CHECK(ys[0] == 1);
        CHECK(ys[1] == 2);
    });
    REQUIRE(ySpan.has_value());
}

TEST_CASE("Span pack read rejects block and pop options") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 2).errors.empty());

    auto block = space.read<"x","y">("/ints",
                                     [&](std::span<const int>, std::span<const int>) {},
                                     Out{} & Block{std::chrono::milliseconds{5}});
    CHECK_FALSE(block.has_value());
    CHECK(block.error().code == Error::Code::NotSupported);

    auto pop = space.read<"x","y">("/ints",
                                   [&](std::span<const int>, std::span<const int>) {},
                                   Out{} & Pop{});
    CHECK_FALSE(pop.has_value());
    CHECK(pop.error().code == Error::Code::NotSupported);
}

TEST_CASE("Span pack take supports block and optional pop flag") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 2).errors.empty());

    auto block = space.take<"x","y">("/ints",
                                     [&](std::span<int>, std::span<int>) {},
                                     Out{} & Block{std::chrono::milliseconds{5}});
    REQUIRE(block.has_value());

    auto pop = space.take<"x","y">("/ints",
                                   [&](std::span<int> a, std::span<int> b) {
                                       REQUIRE(a.size() == 1);
                                       REQUIRE(b.size() == 1);
                                       return true;
                                   },
                                   Out{} & Pop{});
    REQUIRE(pop.has_value());
    auto a = space.take<int>("/ints/x");
    auto b = space.take<int>("/ints/y");
    CHECK_FALSE(a.has_value());
    CHECK_FALSE(b.has_value());
}

TEST_CASE("Span pack read rejects glob and indexed base paths") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 2).errors.empty());

    auto glob = space.read<"x","y">("/ints/*", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(glob.has_value());
    CHECK(glob.error().code == Error::Code::InvalidPath);

    auto indexed = space.read<"x","y">("/ints[0]", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(indexed.has_value());
    CHECK(indexed.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Span pack read surfaces lane length mismatch") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/x", 2).errors.empty());
    REQUIRE(space.insert("/ints/y", 1).errors.empty());

    auto ret = space.read<"x","y">("/ints", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);

    auto xs1 = space.take<int>("/ints/x");
    auto xs2 = space.take<int>("/ints/x");
    auto ys1 = space.take<int>("/ints/y");
    REQUIRE(xs1.has_value());
    REQUIRE(xs2.has_value());
    REQUIRE(ys1.has_value());
    CHECK(xs1.value() == 1);
    CHECK(xs2.value() == 2);
    CHECK(ys1.value() == 1);
}

TEST_CASE("Minimal span take fails fast when lane lacks POD payload") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 7).errors.empty());
    REQUIRE(space.insert("/ints/y", std::string("nonpod")).errors.empty());

    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int>, std::span<int>) {},
                                   Out{} & Minimal{});
    CHECK_FALSE(ret.has_value());
    auto code = ret.error().code;
    CHECK(code == Error::Code::InvalidType);
    if (code != Error::Code::InvalidType) {
        CHECK(code == Error::Code::NotSupported);
    }

    auto x = space.read<int>("/ints/x");
    REQUIRE(x.has_value());
    CHECK(x.value() == 7);
}

TEST_SUITE_END();
