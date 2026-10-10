#include "SceneView/SplineStrokeCapture.h"

namespace GameEngine::Editor::SceneTools
{

namespace
{
// A release this close to the last sample repeats it.
constexpr float kRepeatedSampleSquared = 1.0e-6f;
} // namespace

bool CaptureSplineStroke(std::vector<Mathematics::Vector3>& stroke, const Mathematics::Vector3& point,
                         float minSpacing)
{
    if (!stroke.empty() && (point - stroke.back()).LengthSquared() < minSpacing * minSpacing)
        return false;
    stroke.push_back(point);
    return true;
}

void EndSplineStroke(std::vector<Mathematics::Vector3>& stroke, const Mathematics::Vector3& release)
{
    if (!stroke.empty() && (release - stroke.back()).LengthSquared() > kRepeatedSampleSquared)
        stroke.push_back(release);
}

} // namespace GameEngine::Editor::SceneTools
