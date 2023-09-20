#include "catch.hpp"

#include "PathSpace.hpp"

using namespace PathSpace;

TEST_CASE("Types") {
    SECTION("Fundamental Types Equality") {
        REQUIRE(typeid(short)==typeid(short int));
        REQUIRE(typeid(signed short)==typeid(short int));
        REQUIRE(typeid(signed short int)==typeid(short int));

        REQUIRE(typeid(signed)==typeid(int));
        REQUIRE(typeid(signed int)==typeid(int));

        REQUIRE(typeid(unsigned)==typeid(unsigned int));

        REQUIRE(typeid(long)==typeid(long int));
        REQUIRE(typeid(signed long int)==typeid(long int));

        REQUIRE(typeid(unsigned long)==typeid(unsigned long int));

        REQUIRE(typeid(long long)==typeid(long long int));
        REQUIRE(typeid(signed long long)==typeid(long long int));
        REQUIRE(typeid(signed long long int)==typeid(long long int));

        REQUIRE(typeid(unsigned long long)==typeid(unsigned long long int));

        REQUIRE(typeid(unsigned long long)!=typeid(int));
    }
}

TEST_CASE("TypeInfo") {
    SECTION("Int") {
        auto const tinfo = TypeInfo::Create<int>();
        REQUIRE(tinfo.element_size==sizeof(int));
        REQUIRE(tinfo.nbr_elements.has_value()==false);
        REQUIRE(tinfo.type==&typeid(int));
        REQUIRE(tinfo.arrayElementType==nullptr);
        REQUIRE(tinfo.isTriviallyCopyable==true);
        REQUIRE(tinfo.isInternalDataTriviallyCopyable==false);
        REQUIRE(tinfo.fundamentalType==TypeInfo::FundamentalTypes::Int);
        REQUIRE(tinfo.isPathSpace==false);
        REQUIRE(tinfo.isArray==false);
    }

    SECTION("std::string") {
        auto const tinfo = TypeInfo::Create<std::string>();
        REQUIRE(tinfo.element_size==sizeof(std::string::value_type));
        REQUIRE(tinfo.nbr_elements.has_value()==false);
        REQUIRE(tinfo.type==&typeid(std::string));
        REQUIRE(tinfo.arrayElementType==&typeid(std::string::value_type));
        REQUIRE(tinfo.isTriviallyCopyable==false);
        REQUIRE(tinfo.isInternalDataTriviallyCopyable==true);
        REQUIRE(tinfo.fundamentalType==TypeInfo::FundamentalTypes::None);
        REQUIRE(tinfo.isPathSpace==false);
        REQUIRE(tinfo.isArray==false);
    }

    SECTION("Folder") {
        auto const tinfo = TypeInfo::Create<Folder>();
        REQUIRE(tinfo.element_size==sizeof(Folder));
        REQUIRE(tinfo.nbr_elements.has_value()==false);
        REQUIRE(tinfo.type==&typeid(Folder));
        REQUIRE(tinfo.arrayElementType==nullptr);
        REQUIRE(tinfo.isTriviallyCopyable==false);
        REQUIRE(tinfo.isInternalDataTriviallyCopyable==false);
        REQUIRE(tinfo.fundamentalType==TypeInfo::FundamentalTypes::None);
        REQUIRE(tinfo.isPathSpace==true);
        REQUIRE(tinfo.isArray==false);
    }

    SECTION("std::vector<int>") {
        auto const tinfo = TypeInfo::Create<std::vector<int>>();
        REQUIRE(tinfo.element_size==sizeof(int));
        REQUIRE(tinfo.nbr_elements.has_value()==false);
        REQUIRE(tinfo.type==&typeid(std::vector<int>));
        REQUIRE(tinfo.arrayElementType==&typeid(int));
        REQUIRE(tinfo.isTriviallyCopyable==false);
        REQUIRE(tinfo.isInternalDataTriviallyCopyable==true);
        REQUIRE(tinfo.fundamentalType==TypeInfo::FundamentalTypes::None);
        REQUIRE(tinfo.isPathSpace==false);
        REQUIRE(tinfo.isArray==false);
    }

    /*SECTION("std::vector<std::string>") {
        auto const tinfo = TypeInfo::Create<std::vector<std::string>>();
        REQUIRE(tinfo.element_size==sizeof(std::string::value_type));
        REQUIRE(tinfo.nbr_elements.has_value()==false);
        REQUIRE(tinfo.type==&typeid(std::vector<std::string>));
        REQUIRE(tinfo.arrayElementType==&typeid(std::string::value_type));
        REQUIRE(tinfo.isTriviallyCopyable==false);
        REQUIRE(tinfo.isInternalDataTriviallyCopyable==true);
        REQUIRE(tinfo.isFundamental==false);
        REQUIRE(tinfo.isPathSpace==false);
        REQUIRE(tinfo.isArray==false);
    }*/
}
