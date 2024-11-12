#pragma once
#include <chrono>

namespace SP {

struct ExecutionOptions {
    enum class Category {
        Immediate,
        Lazy
    };

    Category category = Category::Immediate;
};

} // namespace SP