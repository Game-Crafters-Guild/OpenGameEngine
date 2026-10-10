#pragma once

// FBX adapter: ufbx_coordinate_axes -> ModelImport::AxisConversion, plus
// ufbx_vec3 / ufbx_quat / ufbx_matrix wrappers around ModelImport convert
// helpers. Shared bake/mirror math lives in ModelAxisConversion.h.

#include "ModelAxisConversion.h"
#include "Assets/FbxLoaderOptions.h"

#include <ufbx.h>

namespace GameEngine::FbxImport
{

inline bool AxisVector(ufbx_coordinate_axis axis, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    switch (axis)
    {
    case UFBX_COORDINATE_AXIS_POSITIVE_X: out[0] =  1.0f; return true;
    case UFBX_COORDINATE_AXIS_NEGATIVE_X: out[0] = -1.0f; return true;
    case UFBX_COORDINATE_AXIS_POSITIVE_Y: out[1] =  1.0f; return true;
    case UFBX_COORDINATE_AXIS_NEGATIVE_Y: out[1] = -1.0f; return true;
    case UFBX_COORDINATE_AXIS_POSITIVE_Z: out[2] =  1.0f; return true;
    case UFBX_COORDINATE_AXIS_NEGATIVE_Z: out[2] = -1.0f; return true;
    default: return false;
    }
}

inline ModelImport::AxisConversion Make(ufbx_coordinate_axes axes)
{
    ModelImport::AxisConversion c{};

    float sourceRight[3], sourceUp[3], sourceFront[3];
    if (!AxisVector(axes.right, sourceRight)
        || !AxisVector(axes.up, sourceUp)
        || !AxisVector(axes.front, sourceFront))
    {
        return c;
    }

    // Source axes -> engine +X right, +Y up, +Z forward. No implicit Unity
    // -X bake: that is ApplyEngineMirrors (default X on, stored in kv).
    const float targetAxes[3][3] = {
        { 1.0f, 0.0f, 0.0f}, // source right -> engine +X
        { 0.0f, 1.0f, 0.0f}, // source up    -> engine up
        { 0.0f, 0.0f, 1.0f}, // source front -> engine +Z
    };
    const float sourceAxes[3][3] = {
        {sourceRight[0], sourceUp[0], sourceFront[0]},
        {sourceRight[1], sourceUp[1], sourceFront[1]},
        {sourceRight[2], sourceUp[2], sourceFront[2]},
    };

    for (int r = 0; r < 3; ++r)
    {
        for (int col = 0; col < 3; ++col)
        {
            c.m[r][col] = targetAxes[0][r] * sourceAxes[col][0]
                        + targetAxes[1][r] * sourceAxes[col][1]
                        + targetAxes[2][r] * sourceAxes[col][2];
        }
    }

    c.reverseWinding = ModelImport::Determinant3x3(c.m) < 0.0f;
    return c;
}

inline ModelImport::AxisConversion MakeEngineConversion(ufbx_coordinate_axes axes, const FbxLoaderOptions& opts)
{
    return ModelImport::MakeEngineConversion(Make(axes), opts.Axis);
}

inline ufbx_vec3 ConvertVec3(const ufbx_vec3& v, const ModelImport::AxisConversion& c, float scale)
{
    const ModelImport::Vec3 r = ModelImport::ConvertVec3(
        {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}, c, scale);
    return {r.x, r.y, r.z};
}

inline ufbx_vec3 ConvertScale(const ufbx_vec3& v, const ModelImport::AxisConversion& c)
{
    const ModelImport::Vec3 r = ModelImport::ConvertScale(
        {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}, c);
    return {r.x, r.y, r.z};
}

inline ufbx_quat ConvertQuat(const ufbx_quat& q, const ModelImport::AxisConversion& c)
{
    const ModelImport::Quat r = ModelImport::ConvertQuat(
        {static_cast<float>(q.x), static_cast<float>(q.y),
         static_cast<float>(q.z), static_cast<float>(q.w)}, c);
    return {r.x, r.y, r.z, r.w};
}

inline void StoreMatrixColumnMajor(const ufbx_matrix& m,
                                   const ModelImport::AxisConversion& c,
                                   float s,
                                   float* out16)
{
    const float src[3][3] = {
        {static_cast<float>(m.m00), static_cast<float>(m.m01), static_cast<float>(m.m02)},
        {static_cast<float>(m.m10), static_cast<float>(m.m11), static_cast<float>(m.m12)},
        {static_cast<float>(m.m20), static_cast<float>(m.m21), static_cast<float>(m.m22)},
    };
    ModelImport::ConvertAffine(
        src,
        static_cast<float>(m.m03), static_cast<float>(m.m13), static_cast<float>(m.m23),
        c, s, out16);
}

} // namespace GameEngine::FbxImport
