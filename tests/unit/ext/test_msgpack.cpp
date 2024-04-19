#include "ext/doctest.h"
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

TEST_CASE("External: Msgpack") {
    SUBCASE("Simple Packing") {
        auto person = Person{"John", 22, {"Ripper", "Silverhand"}};

        auto data = msgpack::pack(person); // Pack your object
        auto john = msgpack::unpack<Person>(data); // Unpack it
        REQUIRE(john==person);
    }

    SUBCASE("Simple Packing Into Vector") {
        auto person = Person{"John", 22, {"Ripper", "Silverhand"}};

        std::vector<uint8_t> data;
        msgpack::pack(person, data); // Pack your object
        auto john = msgpack::unpack<Person>(data); // Unpack it
        REQUIRE(john==person);
    }
}