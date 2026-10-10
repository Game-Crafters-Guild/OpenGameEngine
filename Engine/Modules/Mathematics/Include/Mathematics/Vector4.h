#pragma once

#include "Mathematics/AnonymousStruct.h"
#include "Mathematics/Vector3.h"
#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
#include <type_traits>

namespace GameEngine {
namespace Mathematics {

// 4D vector wrapper with convenience methods and GLM interop.
struct Vector4
{
    union
    {
        GE_ANONYMOUS_STRUCT_BEGIN
        struct
        {
            float x;
            float y;
            float z;
            float w;
        };
        GE_ANONYMOUS_STRUCT_END
        /// The components as an array, aliasing `x`, `y`, `z` and `w`.
        float v[4];
    };

    Vector4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
    Vector4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vector4(const Vector3& vec, float w_) : x(vec.x), y(vec.y), z(vec.z), w(w_) {}
    explicit Vector4(const glm::vec4& vec) : x(vec.x), y(vec.y), z(vec.z), w(vec.w) {}

    /// Component by index: 0 is `x`, 1 is `y`, 2 is `z`, 3 is `w`. The caller keeps the index below 4.
    float& operator[](std::size_t index) { return v[index]; }
    const float& operator[](std::size_t index) const { return v[index]; }

    // Basic arithmetic
    Vector4 operator+(const Vector4& other) const { return Vector4(x + other.x, y + other.y, z + other.z, w + other.w); }
    Vector4 operator-(const Vector4& other) const { return Vector4(x - other.x, y - other.y, z - other.z, w - other.w); }
    Vector4 operator*(float scalar) const { return Vector4(x * scalar, y * scalar, z * scalar, w * scalar); }

    float Dot(const Vector4& other) const
    {
        return x * other.x + y * other.y + z * other.z + w * other.w;
    }

    // GLM interop.
    explicit operator glm::vec4() const { return glm::vec4(x, y, z, w); }
};

static_assert(std::is_standard_layout_v<Vector4>);
static_assert(sizeof(Vector4) == 4 * sizeof(float));

inline Vector4 operator*(float scalar, const Vector4& vec)
{
    return vec * scalar;
}

} // namespace Mathematics
} // namespace GameEngine
