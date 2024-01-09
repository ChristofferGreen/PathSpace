#pragma once
#include <vector>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace SP {

template <typename T>
class PopFrontVector {
public:
    // Default constructor, copy constructor, move constructor, destructor
    // can rely on std::vector's implementations.

    void push_back(const T& value) {
        vec.push_back(value);
    }

    T* data() {
        return &this->vec.at(this->frontIndex);
    }

    T const* data() const {
        return &this->vec.at(this->frontIndex);
    }

    template <typename... Args>
    void emplace_back(Args&&... args) {
        vec.emplace_back(std::forward<Args>(args)...);
    }

    void pop_front() {
        if (isEmpty()) {
            throw std::out_of_range("PopFrontVector is empty");
        }
        frontIndex++;
        performGarbageCollectionIfNeeded();
    }

    T& operator[](size_t index) {
        return vec.at(frontIndex + index); // Use at for bounds checking
    }

    const T& operator[](size_t index) const {
        return vec.at(frontIndex + index);
    }

    bool isEmpty() const {
        return frontIndex >= vec.size();
    }

    size_t size() const {
        return vec.size() - frontIndex;
    }

    void clear() {
        vec.clear();
        frontIndex = 0;
    }

    // Iterator support
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    iterator begin() {
        return vec.begin() + frontIndex;
    }

    iterator end() {
        return vec.end();
    }

    const_iterator begin() const {
        return vec.cbegin() + frontIndex;
    }

    const_iterator end() const {
        return vec.cend();
    }

    const_iterator cbegin() const {
        return vec.cbegin() + frontIndex;
    }

    const_iterator cend() const {
        return vec.cend();
    }

private:
    std::vector<T> vec;
    size_t frontIndex = 0;

    void performGarbageCollectionIfNeeded() {
        if (frontIndex > vec.size() * 0.3) { // Customize the threshold as needed
            vec = std::vector<T>(begin(), end());
            frontIndex = 0;
        }
    }
};

} // namespace SP