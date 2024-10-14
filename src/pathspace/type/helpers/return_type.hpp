#pragma once
/*
    Defines a helper template ReturnTypeInfo that can return a type_info pointer
    for any type. If the type is a function pointer, it will return the return value type.
*/

template <typename T>
struct return_type_helper {
    using type = std::remove_cvref_t<T>;
};

template <typename R, typename... Args>
struct return_type_helper<R (*)(Args...)> {
    using type = R;
};

template <typename R, typename... Args>
struct return_type_helper<R(Args...)> {
    using type = R;
};

template <typename T>
concept Invocable = requires(T t) { std::invoke(t); };

template <Invocable T>
struct return_type_helper<T> {
    using type = std::invoke_result_t<T>;
};

template <typename T>
std::type_info const* ReturnTypeInfo = &typeid(typename return_type_helper<std::remove_cvref_t<T>>::type);