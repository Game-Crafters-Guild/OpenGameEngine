#include "DebugServer/CameraProjectionReport.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/Exposure.h"
#include "Mathematics/MatrixOps.h"

#include <cmath>

namespace GameEngine::Editor
{
namespace
{

constexpr float kRadToDeg = 57.295779513082320876f;

// Column-major (glm) 4x4: index = column * 4 + row.
constexpr int kM00 = 0;  // perspective: cot(fovY/2) / aspect   ortho: 2/(right-left)
constexpr int kM11 = 5;  // perspective: cot(fovY/2)            ortho: 2/(top-bottom)

// Both builders produce strictly positive diagonal scales: cot(fovY/2) for a
// FOV in (0, pi), and 2/(right-left), 2/(top-bottom) for a non-inverted ortho
// box. A negative one is a matrix this code cannot describe, and the arctangent
// would answer with a negative angle — a number, wrong, and indistinguishable
// from a real reading. Absence, not a guess.
bool UsableScale(float v)
{
    return std::isfinite(v) && v > 1e-6f;
}

// 2 * atan(1 / cot(half)) in degrees. Both angles fall straight out of the
// matrix this way: the vertical from m11, the horizontal from m00 — deriving
// the horizontal from the vertical and the aspect instead would multiply two
// recovered quantities and their error.
float FullAngleDegFromCotHalf(float cotHalfAngle)
{
    return 2.0f * std::atan(1.0f / cotHalfAngle) * kRadToDeg;
}

} // namespace

nlohmann::json DescribeCameraProjection(const float proj[16],
                                        std::uint32_t viewportWidthPx,
                                        std::uint32_t viewportHeightPx)
{
    using nlohmann::json;

    if (proj == nullptr || !UsableScale(proj[kM00]) || !UsableScale(proj[kM11]))
        return json(nullptr);

    // Both projections scale X by width and Y by height, so the ratio of the
    // two diagonal terms is the aspect in both cases.
    const float aspect = proj[kM11] / proj[kM00];

    json out{
        {"aspect", aspect},
        {"viewportWidthPx", viewportWidthPx},
        {"viewportHeightPx", viewportHeightPx}
    };

    if (Engine::Renderer::IsOrthographicProjectionLH_ZO(proj))
    {
        out["type"] = "orthographic";
        out["orthoHeight"] = 2.0f / proj[kM11];
        return out;
    }

    out["type"] = "perspective";
    out["fovVerticalDeg"] = FullAngleDegFromCotHalf(proj[kM11]);
    out["fovHorizontalDeg"] = FullAngleDegFromCotHalf(proj[kM00]);
    return out;
}

nlohmann::json DescribeGameViewCamera(const Rendering::CameraData& camera,
                                      Components::ExposureMode mode,
                                      std::uint32_t viewportWidthPx,
                                      std::uint32_t viewportHeightPx)
{
    const auto world = Mathematics::Inverse(Mathematics::Matrix4x4::FromColumnMajor(camera.view));
    auto transform = nlohmann::json::array();
    for (int index = 0; index < 16; ++index)
        transform.push_back(world.Data()[index]);
    const char* modeName = "unknown";
    switch (mode)
    {
    case Components::ExposureMode::Fixed: modeName = "fixed"; break;
    case Components::ExposureMode::Manual: modeName = "manual"; break;
    case Components::ExposureMode::Physical: modeName = "physical"; break;
    case Components::ExposureMode::Auto: modeName = "auto"; break;
    }
    return {{"position", {camera.cameraPos[0], camera.cameraPos[1], camera.cameraPos[2]}},
            {"worldTransform", std::move(transform)},
            {"projection", DescribeCameraProjection(camera.proj, viewportWidthPx, viewportHeightPx)},
            {"exposure", {{"mode", modeName}, {"metered", nullptr}}}};
}

nlohmann::json DescribeMeteredExposure(float linearScale, std::uint64_t frameIndex)
{
    if (!std::isfinite(linearScale) || linearScale <= 0.0f)
        return nullptr;
    return {{"linearScale", linearScale},
            {"ev100", Rendering::LinearExposureToEv(linearScale)},
            {"frameIndex", frameIndex}};
}

} // namespace GameEngine::Editor
