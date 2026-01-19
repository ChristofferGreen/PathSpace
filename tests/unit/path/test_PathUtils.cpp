#include "path/utils.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE_BEGIN("path.utils");

TEST_CASE("parse_indexed_component extracts numeric suffix and preserves base") {
    auto parsed = parse_indexed_component("child[12]");
    CHECK(parsed.base == "child");
    REQUIRE(parsed.index.has_value());
    CHECK(*parsed.index == 12u);
    CHECK_FALSE(parsed.malformed);
}

TEST_CASE("parse_indexed_component handles malformed or escaped brackets") {
    auto emptyIndex = parse_indexed_component("child[]");
    CHECK_FALSE(emptyIndex.index.has_value());
    CHECK(emptyIndex.malformed);

    auto alphaIndex = parse_indexed_component("child[a]");
    CHECK_FALSE(alphaIndex.index.has_value());
    CHECK(alphaIndex.malformed);

    auto escaped = parse_indexed_component("child\\[2]");
    CHECK_FALSE(escaped.index.has_value());
    CHECK_FALSE(escaped.malformed);

    auto bracketNotTerminal = parse_indexed_component("child[1]extra");
    CHECK_FALSE(bracketNotTerminal.index.has_value());
    CHECK_FALSE(bracketNotTerminal.malformed);

    auto noBase = parse_indexed_component("[3]");
    CHECK_FALSE(noBase.index.has_value());
    CHECK_FALSE(noBase.malformed);

    auto plain = parse_indexed_component("plain");
    CHECK(plain.base == "plain");
    CHECK_FALSE(plain.index.has_value());
    CHECK_FALSE(plain.malformed);

    auto unterminated = parse_indexed_component("node[1");
    CHECK_FALSE(unterminated.index.has_value());
    CHECK_FALSE(unterminated.malformed);
}

TEST_CASE("append_index_suffix elides zero and formats numeric suffixes") {
    CHECK(append_index_suffix("base", 0) == "base");
    CHECK(append_index_suffix("base", 5) == "base[5]");
}

TEST_CASE("append_index_suffix round-trips through parse_indexed_component") {
    auto appended = append_index_suffix("round", 42);
    auto parsed   = parse_indexed_component(appended);

    CHECK(parsed.base == "round");
    REQUIRE(parsed.index.has_value());
    CHECK(*parsed.index == 42u);
    CHECK_FALSE(parsed.malformed);
}

TEST_CASE("is_glob treats numeric indices as concrete") {
    CHECK_FALSE(is_glob("/root/child[3]"));
    CHECK(is_glob("/root/child[*]"));

    CHECK_FALSE(is_glob("/root/node[12]/leaf"));
    CHECK(is_glob("/root/node[12]x"));   // trailing character invalidates index form
    CHECK(is_glob("/root/node[]/leaf")); // empty index is a glob
}

TEST_CASE("is_glob handles escapes and malformed brackets") {
    CHECK_FALSE(is_glob("/root/escaped\\[7\\]"));
    CHECK(is_glob("/root/unmatched]"));
    CHECK(is_glob("/root/unclosed["));
    CHECK(is_glob("/root/alpha[1a]/beta"));
    CHECK_FALSE(is_glob("/root/indexed[4]/child"));
}

TEST_CASE("match_names rejects malformed character classes") {
    CHECK_FALSE(match_names("[abc", "a"));
    CHECK_FALSE(match_names("test[!", "testa"));
}

TEST_SUITE_END();
