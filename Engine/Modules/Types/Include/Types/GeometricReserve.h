#pragma once

// Capacity provisioning for containers whose release path must not allocate.
// A resource retires in two steps -- invalidate it, then transfer it to a free
// list -- and an allocation failure between those steps strands the resource
// with no owner. Callers reserve the release storage at acquisition instead,
// where failing is still harmless because nothing has been invalidated yet.

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace GameEngine
{

inline constexpr size_t kMinimumGeometricCapacity = 8;

// Next capacity step for `current`, clamped so the doubling cannot overflow
// `maximum`. Doubling keeps repeated acquisition amortised constant.
[[nodiscard]] inline constexpr size_t NextGeometricCapacity(size_t current, size_t maximum)
{
    if (current > maximum / 2)
        return maximum;
    return std::max(kMinimumGeometricCapacity, current * 2);
}

// Reserve room for at least `required` elements without shrinking.
template <class T>
void ReserveGeometric(std::vector<T>& values, size_t required)
{
    if (required <= values.capacity())
        return;
    values.reserve(std::max(required, NextGeometricCapacity(values.capacity(), values.max_size())));
}

// Reserve room for `count` more elements so the appends that follow cannot
// fail. Throws std::length_error when the container can never hold them.
template <class T>
void ReserveForAppend(std::vector<T>& values, size_t count)
{
    if (count > values.max_size() - values.size())
        throw std::length_error("append would exceed the container's maximum size");
    ReserveGeometric(values, values.size() + count);
}

} // namespace GameEngine
