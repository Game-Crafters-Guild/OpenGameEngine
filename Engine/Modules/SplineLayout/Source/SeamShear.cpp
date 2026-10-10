#include "SplineLayout/SeamShear.h"

#include "Mathematics/VectorOps.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineLayout
{
namespace
{

using V3 = Mathematics::Vector3;

} // namespace

float32 SignedGroundYaw(const V3& a, const V3& b)
{
    const float32 aLenSq = a.x * a.x + a.z * a.z;
    const float32 bLenSq = b.x * b.x + b.z * b.z;
    if (!std::isfinite(aLenSq) || !std::isfinite(bLenSq) ||
        aLenSq < 1.0e-8f || bLenSq < 1.0e-8f)
        return 0.0f;
    const float32 cross = a.z * b.x - a.x * b.z; // +Y component of Cross(a, b)
    const float32 dot = a.x * b.x + a.z * b.z;
    return std::atan2(cross, dot);
}

std::vector<float32> ComputeSeamShearFactors(const std::vector<V3>& forwards, float32 capDegrees)
{
    std::vector<float32> shear(forwards.size(), 0.0f);
    if (capDegrees <= 0.0f || forwards.size() < 2)
        return shear;

    const float32 capShear = std::tan(capDegrees * (Mathematics::Pi / 180.0f));

    std::vector<float32> jointYaw(forwards.size() - 1);
    for (size_t j = 0; j + 1 < forwards.size(); ++j)
        jointYaw[j] = SignedGroundYaw(forwards[j], forwards[j + 1]);

    for (size_t i = 0; i < forwards.size(); ++i)
    {
        // A right turn (positive yaw) opens its wedge on the left: the earlier
        // tile shears its left side forward (negative factor), the later tile
        // shears it backward (positive factor); at an isolated joint both end
        // faces then meet exactly on the joint bisector. Each demand is
        // clamped before summing so a reversal joint cannot poison the total.
        const float32 fromStart =
            (i > 0) ? std::clamp(std::tan(0.5f * jointYaw[i - 1]), -capShear, capShear) : 0.0f;
        const float32 fromEnd =
            (i + 1 < forwards.size())
                ? std::clamp(-std::tan(0.5f * jointYaw[i]), -capShear, capShear)
                : 0.0f;
        shear[i] = std::clamp(fromStart + fromEnd, -capShear, capShear);
    }
    return shear;
}

void ApplySeamShear(std::vector<TilePose>& poses, const std::vector<float32>& shearFactors,
                    const Mathematics::Vector3& meshBoundsCenter, PieceAxis axis)
{
    // The factors above were derived from the WORLD forwards, so what they name
    // is a world direction: the across-travel edge lands on Right + s * Forward
    // for every piece, however that piece's own axes reach it.
    const float32 centerOnRight = PieceCenterOnPoseRight(meshBoundsCenter, axis);
    const size_t count = std::min(poses.size(), shearFactors.size());
    for (size_t i = 0; i < count; ++i)
    {
        const float32 s = shearFactors[i];
        if (s == 0.0f)
            continue;
        // The layout centred the footprint by writing -Right * centerOnRight
        // into Position. Right is about to shear, so that term shears with it.
        poses[i].Position = poses[i].Position - poses[i].Forward * (s * centerOnRight);
        poses[i].Right = poses[i].Right + poses[i].Forward * s;
    }
}

} // namespace GameEngine::SplineLayout
