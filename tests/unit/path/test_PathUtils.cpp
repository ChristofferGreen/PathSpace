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

    auto zeroIndex = parse_indexed_component("child[0]");
    CHECK(zeroIndex.base == "child");
    REQUIRE(zeroIndex.index.has_value());
    CHECK(*zeroIndex.index == 0u);
    CHECK_FALSE(zeroIndex.malformed);

    auto leadingZeros = parse_indexed_component("child[001]");
    CHECK(leadingZeros.base == "child");
    REQUIRE(leadingZeros.index.has_value());
    CHECK(*leadingZeros.index == 1u);
    CHECK_FALSE(leadingZeros.malformed);
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

    // Escaped closing bracket inside index should mark the index malformed.
    auto escapedClose = parse_indexed_component("child[1\\]]");
    CHECK_FALSE(escapedClose.index.has_value());
    CHECK(escapedClose.malformed);

    // Escaped digit inside index yields malformed index detection.
    auto escapedDigit = parse_indexed_component("child[1\\2]");
    CHECK_FALSE(escapedDigit.index.has_value());
    CHECK(escapedDigit.malformed);
}

TEST_CASE("append_index_suffix elides zero and formats numeric suffixes") {
    CHECK(append_index_suffix("base", 0) == "base");
    CHECK(append_index_suffix("base", 5) == "base[5]");
    CHECK(append_index_suffix("", 0).empty());
    CHECK(append_index_suffix("", 3) == "[3]");
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
    CHECK_FALSE(is_glob("/root/child[3]/"));
    CHECK(is_glob("/root/child[*]"));

    CHECK_FALSE(is_glob("/root/node[12]/leaf"));
    CHECK(is_glob("/root/node[12]x"));   // trailing character invalidates index form
    CHECK(is_glob("/root/node[]/leaf")); // empty index is a glob
}

TEST_CASE("is_glob handles escapes and malformed brackets") {
    CHECK_FALSE(is_glob("/root/escaped\\[7\\]"));
    CHECK_FALSE(is_glob("/root/escaped\\?/ok"));
    CHECK_FALSE(is_glob("/root/escaped\\*/ok"));
    CHECK_FALSE(is_glob("/root/escaped\\]/ok"));
    CHECK(is_glob("/root/unmatched]"));
    CHECK(is_glob("/root/unclosed["));
    CHECK(is_glob("/root/alpha[1a]/beta"));
    CHECK_FALSE(is_glob("/root/indexed[4]/child"));
}

TEST_CASE("is_concrete mirrors glob detection") {
    CHECK(is_concrete("/root/child"));
    CHECK_FALSE(is_concrete("/root/*"));
    CHECK(is_concrete("/root/escaped\\*"));
}

TEST_CASE("match_names rejects malformed character classes") {
    CHECK_FALSE(match_names("[abc", "a"));
    CHECK_FALSE(match_names("test[!", "testa"));
    CHECK_FALSE(match_names("[]", "a"));
}

TEST_CASE("match_names covers wildcards, ranges, and escapes") {
    CHECK(match_names("fo*", "foobar"));
    CHECK(match_names("*a", "ba"));
    CHECK(match_names("ba?r", "baar"));
    CHECK_FALSE(match_names("ba?r", "bar")); // missing char for '?'
    CHECK(match_names("h[ae]llo", "hello"));
    CHECK(match_names("h[!a]llo", "hello"));
    CHECK(match_names("h[!a]llo", "hbllo"));
    CHECK(match_names("a[0-9]b", "a5b"));
    CHECK_FALSE(match_names("a[0-9]b", "acb"));
    CHECK(match_names("star\\*", "star*"));
    CHECK_FALSE(match_names("star\\*", "starX"));
    CHECK(match_names("path\\\\name", "path\\name"));
    CHECK(match_names("close\\]", "close]"));
}

TEST_CASE("match_names handles star backtracking misses and literal hyphens") {
    CHECK_FALSE(match_names("a*b", "ac")); // '*' cannot find trailing 'b'
    CHECK(match_names("[-a]", "-"));
    CHECK(match_names("[-a]", "a"));
    CHECK_FALSE(match_names("[-a]", "b"));

    // Leading range before any previous character should still match properly.
    CHECK(match_names("[a-c]", "b"));
    CHECK_FALSE(match_names("[a-c]", "d"));
}

TEST_CASE("match_names handles empty and dangling escape patterns") {
    CHECK(match_names("", ""));
    CHECK_FALSE(match_names("", "x"));
    CHECK_FALSE(match_names("foo\\", "foo"));

    CHECK(match_names("a*", "a"));
    CHECK_FALSE(match_names("a*", ""));

    CHECK(match_names("*", "")); // trailing-star cleanup
}

TEST_CASE("match_paths handles mismatched lengths and escaped components") {
    CHECK_FALSE(match_paths("/a/b", "/a/b/c"));
    CHECK_FALSE(match_paths("/a/b/c", "/a/b"));
    CHECK(match_paths("/foo/ba\\*/c", "/foo/ba*/c"));
    CHECK_FALSE(match_paths("/foo/ba\\*/c", "/foo/baX/c"));

    CHECK(match_paths("/a/*/c", "/a/b/c"));
    CHECK_FALSE(match_paths("/a/*/c", "/a/b/d"));
}

TEST_CASE("match_paths handles root and empty paths") {
    CHECK(match_paths("/", "/"));
    CHECK_FALSE(match_paths("/", "/a"));
    CHECK_FALSE(match_paths("/a", "/"));
    CHECK_FALSE(match_paths("/*", "/"));
    CHECK(match_paths("", ""));
    CHECK(match_paths("", "/"));
    CHECK(match_paths("/", ""));
    CHECK(match_paths("/alpha/", "/alpha"));
    CHECK(match_paths("/alpha/", "/alpha/"));
    CHECK_FALSE(match_paths("/alpha/", "/alpha/beta"));
    CHECK(match_paths("alpha/beta", "alpha/beta"));
    CHECK(match_paths("alpha/beta", "/alpha/beta"));
}

TEST_SUITE_END();
