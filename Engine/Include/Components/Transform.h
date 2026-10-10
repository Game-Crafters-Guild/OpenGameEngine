#pragma once

#include "Types/Types.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Quaternion.h"

#include <cmath>
#include <cstring>

namespace GameEngine
{

using Mathematics::Vector3;
using Mathematics::Quaternion;

namespace Components
{

// Unified Transform component (4x4 matrix in **local-space** relative to Parent).
//
// Authoring/editor code should treat this as the local transform. World-space
// consumers (rendering, bounds, physics, etc.) are expected to read from
// WorldTransform, which is derived from Transform + Parent hierarchy by
// TransformHierarchySystem (see docs/TransformHierarchySystem.md).
// @ge-no-add  every entity always has a Transform; not user-addable via the menu.
struct Transform
{
    // Structure, not a feature: an entity's placement has no off state.
    static constexpr bool NotToggleable = true;

    // 4x4 transformation matrix stored in column-major order for GPU compatibility
    float32 matrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, // Column 0
        0.0f, 1.0f, 0.0f, 0.0f, // Column 1
        0.0f, 0.0f, 1.0f, 0.0f, // Column 2
        0.0f, 0.0f, 0.0f, 1.0f  // Column 3 (translation)
    };

    Transform() = default;

    // LH TRS: matrix * v == rot.Rotate(v) for a unit quaternion; a non-unit one rotates by
    // rot / |rot| without scaling. +Y yaw sends +Z toward +X.
    static Transform FromTRS(const Vector3& pos, const Quaternion& rot, const Vector3& scale)
    {
        return MakeTRS(pos, rot, scale, true);
    }

    // RH TRS: transpose of FromTRS. +Y yaw sends +Z toward -X.
    static Transform FromTRSRH(const Vector3& pos, const Quaternion& rot, const Vector3& scale)
    {
        return MakeTRS(pos, rot, scale, false);
    }

    // Extract components
    Vector3 GetPosition() const
    {
        return Vector3{matrix[12], matrix[13], matrix[14]};
    }

    // Quaternion whose Rotate() matches this matrix (FromTRS convention).
    //
    // GetRotation, GetScale and FromTRS evaluate in double and round once to float. Evaluated in
    // float, decomposing a composed matrix lands several ulps away from the inputs and a scene
    // save/load cycle random-walks the scale; in double the cycle jitters within a bounded
    // distance of its first save instead. The SceneIO round-trip test poses stay within that test's
    // bound (scale 2 ulp, rotation 2^-23 per component); this is not guaranteed for every pose. A
    // float matrix cannot carry the low bits of a small quaternion component, so bit-exact
    // recovery is not reachable while Transform stores a matrix.
    Quaternion GetRotation() const
    {
        // Extract rotation by removing scale from the column-major matrix.
        // First compute per-axis scales from column lengths.
        const float64 sx = ColumnLength(0);
        const float64 sy = ColumnLength(4);
        const float64 sz = ColumnLength(8);

        if (sx <= 0.0 || sy <= 0.0 || sz <= 0.0)
        {
            return Quaternion{}; // Degenerate, return identity
        }

        // Build a pure rotation 3x3 (rows) from the scaled columns
        const float64 r00 = matrix[0] / sx;
        const float64 r10 = matrix[1] / sx;
        const float64 r20 = matrix[2] / sx;

        const float64 r01 = matrix[4] / sy;
        const float64 r11 = matrix[5] / sy;
        const float64 r21 = matrix[6] / sy;

        const float64 r02 = matrix[8] / sz;
        const float64 r12 = matrix[9] / sz;
        const float64 r22 = matrix[10] / sz;

        const float64 trace = r00 + r11 + r22;
        float64 qw, qx, qy, qz;

        if (trace > 0.0)
        {
            const float64 s = std::sqrt(trace + 1.0) * 2.0;
            qw = 0.25 * s;
            qx = (r21 - r12) / s;
            qy = (r02 - r20) / s;
            qz = (r10 - r01) / s;
        }
        else if (r00 > r11 && r00 > r22)
        {
            const float64 s = std::sqrt(1.0 + r00 - r11 - r22) * 2.0;
            qw = (r21 - r12) / s;
            qx = 0.25 * s;
            qy = (r01 + r10) / s;
            qz = (r02 + r20) / s;
        }
        else if (r11 > r22)
        {
            const float64 s = std::sqrt(1.0 + r11 - r00 - r22) * 2.0;
            qw = (r02 - r20) / s;
            qx = (r01 + r10) / s;
            qy = 0.25 * s;
            qz = (r12 + r21) / s;
        }
        else
        {
            const float64 s = std::sqrt(1.0 + r22 - r00 - r11) * 2.0;
            qw = (r10 - r01) / s;
            qx = (r02 + r20) / s;
            qy = (r12 + r21) / s;
            qz = 0.25 * s;
        }

        return Quaternion(static_cast<float32>(qw), static_cast<float32>(qx), static_cast<float32>(qy),
                          static_cast<float32>(qz));
    }

    Vector3 GetScale() const
    {
        return Vector3{static_cast<float32>(ColumnLength(0)), static_cast<float32>(ColumnLength(4)),
                       static_cast<float32>(ColumnLength(8))};
    }

    // Matrix operations
    void SetIdentity()
    {
        std::memset(matrix, 0, sizeof(matrix));
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    }

    void Translate(float32 x, float32 y, float32 z)
    {
        matrix[12] += x;
        matrix[13] += y;
        matrix[14] += z;
    }

    void Rotate(float32 x, float32 y, float32 z, float32 w)
    {
        *this = FromTRS(GetPosition(), Quaternion(w, x, y, z), GetScale());
    }

    void SetScale(float32 x, float32 y, float32 z)
    {
        // Apply scale to the matrix (replace, don't multiply)
        matrix[0] = x;
        matrix[5] = y;
        matrix[10] = z;
    }

  private:
    // Length of the 3-component matrix column starting at `first`, in double (see GetRotation).
    float64 ColumnLength(int first) const
    {
        const float64 a = matrix[first];
        const float64 b = matrix[first + 1];
        const float64 c = matrix[first + 2];
        return std::sqrt(a * a + b * b + c * c);
    }

    static Transform MakeTRS(const Vector3& pos, const Quaternion& rot, const Vector3& scale,
                             bool leftHanded)
    {
        Transform transform;
        transform.SetIdentity();

        const auto& q = rot.GetGLM();
        const float64 x = q.x;
        const float64 y = q.y;
        const float64 z = q.z;
        const float64 w = q.w;

        // Rotates by q / |q|: scaling the products by 2 / |q|^2 keeps a non-unit quaternion's norm
        // out of the scale columns. Rounding a normalised quaternion to float before composing
        // would put a 1 +- 2^-24 factor there instead, which a save/load cycle accumulates. A
        // zero quaternion composes as the identity rotation.
        const float64 normSquared = x * x + y * y + z * z + w * w;
        const float64 s = normSquared > 0.0 ? 2.0 / normSquared : 0.0;
        const float64 x2 = x * s;
        const float64 y2 = y * s;
        const float64 z2 = z * s;

        const float64 xx = x * x2;
        const float64 xy = x * y2;
        const float64 xz = x * z2;
        const float64 yy = y * y2;
        const float64 yz = y * z2;
        const float64 zz = z * z2;
        const float64 wx = w * x2;
        const float64 wy = w * y2;
        const float64 wz = w * z2;

        // Hamilton (q*v) 3x3. RH is that matrix transposed.
        const float64 r00 = 1.0 - (yy + zz);
        const float64 r11 = 1.0 - (xx + zz);
        const float64 r22 = 1.0 - (xx + yy);
        const float64 r01 = leftHanded ? (xy - wz) : (xy + wz);
        const float64 r02 = leftHanded ? (xz + wy) : (xz - wy);
        const float64 r10 = leftHanded ? (xy + wz) : (xy - wz);
        const float64 r12 = leftHanded ? (yz - wx) : (yz + wx);
        const float64 r20 = leftHanded ? (xz - wy) : (xz + wy);
        const float64 r21 = leftHanded ? (yz + wx) : (yz - wx);

        const float64 sx = scale.x;
        const float64 sy = scale.y;
        const float64 sz = scale.z;

        transform.matrix[0] = static_cast<float32>(r00 * sx);
        transform.matrix[1] = static_cast<float32>(r10 * sx);
        transform.matrix[2] = static_cast<float32>(r20 * sx);

        transform.matrix[4] = static_cast<float32>(r01 * sy);
        transform.matrix[5] = static_cast<float32>(r11 * sy);
        transform.matrix[6] = static_cast<float32>(r21 * sy);

        transform.matrix[8] = static_cast<float32>(r02 * sz);
        transform.matrix[9] = static_cast<float32>(r12 * sz);
        transform.matrix[10] = static_cast<float32>(r22 * sz);

        transform.matrix[12] = pos.x;
        transform.matrix[13] = pos.y;
        transform.matrix[14] = pos.z;

        return transform;
    }
};

// World-space transform derived from local Transform + Parent by
// TransformHierarchySystem. Layout matches Transform::matrix
// (4x4 column-major) so it can be consumed by rendering and bounds systems.
//
// CANONICAL WRITER: TransformHierarchySystem. Other systems should mutate
// the local Components::Transform and let the hierarchy propagate. This is
// the recommended path because the hierarchy auto-bumps Version on actual
// content change (memcmp-gated).
//
// DIRECT WRITERS (rare; today: PhysicsWritebackSystem, NavigationMovementSystem,
// GamepadCameraControllerSystem, editor tools) must call
// Components::BumpWorldTransform(world, entity, wt)
// (Components/TransformDirtyFeed.h) after mutating matrix bytes — it bumps
// Version AND emits the entity into the world's dirty feed in one step, so
// the two cannot fork. A bare `++wt.Version;` bump keeps Version-polling
// consumers working but silently starves feed consumers (extraction's
// patch lane). Audit any new direct writer
// against this contract.
//
// A THIRD channel — the per-(chunk, column) write grant that
// Changed<WorldTransform> consumers filter on (the terrain modifier dirty
// scan) — is issued for you whenever you reach the component through a
// Write<WorldTransform> query visit or GetComponentForWrite. It is NOT issued
// by writing through a pointer cached outside such a call; that writer must
// stamp explicitly (World::StampComponentWriteBatch). The transform
// hierarchy's node cache is the only writer of that shape today.
//
// VERSION SEMANTICS: monotonic uint32, may wrap at ~4 billion frames (>2 years
// at 60 FPS). Consumers compare cached vs current value; equality means "no
// change since last observation." Wrap is not handled in v1 — if it ever
// matters, switch to uint64 or rebuild detection logic.
// [DoNotSerialize] — derived world matrix, recomputed from Transform each frame; never authored.
struct WorldTransform
{
    // Derived from Transform and the Parent chain: it has no off state of its own.
    static constexpr bool NotToggleable = true;

    float32 matrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    uint32 Version = 0u;
};

// Helper utilities for quaternion  Euler angle conversions (degrees, XYZ = roll, pitch, yaw).
inline Quaternion QuaternionFromEulerXYZDegrees(float32 xDeg, float32 yDeg, float32 zDeg)
{
    constexpr float32 kDegToRad = 3.14159265358979323846f / 180.0f;

    float32 roll = xDeg * kDegToRad;
    float32 pitch = yDeg * kDegToRad;
    float32 yaw = zDeg * kDegToRad;

    float32 cy = std::cos(yaw * 0.5f);
    float32 sy = std::sin(yaw * 0.5f);
    float32 cp = std::cos(pitch * 0.5f);
    float32 sp = std::sin(pitch * 0.5f);
    float32 cr = std::cos(roll * 0.5f);
    float32 sr = std::sin(roll * 0.5f);

    float32 w = cr * cp * cy + sr * sp * sy;
    float32 x = sr * cp * cy - cr * sp * sy;
    float32 y = cr * sp * cy + sr * cp * sy;
    float32 z = cr * cp * sy - sr * sp * cy;
    return Quaternion(w, x, y, z);
}

inline void EulerXYZDegreesFromQuaternion(const Quaternion& q, float32& xDeg, float32& yDeg, float32& zDeg)
{
    constexpr float32 kRadToDeg = 180.0f / 3.14159265358979323846f;

    const auto& g = q.GetGLM();

    // roll (X-axis rotation)
    float32 sinr_cosp = 2.0f * (g.w * g.x + g.y * g.z);
    float32 cosr_cosp = 1.0f - 2.0f * (g.x * g.x + g.y * g.y);
    float32 roll = std::atan2(sinr_cosp, cosr_cosp);

    // pitch (Y-axis rotation)
    float32 sinp = 2.0f * (g.w * g.y - g.z * g.x);
    float32 pitch;
    if (std::fabs(sinp) >= 1.0f)
    {
        pitch = (sinp > 0.0f ? 1.0f : -1.0f) * (3.14159265358979323846f * 0.5f);
    }
    else
    {
        pitch = std::asin(sinp);
    }

    // yaw (Z-axis rotation)
    float32 siny_cosp = 2.0f * (g.w * g.z + g.x * g.y);
    float32 cosy_cosp = 1.0f - 2.0f * (g.y * g.y + g.z * g.z);
    float32 yaw = std::atan2(siny_cosp, cosy_cosp);

    xDeg = roll * kRadToDeg;
    yDeg = pitch * kRadToDeg;
    zDeg = yaw * kRadToDeg;
}

// Convenience alias for clarity at callsites where we want to emphasize
// that Transform is local-space.
using LocalTransform = Transform;

} // namespace Components
} // namespace GameEngine
