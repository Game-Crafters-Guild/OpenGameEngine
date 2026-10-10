#pragma once

#include <cmath>

// Double-precision math for the Scene View camera rig (orbit / pan / dolly /
// fly / view build). The controller's authoritative position and orbit pivot
// are doubles; every step that combines a large absolute position with a small
// offset runs here in double and rounds to fp32 exactly once, at the hand-off
// to rendering. This keeps the camera smooth at planetary coordinates
// (|pos| ~5e4 today, 6.4e6 on the roadmap), where fp32 quantizes positions to
// ~4 mm / ~0.5 m and — far worse — the old MakeLookAtLH(eye, eye + look)
// round-trip quantized the view DIRECTION to ULP(|eye|)-sized angular steps.
//
// Conventions match SceneViewController: left-handed, +Y world up, yaw/pitch
// in degrees with yaw 90 => +Z forward; view matrix layout is identical to
// Mathematics::MakeLookAtLH (glm::lookAtLH, column-major).
namespace GameEngine::Editor::CameraRig
{

struct Vec3d
{
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
};

inline constexpr double kDegToRad = 0.017453292519943295769; // pi / 180

// Orthographic wheel levels: whole rendered pixels per world unit when
// magnifying, reciprocal whole-number reductions when zoomed farther out.
inline double StepOrthographicPixelZoom(double height, double rasterHeight, int direction)
{
    if (!(height > 0) || !(rasterHeight > 0) || direction == 0 ||
        !std::isfinite(height) || !std::isfinite(rasterHeight)) return height;
    const double scale = rasterHeight / height;
    constexpr double epsilon = 1e-5;
    double next;
    if (direction > 0)
        next = scale < 1 - epsilon ? 1.0 / std::fmax(1.0, std::ceil(1.0 / scale - epsilon) - 1.0)
                                   : std::floor(scale + epsilon) + 1.0;
    else
        next = scale > 1 + epsilon ? std::fmax(1.0, std::ceil(scale - epsilon) - 1.0)
                                   : 1.0 / (std::floor(1.0 / scale + epsilon) + 1.0);
    return rasterHeight / next;
}

// Unit look direction for yaw/pitch (degrees).
inline Vec3d LookDirection(double yawDeg, double pitchDeg)
{
    const double yaw = yawDeg * kDegToRad;
    const double pit = pitchDeg * kDegToRad;
    const double cp = std::cos(pit);
    Vec3d f{cp * std::cos(yaw), std::sin(pit), cp * std::sin(yaw)};
    const double len = std::sqrt(f.X * f.X + f.Y * f.Y + f.Z * f.Z);
    if (len > 0.0)
    {
        f.X /= len;
        f.Y /= len;
        f.Z /= len;
    }
    return f;
}

// Camera position on the orbit sphere: pivot - look * distance.
inline Vec3d OrbitCameraPosition(const Vec3d& pivot, double yawDeg, double pitchDeg, double distance)
{
    const Vec3d f = LookDirection(yawDeg, pitchDeg);
    return Vec3d{pivot.X - f.X * distance, pivot.Y - f.Y * distance, pivot.Z - f.Z * distance};
}

// Pivot ahead of the camera: camera + look * distance.
inline Vec3d OrbitPivotFromCamera(const Vec3d& camPos, double yawDeg, double pitchDeg, double distance)
{
    const Vec3d f = LookDirection(yawDeg, pitchDeg);
    return Vec3d{camPos.X + f.X * distance, camPos.Y + f.Y * distance, camPos.Z + f.Z * distance};
}

// Pan basis used by camera drag-pan: right is the horizontal (XZ) vector
// perpendicular to the look direction, up completes it via cross(right, look).
// Mirrors the historical fp32 pan basis exactly (including the straight-down
// fallback) so pan feel is unchanged.
inline void PanBasis(double yawDeg, double pitchDeg, Vec3d& outRight, Vec3d& outUp)
{
    const Vec3d look = LookDirection(yawDeg, pitchDeg);

    double flatX = look.X;
    double flatZ = look.Z;
    const double lenFlatSq = flatX * flatX + flatZ * flatZ;
    if (lenFlatSq > 1e-12)
    {
        const double invLen = 1.0 / std::sqrt(lenFlatSq);
        flatX *= invLen;
        flatZ *= invLen;
    }
    else
    {
        flatX = 0.0;
        flatZ = 1.0;
    }
    outRight = Vec3d{flatZ, 0.0, -flatX};

    // up = cross(right, look), normalized.
    Vec3d up{outRight.Y * look.Z - outRight.Z * look.Y,
             outRight.Z * look.X - outRight.X * look.Z,
             outRight.X * look.Y - outRight.Y * look.X};
    const double upLen = std::sqrt(up.X * up.X + up.Y * up.Y + up.Z * up.Z);
    if (upLen > 0.0)
    {
        up.X /= upLen;
        up.Y /= upLen;
        up.Z /= upLen;
    }
    outUp = up;
}

// Column-major fp32 LH view matrix with the same layout as
// Mathematics::MakeLookAtLH(eye, eye + look, worldUp), but the look direction
// is taken exactly from yaw/pitch and every product is accumulated in double.
// The old form re-derived the direction as normalize((eye + look) - eye) in
// fp32, which quantizes the ROTATION BASIS to ULP(|eye|) — ~0.22 deg steps at
// |eye| = 5e4 (~4 px of jitter per mouse-move at 60 deg FOV). The single fp32
// rounding here happens on store, so basis and translation are smooth
// functions of the pose at any coordinate magnitude.
inline void BuildViewMatrixLH(const Vec3d& eye, double yawDeg, double pitchDeg, float out[16])
{
    const Vec3d f = LookDirection(yawDeg, pitchDeg);

    // s = normalize(cross(worldUp, f)) with worldUp = (0,1,0) => (f.Z, 0, -f.X).
    double sX = f.Z;
    double sZ = -f.X;
    const double sLen = std::sqrt(sX * sX + sZ * sZ);
    if (sLen > 1e-12)
    {
        sX /= sLen;
        sZ /= sLen;
    }
    else
    {
        // Looking straight up/down (pitch is clamped before reaching here, but
        // stay finite): pick +X as the side vector.
        sX = 1.0;
        sZ = 0.0;
    }

    // u = cross(f, s), s.Y == 0.
    const double uX = f.Y * sZ;
    const double uY = f.Z * sX - f.X * sZ;
    const double uZ = -f.Y * sX;

    out[0] = static_cast<float>(sX);
    out[1] = static_cast<float>(uX);
    out[2] = static_cast<float>(f.X);
    out[3] = 0.0f;
    out[4] = 0.0f; // s.Y
    out[5] = static_cast<float>(uY);
    out[6] = static_cast<float>(f.Y);
    out[7] = 0.0f;
    out[8] = static_cast<float>(sZ);
    out[9] = static_cast<float>(uZ);
    out[10] = static_cast<float>(f.Z);
    out[11] = 0.0f;
    out[12] = static_cast<float>(-(sX * eye.X + sZ * eye.Z));
    out[13] = static_cast<float>(-(uX * eye.X + uY * eye.Y + uZ * eye.Z));
    out[14] = static_cast<float>(-(f.X * eye.X + f.Y * eye.Y + f.Z * eye.Z));
    out[15] = 1.0f;
}

} // namespace GameEngine::Editor::CameraRig
