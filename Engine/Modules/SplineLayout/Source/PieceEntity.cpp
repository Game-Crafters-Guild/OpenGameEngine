#include "SplineLayout/PieceEntity.h"

#include "Components/Transform.h"
#include "Mathematics/MatrixOps.h"
#include "SplineLayout/PieceBasis.h"
#include "SplineLayout/TileLayout.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace GameEngine::SplineLayout
{

Mathematics::Matrix4x4 InvertPlacerWorld(const float32* placerWorldMatrix)
{
    Mathematics::Matrix4x4 placer;
    std::memcpy(placer.Data(), placerWorldMatrix, sizeof(float32) * 16u);

    const Mathematics::Matrix4x4 inverse = Mathematics::Inverse(placer);
    const float32* values = inverse.Data();
    for (int i = 0; i < 16; ++i)
    {
        if (!std::isfinite(values[i]))
            return Mathematics::Matrix4x4::Identity();
    }
    return inverse;
}

void WriteParentLocalPose(Components::Transform& out,
                          const Mathematics::Matrix4x4& invPlacerWorld,
                          const TilePose& pose,
                          PieceAxis axis,
                          float32 lengthScale)
{
    // Columns 0/1/2 are where the piece's own local +X/+Y/+Z point, so the
    // basis and the stretch both come from the axis rather than from a fixed
    // reading of Forward.
    const PieceBasis basis = MakePieceBasis(pose, axis);
    const PieceAxisScale scale = MakePieceAxisScale(axis, lengthScale);
    const Mathematics::Vector3 localX = basis.LocalX * scale.X;
    const Mathematics::Vector3 localZ = basis.LocalZ * scale.Z;

    Mathematics::Matrix4x4 world;
    float32* m = world.Data();
    m[0] = localX.x;         m[1] = localX.y;         m[2] = localX.z;         m[3] = 0.0f;
    m[4] = basis.LocalY.x;   m[5] = basis.LocalY.y;   m[6] = basis.LocalY.z;   m[7] = 0.0f;
    m[8] = localZ.x;         m[9] = localZ.y;         m[10] = localZ.z;        m[11] = 0.0f;
    m[12] = pose.Position.x; m[13] = pose.Position.y; m[14] = pose.Position.z; m[15] = 1.0f;

    const Mathematics::Matrix4x4 local = invPlacerWorld * world;
    std::memcpy(out.matrix, local.Data(), sizeof(out.matrix));
}

Components::Name MakePieceLabel(const char* role, uint32 index)
{
    Components::Name name{};
    std::snprintf(name.value, sizeof(name.value), "%s %u", role, index);
    return name;
}

} // namespace GameEngine::SplineLayout
