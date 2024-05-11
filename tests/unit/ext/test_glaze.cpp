#include "ext/doctest.h"

#include "glaze/glaze.hpp"

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

/*TEST_CASE("External: Glaze") {
    auto const person = Person{"John", 22, {"Ripper", "Silverhand"}};
    std::vector<std::byte> bytes;
    SUBCASE("Simple struct") {
        glz::write_binary_untagged(person, bytes);
        Person john{};
        auto error = glz::read_binary_untagged(john, bytes);
        REQUIRE(error == false);
        REQUIRE(john==person);
    }
}*/