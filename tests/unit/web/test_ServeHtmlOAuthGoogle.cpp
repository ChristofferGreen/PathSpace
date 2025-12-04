#include "third_party/doctest.h"

#include "pathspace/web/serve_html/auth/OAuthGoogle.hpp"

TEST_CASE("parse_url handles https endpoints") {
    auto parsed = SP::ServeHtml::OAuthGoogle::parse_url("https://example.com:8443/oauth");
    REQUIRE(parsed.has_value());
    CHECK(parsed->scheme == "https");
    CHECK(parsed->host == "example.com");
    CHECK(parsed->port == 8443);
    CHECK(parsed->path == "/oauth");
    CHECK(parsed->tls);
}

TEST_CASE("AuthStateStore issues and consumes states") {
    SP::ServeHtml::OAuthGoogle::AuthStateStore store;
    auto issued = store.issue("/apps/demo");
    REQUIRE_FALSE(issued.state.empty());
    REQUIRE_FALSE(issued.entry.code_verifier.empty());

    auto taken = store.take(issued.state);
    REQUIRE(taken.has_value());
    CHECK(taken->redirect == "/apps/demo");
    CHECK(taken->code_verifier == issued.entry.code_verifier);
    CHECK_FALSE(store.take(issued.state).has_value());
}

TEST_CASE("compute_code_challenge matches RFC example") {
    std::string_view verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    auto challenge = SP::ServeHtml::OAuthGoogle::compute_code_challenge(verifier);
    CHECK(challenge == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

