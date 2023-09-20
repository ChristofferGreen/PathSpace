#include <optional>

namespace PathSpace {

struct TypeInfo {
    enum struct FundamentalTypes {
        None = 0,

        SignedChar,
        UnsignedChar,
        ShortInt,
        UnsignedShortInt,
        Int,
        UnsignedInt,
        LongInt,
        UnsignedLongInt,
        LongLongInt,
        UnsignedLongLongInt,

        Bool,

        Char,
        WCharT,
        Char8T,
        Char16T,
        Char32T,

        Float,
        Double,
        LongDouble
    };

    template<typename T>
    static consteval auto DetermineFundamentality() -> FundamentalTypes {
        if(std::is_same_v<T, signed char>)
            return FundamentalTypes::SignedChar;
        else if(std::is_same_v<T, unsigned char>)
            return FundamentalTypes::UnsignedChar;
        else if(std::is_same_v<T, short int>)
            return FundamentalTypes::ShortInt;
        else if(std::is_same_v<T, unsigned short int>)
            return FundamentalTypes::UnsignedShortInt;
        else if(std::is_same_v<T, int>)
            return FundamentalTypes::Int;
        else if(std::is_same_v<T, unsigned int>)
            return FundamentalTypes::UnsignedInt;
        else if(std::is_same_v<T, long int>)
            return FundamentalTypes::LongInt;
        else if(std::is_same_v<T, unsigned long int>)
            return FundamentalTypes::UnsignedLongInt;
        else if(std::is_same_v<T, long long int>)
            return FundamentalTypes::LongLongInt;
        else if(std::is_same_v<T, unsigned long long int>)
            return FundamentalTypes::LongLongInt;
        else if(std::is_same_v<T, bool>)
            return FundamentalTypes::Bool;
        else if(std::is_same_v<T, char>)
            return FundamentalTypes::Char;
        else if(std::is_same_v<T, wchar_t>)
            return FundamentalTypes::WCharT;
        else if(std::is_same_v<T, char8_t>)
            return FundamentalTypes::Char8T;
        else if(std::is_same_v<T, char16_t>)
            return FundamentalTypes::Char16T;
        else if(std::is_same_v<T, char32_t>)
            return FundamentalTypes::Char32T;
        else if(std::is_same_v<T, float>)
            return FundamentalTypes::Float;
        else if(std::is_same_v<T, double>)
            return FundamentalTypes::Double;
        else if(std::is_same_v<T, long double>)
            return FundamentalTypes::LongDouble;
        return FundamentalTypes::None;
    }

    // std::string
    template<typename T>
    requires std::same_as<T, std::string>
    static consteval auto Create() -> TypeInfo {
        return {
            .element_size = sizeof(std::string::value_type),
            .type = &typeid(std::string),
            .arrayElementType = &typeid(std::string::value_type),
            .isInternalDataTriviallyCopyable = true
        };
    }
    
    // std::vector<scalar>
    template<typename T>
    requires std::same_as<T, std::vector<typename T::value_type>> && std::is_scalar<typename T::value_type>::value
    static consteval auto Create() -> TypeInfo {
        return {
            .element_size = sizeof(typename T::value_type),
            .type = &typeid(T),
            .arrayElementType = &typeid(typename T::value_type),
            .isInternalDataTriviallyCopyable = true
        };
    }

    // PathSpace2
    /*template<typename T>
    requires IsPathSpace2<T>
    static consteval auto Create() -> TypeInfo {
        return {
            .element_size = sizeof(T),
            .type = &typeid(T),
            .isPathSpace = true
        };
    }*/

    // General case
    template<typename T>
    static consteval auto Create() -> TypeInfo {
        return {
            .element_size = sizeof(T),
            .type = &typeid(T),
            .isTriviallyCopyable = std::is_trivially_copyable<T>::value,
            .fundamentalType = DetermineFundamentality<T>()
        };
    }

    std::size_t element_size = 0;
    std::optional<int> nbr_elements; // Some types can include number of elements, for example arrays
    std::type_info const *type = nullptr;
    std::type_info const *arrayElementType = nullptr; // Some types are better handled like if they were another type: std::string/char*
    bool isTriviallyCopyable = false;
    bool isInternalDataTriviallyCopyable = false; // The internal data of for example std::vector
    bool isPathSpace = false;
    bool isArray = false;
    FundamentalTypes fundamentalType = FundamentalTypes::None;
};

struct Folder {

};

}