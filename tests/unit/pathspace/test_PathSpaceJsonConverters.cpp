#include "third_party/doctest.h"

#include <nlohmann/json.hpp>
#include <pathspace/tools/PathSpaceJsonConverters.hpp>

#include <optional>
#include <string>
#include <typeindex>
#include <utility>

using namespace SP;

namespace {

struct Widget {
    int value;
};

struct Gadget {
    int value;
};

struct Unregistered {
    int value;
};

template <typename T>
class SimpleReader final : public detail::PathSpaceJsonValueReader {
public:
    explicit SimpleReader(T v, bool fail = false) : value(std::move(v)), shouldFail(fail) {}

private:
    auto popImpl(void* destination, InputMetadata const& metadata) -> std::optional<Error> override {
        if (shouldFail) {
            return Error{Error::Code::InvalidType, "forced failure"};
        }
        if (metadata.typeInfo != &typeid(T)) {
            return Error{Error::Code::InvalidType, "type mismatch"};
        }
        auto* target = static_cast<T*>(destination);
        *target = value;
        return std::nullopt;
    }

    T    value;
    bool shouldFail;
};

class FailingReader : public detail::PathSpaceJsonValueReader {
private:
    auto popImpl(void*, InputMetadata const&) -> std::optional<Error> override {
        return Error{Error::Code::InvalidType, "boom"};
    }
};

} // namespace

TEST_SUITE_BEGIN("pathspace.json.converters");

TEST_CASE("custom converter registers and converts value") {
    SimpleReader<Widget> reader{Widget{17}};
    PathSpaceJsonRegisterConverterAs<Widget>("WidgetType", [](Widget const& w) {
        nlohmann::json j;
        j["value"] = w.value;
        return j;
    });

    auto converted = detail::ConvertWithRegisteredConverter(std::type_index(typeid(Widget)), reader);
    REQUIRE(converted.has_value());
    CHECK(converted->at("value") == 17);

    auto typeName = detail::DescribeRegisteredType(std::type_index(typeid(Widget)));
    CHECK(typeName == std::string("WidgetType"));
}

TEST_CASE("converter propagates pop failure as nullopt") {
    PathSpaceJsonRegisterConverter<Gadget>([](Gadget const& g) {
        nlohmann::json j;
        j["value"] = g.value;
        return j;
    });

    FailingReader reader;
    auto converted = detail::ConvertWithRegisteredConverter(std::type_index(typeid(Gadget)), reader);
    CHECK_FALSE(converted.has_value());
}

TEST_CASE("converter default type name uses typeid when no custom name provided") {
    SimpleReader<Gadget> reader{Gadget{9}};
    PathSpaceJsonRegisterConverter<Gadget>([](Gadget const& g) {
        nlohmann::json j;
        j["value"] = g.value;
        return j;
    });

    auto converted = detail::ConvertWithRegisteredConverter(std::type_index(typeid(Gadget)), reader);
    REQUIRE(converted.has_value());
    CHECK(converted->at("value") == 9);

    auto typeName = detail::DescribeRegisteredType(std::type_index(typeid(Gadget)));
    CHECK(typeName == std::string(typeid(Gadget).name()));
}

TEST_CASE("unregistered types return nullopt and fall back to typeid name") {
    SimpleReader<Unregistered> reader{Unregistered{1}};

    auto converted = detail::ConvertWithRegisteredConverter(std::type_index(typeid(Unregistered)), reader);
    CHECK_FALSE(converted.has_value());

    auto typeName = detail::DescribeRegisteredType(std::type_index(typeid(Unregistered)));
    CHECK(typeName == std::string(typeid(Unregistered).name()));
}

TEST_CASE("converter registration overwrites existing entries") {
    SimpleReader<Widget> reader{Widget{42}};
    PathSpaceJsonRegisterConverterAs<Widget>("First", [](Widget const& w) {
        nlohmann::json j;
        j["value"] = w.value;
        j["tag"] = "first";
        return j;
    });

    PathSpaceJsonRegisterConverterAs<Widget>("Second", [](Widget const& w) {
        nlohmann::json j;
        j["value"] = w.value + 1;
        j["tag"] = "second";
        return j;
    });

    auto converted = detail::ConvertWithRegisteredConverter(std::type_index(typeid(Widget)), reader);
    REQUIRE(converted.has_value());
    CHECK(converted->at("value") == 43);
    CHECK(converted->at("tag") == "second");

    auto typeName = detail::DescribeRegisteredType(std::type_index(typeid(Widget)));
    CHECK(typeName == std::string("Second"));
}

TEST_CASE("PathSpaceJsonValueReader pop forwards to popImpl") {
    SimpleReader<Widget> reader{Widget{33}};
    Widget out{0};

    auto err = reader.pop(out);
    CHECK_FALSE(err.has_value());
    CHECK(out.value == 33);
}
