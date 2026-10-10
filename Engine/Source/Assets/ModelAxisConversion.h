#pragma once

// Shared source-axes -> engine-axes conversion math. Format loaders own
// their source frames (FbxImport::Make, GltfImport::SourceConversion,
// BlendImport::SourceConversion) and compose them here with bake/mirrors.
//
// Engine space is +X right, +Y up, +Z forward, left-handed. Mesh vertices,
// rest TRS, IBMs, and clip keys share one matrix per load.

#include "Assets/ModelImportOptions.h"

#include <cmath>

namespace GameEngine::ModelImport
{

struct AxisConversion
{
    // Row-major 3x3: target = m * source. Identity until a source frame /
    // ApplyEngineMirrors / ComposeEngineEulerDegrees; the Unity -X bake is
    // ApplyEngineMirrors(true, false, false), not this default.
    float m[3][3] = {
        { 1.0f, 0.0f, 0.0f},
        { 0.0f, 1.0f, 0.0f},
        { 0.0f, 0.0f, 1.0f},
    };
    bool reverseWinding = false;
};

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

inline float Determinant3x3(const float m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Negate engine axes after the source mapping. Each enabled axis flips that
// output row and the winding follows the composed determinant.
inline void ApplyEngineMirrors(AxisConversion& c, bool x, bool y, bool z)
{
    if (!x && !y && !z)
        return;
    for (int col = 0; col < 3; ++col)
    {
        if (x)
            c.m[0][col] = -c.m[0][col];
        if (y)
            c.m[1][col] = -c.m[1][col];
        if (z)
            c.m[2][col] = -c.m[2][col];
    }
    c.reverseWinding = Determinant3x3(c.m) < 0.0f;
}

// Extra engine-space Euler XYZ (degrees), applied AFTER axis conversion so
// mesh vertices, rest TRS, IBMs, and clip keys stay in one frame. Identity
// when all components are ~0. Winding follows the composed determinant.
inline void ComposeEngineEulerDegrees(AxisConversion& c, float degX, float degY, float degZ)
{
    constexpr float kEps = 1.0e-5f;
    if (std::fabs(degX) < kEps && std::fabs(degY) < kEps && std::fabs(degZ) < kEps)
        return;

    constexpr float kDegToRad = 0.017453292519943295f;
    const float x = degX * kDegToRad;
    const float y = degY * kDegToRad;
    const float z = degZ * kDegToRad;
    const float cx = std::cos(x), sx = std::sin(x);
    const float cy = std::cos(y), sy = std::sin(y);
    const float cz = std::cos(z), sz = std::sin(z);

    const float rx[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, cx,  -sx},
        {0.0f, sx,   cx},
    };
    const float ry[3][3] = {
        {cy,  0.0f, sy},
        {0.0f, 1.0f, 0.0f},
        {-sy, 0.0f, cy},
    };
    const float rz[3][3] = {
        {cz, -sz, 0.0f},
        {sz,  cz, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };

    auto mul = [](const float a[3][3], const float b[3][3], float out[3][3]) {
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col)
                out[r][col] = a[r][0] * b[0][col] + a[r][1] * b[1][col] + a[r][2] * b[2][col];
    };
    float ryx[3][3]{};
    mul(ry, rx, ryx);
    float r[3][3]{};
    mul(rz, ryx, r);

    float composed[3][3]{};
    mul(r, c.m, composed);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            c.m[row][col] = composed[row][col];
    c.reverseWinding = Determinant3x3(c.m) < 0.0f;
}

inline AxisConversion MakeEngineConversion(AxisConversion source, const AxisOptions& opts)
{
    ApplyEngineMirrors(source, opts.MirrorAxis[0], opts.MirrorAxis[1], opts.MirrorAxis[2]);
    float bakeDeg[3];
    opts.EffectiveBakeRotationDeg(bakeDeg);
    ComposeEngineEulerDegrees(source, bakeDeg[0], bakeDeg[1], bakeDeg[2]);
    return source;
}

inline Vec3 ConvertVec3(Vec3 v, const AxisConversion& c, float scale)
{
    return {
        (c.m[0][0] * v.x + c.m[0][1] * v.y + c.m[0][2] * v.z) * scale,
        (c.m[1][0] * v.x + c.m[1][1] * v.y + c.m[1][2] * v.z) * scale,
        (c.m[2][0] * v.x + c.m[2][1] * v.y + c.m[2][2] * v.z) * scale,
    };
}

inline Vec3 ConvertScale(Vec3 v, const AxisConversion& c)
{
    return {
        std::abs(c.m[0][0]) * v.x + std::abs(c.m[0][1]) * v.y + std::abs(c.m[0][2]) * v.z,
        std::abs(c.m[1][0]) * v.x + std::abs(c.m[1][1]) * v.y + std::abs(c.m[1][2]) * v.z,
        std::abs(c.m[2][0]) * v.x + std::abs(c.m[2][1]) * v.y + std::abs(c.m[2][2]) * v.z,
    };
}

inline void QuatToMatrix3x3(Quat q, float out[3][3])
{
    const float x = q.x;
    const float y = q.y;
    const float z = q.z;
    const float w = q.w;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    out[0][0] = 1.0f - 2.0f * (yy + zz);
    out[0][1] = 2.0f * (xy - wz);
    out[0][2] = 2.0f * (xz + wy);
    out[1][0] = 2.0f * (xy + wz);
    out[1][1] = 1.0f - 2.0f * (xx + zz);
    out[1][2] = 2.0f * (yz - wx);
    out[2][0] = 2.0f * (xz - wy);
    out[2][1] = 2.0f * (yz + wx);
    out[2][2] = 1.0f - 2.0f * (xx + yy);
}

inline Quat Matrix3x3ToQuat(const float m[3][3])
{
    Quat q{};
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }

    const float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq > 0.0f)
    {
        const float invLen = 1.0f / std::sqrt(lenSq);
        q.x *= invLen;
        q.y *= invLen;
        q.z *= invLen;
        q.w *= invLen;
    }
    else
    {
        q.w = 1.0f;
    }
    return q;
}

inline Quat ConvertQuat(Quat q, const AxisConversion& c)
{
    float r[3][3];
    QuatToMatrix3x3(q, r);

    float cr[3][3]{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                cr[row][col] += c.m[row][k] * r[k][col];

    float converted[3][3]{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                converted[row][col] += cr[row][k] * c.m[col][k];

    return Matrix3x3ToQuat(converted);
}

// 3x3 linear (row-major) plus translation through C R C^T / C t into
// column-major float[16]. Uniform scale applies only to the translation.
inline void ConvertAffine(const float src3x3[3][3],
                          float tx, float ty, float tz,
                          const AxisConversion& c,
                          float scale,
                          float* out16)
{
    float cs[3][3]{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                cs[row][col] += c.m[row][k] * src3x3[k][col];

    float dst[3][3]{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                dst[row][col] += cs[row][k] * c.m[col][k];

    out16[ 0] = dst[0][0];
    out16[ 1] = dst[1][0];
    out16[ 2] = dst[2][0];
    out16[ 3] = 0.0f;
    out16[ 4] = dst[0][1];
    out16[ 5] = dst[1][1];
    out16[ 6] = dst[2][1];
    out16[ 7] = 0.0f;
    out16[ 8] = dst[0][2];
    out16[ 9] = dst[1][2];
    out16[10] = dst[2][2];
    out16[11] = 0.0f;

    const Vec3 t = ConvertVec3({tx, ty, tz}, c, scale);
    out16[12] = t.x;
    out16[13] = t.y;
    out16[14] = t.z;
    out16[15] = 1.0f;
}

inline void ConvertColumnMajor16(const float in[16], const AxisConversion& c, float scale, float out[16])
{
    const float src3x3[3][3] = {
        {in[0], in[4], in[8]},
        {in[1], in[5], in[9]},
        {in[2], in[6], in[10]},
    };
    ConvertAffine(src3x3, in[12], in[13], in[14], c, scale, out);
}

} // namespace GameEngine::ModelImport
