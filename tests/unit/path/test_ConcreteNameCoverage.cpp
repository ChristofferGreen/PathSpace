#include "path/ConcreteName.hpp"
#include "third_party/doctest.h"

#include <string>
#include <string_view>
#include <unordered_set>

using namespace SP;

TEST_SUITE("path.concrete_name") {

TEST_CASE("constructors preserve referenced slices") {
    ConcreteName fromPtr{"alpha"};
    CHECK(fromPtr.getName() == std::string_view{"alpha"});

    std::string_view sv{"bravo"};
    ConcreteName fromView{sv};
    CHECK(fromView.getName().data() == sv.data());
    CHECK(fromView.getName().size() == sv.size());

    std::string backing{"/root/charlie"};
    ConcreteName fromIter{backing.begin() + 6, backing.end()};
    CHECK(fromIter.getName() == std::string_view{"charlie"});

    std::string_view deltaView{"delta_echo"};
    ConcreteName fromViewIter{deltaView.begin() + 6, deltaView.end()};
    CHECK(fromViewIter.getName() == std::string_view{"echo"});
}

TEST_CASE("comparison and ordering align with std::string_view semantics") {
    ConcreteName alpha{"alpha"};
    ConcreteName alphaCopy{"alpha"};
    ConcreteName bravo{"bravo"};

    CHECK(alpha == alphaCopy);
    CHECK(alpha == "alpha");
    CHECK((alpha <=> bravo) == std::strong_ordering::less);
    CHECK((bravo <=> alpha) == std::strong_ordering::greater);
}

TEST_CASE("hashing matches std::string_view equality") {
    std::unordered_set<ConcreteName> names;
    names.insert(ConcreteName{"zulu"});

    CHECK(names.count(ConcreteName{"zulu"}) == 1);
    CHECK(names.count(ConcreteName{"zulu"}) == 1); // identical value deduplicates
    CHECK(names.count(ConcreteName{"other"}) == 0);
}

}
