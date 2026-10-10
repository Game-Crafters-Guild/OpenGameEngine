#pragma once

// Minimal GLSL type + builtin shim, so a block of shader source can be compiled as C++ and
// exercised on the host. It serves every block that ExtractShaderBlock.cmake lifts verbatim out of
// the shipped shaders (grep GE_SHARED_ for the current set, across the terrain, grass and shared
// surface includes) and carries only what those blocks use, so an unfamiliar construct in a newly
// extracted block is expected to need a small addition here.
//
// It carries NO SWIZZLES, deliberately: `.xy` and `.rg` cannot be data members without union
// punning, and a shim that fakes them would be a second implementation of the thing under test.
// A block written to be extracted is written component-wise instead.
//
// The semantics that have to match, or the host result stops standing for the shader's:
//   * mat2 is COLUMN-major, exactly as in GLSL: mat2(a, b, c, d) has columns (a, b) and (c, d).
//   * every scalar is float32. The shared block suffixes its float literals `f` so neither
//     compiler promotes an expression to double; an unsuffixed literal narrows and MSVC's C4244
//     turns it into a build error here, which is the intended signal.
//   * % on int is C++'s remainder, which is SPIR-V's OpSRem — the same operation GLSL emits, and
//     the same one GLSL leaves undefined for negative operands. Shader code that needs a total
//     residue does the fold itself; this shim does not paper over it.
//   * uint arithmetic wraps modulo 2^32 in both languages.

#include <cassert>
#include <cmath>
#include <cstdint>

namespace GameEngine::GlslShim
{

using uint = std::uint32_t;

struct vec2;

struct ivec2
{
    int x = 0;
    int y = 0;

    ivec2() = default;
    ivec2(int inX, int inY) : x(inX), y(inY) {}
    explicit ivec2(const vec2& v); // GLSL ivec2(vec2): truncates toward zero

    friend ivec2 operator+(ivec2 a, ivec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend bool operator==(ivec2 a, ivec2 b) { return a.x == b.x && a.y == b.y; }
    friend bool operator!=(ivec2 a, ivec2 b) { return !(a == b); }
};

struct vec2
{
    float x = 0.0f;
    float y = 0.0f;

    vec2() = default;
    vec2(float inX, float inY) : x(inX), y(inY) {}
    explicit vec2(ivec2 v) : x(static_cast<float>(v.x)), y(static_cast<float>(v.y)) {}

    vec2& operator*=(float s)
    {
        x *= s;
        y *= s;
        return *this;
    }

    friend vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend vec2 operator*(vec2 a, float s) { return {a.x * s, a.y * s}; }
    friend vec2 operator*(float s, vec2 a) { return {a.x * s, a.y * s}; }
    friend vec2 operator/(vec2 a, float s) { return {a.x / s, a.y / s}; }
    // GLSL broadcasts a scalar across the vector in the mixed forms. The tangent-normal decode
    // relies on it: `s.xy * 2.0f - 1.0f` subtracts 1 from BOTH components, not just x.
    friend vec2 operator-(vec2 a, float s) { return {a.x - s, a.y - s}; }
    friend vec2 operator+(vec2 a, float s) { return {a.x + s, a.y + s}; }
};

inline float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(vec2 v) { return std::sqrt(dot(v, v)); }

inline ivec2::ivec2(const vec2& v) : x(static_cast<int>(v.x)), y(static_cast<int>(v.y)) {}

struct vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    vec3() = default;
    vec3(float inX, float inY, float inZ) : x(inX), y(inY), z(inZ) {}
    vec3(vec2 xy, float inZ) : x(xy.x), y(xy.y), z(inZ) {}
};
inline float dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vec3 operator+(const vec3& a, const vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(const vec3& a, const vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator*(const vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline vec3 operator*(float s, const vec3& a) { return a * s; }
inline vec3 normalize(const vec3& v)
{
    const float len = std::sqrt(dot(v, v));
    // GLSL leaves normalize(0) undefined, so a shader that reaches it has a bug. Assert rather than
    // substitute a plausible value, for the same reason vec4::operator[] asserts on a bad index: a
    // silent fallback here would reproduce the bug as a believable number.
    assert(len > 0.0f);
    return {v.x / len, v.y / len, v.z / len};
}
inline vec3 mix(const vec3& a, const vec3& b, float t) { return a * (1.0f - t) + b * t; }
inline float length(const vec3& v) { return std::sqrt(dot(v, v)); }
inline vec3 cross(const vec3& a, const vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// vec4 exists for the blend-selection block, whose whole point is dynamic component indexing:
// weights[i] both reads and WRITES, so operator[] returns a reference. GLSL indexes a vector with
// an int and leaves an out-of-range index undefined; this asserts instead, because a shader bug
// that silently read a neighbouring component would otherwise reproduce here as a plausible number.
//
// The components must stay adjacent and in declaration order for (&x)[i] to be the right component.
struct vec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    vec4() = default;
    explicit vec4(float broadcast) : x(broadcast), y(broadcast), z(broadcast), w(broadcast) {}
    vec4(float inX, float inY, float inZ, float inW) : x(inX), y(inY), z(inZ), w(inW) {}
    // GLSL's vec4(vec3, float): how a sampled RGB texel reaches the tangent-normal decode.
    vec4(const vec3& xyz, float inW) : x(xyz.x), y(xyz.y), z(xyz.z), w(inW) {}

    float& operator[](int i)
    {
        assert(i >= 0 && i < 4);
        return (&x)[i];
    }
    float operator[](int i) const
    {
        assert(i >= 0 && i < 4);
        return (&x)[i];
    }

    friend vec4 operator/(const vec4& a, float s) { return {a.x / s, a.y / s, a.z / s, a.w / s}; }
};

struct mat2
{
    // Columns, as GLSL stores them.
    vec2 c0;
    vec2 c1;

    mat2() = default;
    mat2(float m00, float m01, float m10, float m11) : c0(m00, m01), c1(m10, m11) {}

    friend vec2 operator*(const mat2& m, vec2 v) { return {m.c0.x * v.x + m.c1.x * v.y, m.c0.y * v.x + m.c1.y * v.y}; }
};

inline float floor(float v) { return std::floor(v); }
inline vec2 floor(vec2 v) { return {std::floor(v.x), std::floor(v.y)}; }
inline float fract(float v) { return v - std::floor(v); }
inline vec2 fract(vec2 v) { return {fract(v.x), fract(v.y)}; }
inline float step(float edge, float v) { return v < edge ? 0.0f : 1.0f; }
// GLSL max(a, b) is `a < b ? b : a` — it returns a when either operand is NaN, unlike std::fmax.
inline float max(float a, float b) { return a < b ? b : a; }
// GLSL min(a, b) is `b < a ? b : a`, the mirror of max, and likewise returns a on a NaN operand.
inline float min(float a, float b) { return b < a ? b : a; }
inline uint min(uint a, uint b) { return b < a ? b : a; }
inline int min(int a, int b) { return b < a ? b : a; }
inline int max(int a, int b) { return a < b ? b : a; }
// GLSL clamp(x, lo, hi) is min(max(x, lo), hi). It agrees with std::clamp on every finite input
// AND on NaN — both pass it through — which is what lets an extracted block be compared bit for
// bit against a std::clamp-based C++ mirror rather than approximately.
inline float clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float pow(float x, float y) { return std::pow(x, y); }
inline float ceil(float v) { return std::ceil(v); }
inline float log2(float v) { return std::log2(v); }
// GLSL smoothstep: the Hermite ramp over [edge0, edge1], clamped outside it.
inline float smoothstep(float edge0, float edge1, float x)
{
    const float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float acos(float v) { return std::acos(v); }
// GLSL has both arities: atan(y_over_x) is the principal value in [-pi/2, pi/2], atan(y, x) is the
// full-circle form std::atan2 computes. GLSL leaves atan(0, 0) undefined, so a block that can reach
// the origin guards the call instead of relying on std::atan2 answering 0 there.
inline float atan(float v) { return std::atan(v); }
inline float atan(float y, float x) { return std::atan2(y, x); }
inline int abs(int v) { return v < 0 ? -v : v; }
inline float abs(float v) { return std::fabs(v); }
// GLSL mod() is floored, unlike std::fmod which truncates. They agree only for a non-negative
// dividend, and the hex rotation feeds it negative angles.
inline float mod(float v, float m) { return v - m * std::floor(v / m); }
inline float sin(float v) { return std::sin(v); }
inline vec2 sin(vec2 v) { return {std::sin(v.x), std::sin(v.y)}; }
inline float cos(float v) { return std::cos(v); }

} // namespace GameEngine::GlslShim
