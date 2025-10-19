#include "third_party/doctest.h"

#include <alpaca/alpaca.h>

struct Person {
    std::string name;
    uint16_t age;
    std::vector<std::string> aliases;

    bool operator==(Person const& other) const = default;

    template <class T>
    void pack(T& pack) {
        pack(name, age, aliases);
    }
};

TEST_CASE("External: Alpaca") {
    auto const person = Person{"John", 22, {"Ripper", "Silverhand"}};
    std::vector<uint8_t> bytes;
    SUBCASE("Simple struct") {
        alpaca::serialize(person, bytes);
        std::error_code ec;
        auto john = alpaca::deserialize<Person>(bytes, ec);
        REQUIRE_FALSE(ec);
        REQUIRE(john == person);
    }
}