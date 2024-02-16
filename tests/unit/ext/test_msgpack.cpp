#include <catch2/catch_test_macros.hpp>
#include <ext/msgpack.hpp>
struct Person {
  std::string name;
  uint16_t age;
  std::vector<std::string> aliases;

  template<class T>
  void pack(T &pack) {
    pack(name, age, aliases);
  }
};

TEST_CASE("Msgpack", "[Msgpack]") {
    SECTION("Simple PathSpace Construction", "[PathSpace]") {
        auto person = Person{"John", 22, {"Ripper", "Silverhand"}};

        auto data = msgpack::pack(person); // Pack your object
        auto john = msgpack::unpack<Person>(data); // Unpack it
        REQUIRE(john.name=="John");
    }
}