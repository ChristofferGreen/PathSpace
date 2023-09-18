#include <optional>

namespace PathSpace {


struct TypeInfo {
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
            .isFundamental = std::is_fundamental<T>::value
        };
    }

    std::size_t element_size = 0;
    std::optional<int> nbr_elements; // Some types can include number of elements, for example arrays
    std::type_info const *type = nullptr;
    std::type_info const *arrayElementType = nullptr; // Some types are better handled like if they were another type: std::string/char*
    bool isTriviallyCopyable = false;
    bool isInternalDataTriviallyCopyable = false; // The internal data of for example std::vector
    bool isFundamental = false;
    bool isPathSpace = false;
    bool isArray = false;
};

struct Folder {

};

}