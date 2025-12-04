#include "third_party/doctest.h"

#include "pathspace/web/ServeHtmlServer.hpp"
#include "pathspace/web/ServeHtmlOptions.hpp"
#include "pathspace/web/serve_html/auth/SessionStore.hpp"

#include <nlohmann/json.hpp>

namespace {

auto make_config() -> SP::ServeHtml::SessionConfig {
    return SP::ServeHtml::SessionConfig{
        .cookie_name = "ps_session",
        .idle_timeout = std::chrono::seconds{60},
        .absolute_timeout = std::chrono::seconds{300},
    };
}

} // namespace

TEST_CASE("InMemorySessionStore creates validates and revokes sessions") {
    auto config = make_config();
    SP::ServeHtml::InMemorySessionStore store{config};

    auto id = store.create_session("alice");
    REQUIRE(id.has_value());

    auto validated = store.validate(*id);
    REQUIRE(validated.has_value());
    CHECK(*validated == "alice");

    store.revoke(*id);
    CHECK_FALSE(store.validate(*id).has_value());
}

TEST_CASE("PathSpaceSessionStore persists JSON metadata") {
    SP::ServeHtml::ServeHtmlSpace space;
    SP::ServeHtml::ServeHtmlOptions options{};
    options.session_store_backend = "pathspace";
    options.session_store_path = "/system/web/sessions";

    auto config = make_config();
    auto store = SP::ServeHtml::make_session_store(space, options, config);
    REQUIRE(store != nullptr);

    auto session_id = store->create_session("bob");
    REQUIRE(session_id.has_value());

    auto persisted_path = options.session_store_path + "/" + *session_id;
    auto stored         = space.read<std::string, std::string>(persisted_path);
    REQUIRE(stored);

    auto payload = nlohmann::json::parse(*stored, nullptr, false);
    REQUIRE(payload.is_object());
    CHECK(payload["username"].get<std::string>() == "bob");
    CHECK(payload["version"].get<int>() == 1);

    auto validated = store->validate(*session_id);
    CHECK(validated.has_value());
    CHECK(*validated == "bob");

    store->revoke(*session_id);
    CHECK_FALSE(store->validate(*session_id).has_value());
}

