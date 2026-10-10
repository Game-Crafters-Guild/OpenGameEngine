#pragma once

#include "Mathematics/AnonymousStruct.h"
#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
#include <type_traits>

namespace GameEngine {
namespace Mathematics {

// Lightweight 2D vector wrapper around float components with GLM interop.
struct Vector2
{
    union
    {
        GE_ANONYMOUS_STRUCT_BEGIN
        struct
        {
            float x;
            float y;
        };
        GE_ANONYMOUS_STRUCT_END
        /// The components as an array, aliasing `x` and `y`.
        float v[2];
    };

    Vector2() : x(0.0f), y(0.0f) {}
    Vector2(float x_, float y_) : x(x_), y(y_) {}
    explicit Vector2(const glm::vec2& vec) : x(vec.x), y(vec.y) {}

    bool operator==(const Vector2& other) const { return x == other.x && y == other.y; }

    /// Component by index: 0 is `x`, 1 is `y`. The caller keeps the index below 2.
    float& operator[](std::size_t index) { return v[index]; }
    const float& operator[](std::size_t index) const { return v[index]; }

    // Basic arithmetic
    Vector2 operator+(const Vector2& other) const { return Vector2(x + other.x, y + other.y); }
    Vector2 operator-(const Vector2& other) const { return Vector2(x - other.x, y - other.y); }
    Vector2 operator*(float scalar) const { return Vector2(x * scalar, y * scalar); }
    Vector2 operator/(float scalar) const { return Vector2(x / scalar, y / scalar); }

    // Length and normalization helpers (implemented via GLM for consistency).
    float Length() const
    {
        glm::vec2 vec(x, y);
        return glm::length(vec);
    }

    Vector2 Normalize() const
    {
        glm::vec2 vec(x, y);
        glm::vec2 n = glm::normalize(vec);
        return Vector2(n);
    }

    static float Dot(const Vector2& a, const Vector2& b)
    {
        return a.x * b.x + a.y * b.y;
    }

    // GLM interop.
    explicit operator glm::vec2() const { return glm::vec2(x, y); }
};

static_assert(std::is_standard_layout_v<Vector2>);
static_assert(sizeof(Vector2) == 2 * sizeof(float));

inline Vector2 operator*(float scalar, const Vector2& vec)
{
    return vec * scalar;
}

} // namespace Mathematics
} // namespace GameEngine
