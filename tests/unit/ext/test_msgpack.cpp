#include <catch2/catch_test_macros.hpp>
#include <ext/msgpack.hpp>
struct Person {
  std::string name;
  uint16_t age;
  std::vector<std::string> aliases;

  bool operator==(Person const &other) const = default;

  template<class T>
  void pack(T &pack) {
    pack(name, age, aliases);
  }
};

TEST_CASE("Msgpack", "[Msgpack]") {
    SECTION("Simple Packing", "[Msgpack]") {
        auto person = Person{"John", 22, {"Ripper", "Silverhand"}};

        auto data = msgpack::pack(person); // Pack your object
        auto john = msgpack::unpack<Person>(data); // Unpack it
        REQUIRE(john==person);
    }

    SECTION("Simple Packing Into Vector", "[Msgpack]") {
        auto person = Person{"John", 22, {"Ripper", "Silverhand"}};

        std::vector<uint8_t> data;
        msgpack::pack(person, data); // Pack your object
        auto john = msgpack::unpack<Person>(data); // Unpack it
        REQUIRE(john==person);
    }
}