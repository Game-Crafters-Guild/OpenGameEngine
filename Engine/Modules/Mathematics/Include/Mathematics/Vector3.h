#pragma once

#include "Mathematics/AnonymousStruct.h"
#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
#include <type_traits>

namespace GameEngine {
namespace Mathematics {

// 3D vector wrapper with convenience methods and GLM interop.
struct Vector3
{
    union
    {
        GE_ANONYMOUS_STRUCT_BEGIN
        struct
        {
            float x;
            float y;
            float z;
        };
        GE_ANONYMOUS_STRUCT_END
        /// The components as an array, aliasing `x`, `y` and `z`.
        float v[3];
    };

    Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vector3(const glm::vec3& vec) : x(vec.x), y(vec.y), z(vec.z) {}

    /// Component by index: 0 is `x`, 1 is `y`, 2 is `z`. The caller keeps the index below 3.
    float& operator[](std::size_t index) { return v[index]; }
    const float& operator[](std::size_t index) const { return v[index]; }

    // Basic arithmetic
    Vector3 operator+(const Vector3& other) const { return Vector3(x + other.x, y + other.y, z + other.z); }
    Vector3 operator-(const Vector3& other) const { return Vector3(x - other.x, y - other.y, z - other.z); }
    Vector3 operator*(float scalar) const { return Vector3(x * scalar, y * scalar, z * scalar); }
    Vector3 operator/(float scalar) const { return Vector3(x / scalar, y / scalar, z / scalar); }

    // Length and normalization helpers (implemented via GLM for consistency).
    float Length() const
    {
        glm::vec3 vec(x, y, z);
        return glm::length(vec);
    }

    float LengthSquared() const
    {
        return x * x + y * y + z * z;
    }

    Vector3 Normalize() const
    {
        glm::vec3 vec(x, y, z);
        glm::vec3 n = glm::normalize(vec);
        return Vector3(n);
    }

    // Unit-length copy, or the zero vector when this vector has zero length
    // (where Normalize() yields NaN components).
    Vector3 NormalizeOrZero() const
    {
        const float lengthSquared = LengthSquared();
        if (lengthSquared <= 0.0f)
            return Vector3();
        return *this * (1.0f / std::sqrt(lengthSquared));
    }

    static float Dot(const Vector3& a, const Vector3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vector3 Cross(const Vector3& a, const Vector3& b)
    {
        return Vector3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        );
    }

    // GLM interop.
    explicit operator glm::vec3() const { return glm::vec3(x, y, z); }
};

static_assert(std::is_standard_layout_v<Vector3>);
static_assert(sizeof(Vector3) == 3 * sizeof(float));

inline Vector3 operator*(float scalar, const Vector3& vec)
{
    return vec * scalar;
}

} // namespace Mathematics
} // namespace GameEngine
