#include "Rendering/CameraDerivation.h"

#include <cmath>
#include <cstring>

namespace GameEngine
{
namespace Rendering
{

CameraDerivedData DeriveCameraData(const CameraData& source)
{
    CameraDerivedData out{};

    // CameraData stores column-major matrices compatible with Matrix4x4's
    // internal layout, so a raw memcpy lands the matrices in canonical form.
    std::memcpy(out.ViewMatrix.Data(), source.view, sizeof(source.view));
    std::memcpy(out.ProjMatrix.Data(), source.proj, sizeof(source.proj));
    out.ViewProjMatrix = out.ProjMatrix * out.ViewMatrix;

    // Camera position = inverse(view) * origin.
    const Matrix4x4 worldFromCamera = GameEngine::Mathematics::Inverse(out.ViewMatrix);
    out.Position = worldFromCamera.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));

    // Camera forward = inverse(view) * +Z, then normalise. This engine is
    // LEFT-HANDED with +Z forward (MakeLookAtLH), so camera-space forward is
    // +Z — the previous -Z here was the right-handed convention and pointed
    // this vector AWAY from the view, silently negating every consumer's
    // view-depth dot (caught by the sorted transparent drain's per-run depth
    // log: dot(centre - camPos, Forward) came out negative for content dead
    // ahead, inverting the blend sort front-to-back).
    const Vector4 forward4 = worldFromCamera.Transform(Vector4(0.0f, 0.0f, 1.0f, 0.0f));
    out.Forward = Vector3(forward4.x, forward4.y, forward4.z).Normalize();

    // Reverse-Z near/far recovery from the left-handed [1,0] reverse-Z
    // perspective matrix produced by MakePerspectiveLH_ZO_ReverseZ.
    // Column-major glm indexing (proj[col][row]):
    //   m22 = proj[2][2] = -near / (far - near)
    //   m32 = proj[3][2] = (near * far) / (far - near)
    // =>
    //   near = m32 / (1 - m22)
    //   far  = -m32 / m22
    // The defensive checks below leave the default near/far if the matrix
    // doesn't look like the expected reverse-Z perspective (e.g. ortho).
    const float m22 = out.ProjMatrix[2][2];
    const float m32 = out.ProjMatrix[3][2];
    if (std::fabs(m22) > 1e-6f && std::fabs(1.0f - m22) > 1e-6f)
    {
        const float nearPlane = m32 / (1.0f - m22);
        const float farPlane = -m32 / m22;
        if (nearPlane > 0.0f && farPlane > nearPlane && std::isfinite(farPlane))
        {
            out.NearPlane = nearPlane;
            out.FarPlane = farPlane;
        }
    }

    // Orthographic detection + screen-space slope recovery.
    //   perspective: proj[3][3] = 0, proj[1][1] = 1 / tan(fovY/2)
    //     -> ScreenScale = tan(fovY/2)
    //   ortho:       proj[3][3] = 1, proj[1][1] = 2 / (top - bottom)
    //     -> ScreenScale = orthoHalfHeight
    out.IsOrthographic = std::fabs(out.ProjMatrix[3][3] - 1.0f) < 1e-4f;
    const float m11 = out.ProjMatrix[1][1];
    if (std::fabs(m11) > 1e-6f)
        out.ScreenScale = 1.0f / m11;

    return out;
}

} // namespace Rendering
} // namespace GameEngine
