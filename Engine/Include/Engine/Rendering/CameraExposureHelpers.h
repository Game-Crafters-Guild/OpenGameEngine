#pragma once

#include "Components/Rendering/Camera.h"
#include "Engine/Rendering/RenderServices.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer
{

// Copy a Camera component's exposure controls into the render-side CameraExposure the per-view
// post-process resolve consumes. Shared by every site that publishes a rendered camera (the editor
// Game tab and the Player) so the field mapping stays in one place.
inline ViewRegistry::CameraExposure ToCameraExposure(const Components::Camera& c)
{
    ViewRegistry::CameraExposure e{};
    e.Mode = static_cast<int32>(c.ExposureControl);
    e.Exposure = c.Exposure;
    e.ManualExposureEV = c.ManualExposureEV;
    e.ExposureCompensation = c.ExposureCompensation;
    e.Aperture = std::max(c.Aperture, Components::Camera::kApertureMin);
    e.ShutterTime = c.ShutterTime;
    e.Iso = c.Iso;
    e.AutoExposureMinEv = c.AutoExposureMinEv;
    e.AutoExposureMaxEv = c.AutoExposureMaxEv;
    e.AutoExposureSpeedUp = c.AutoExposureSpeedUp;
    e.AutoExposureSpeedDown = c.AutoExposureSpeedDown;
    e.FocusDistance = c.FocusDistance;
    e.ApertureBladeCount = static_cast<int32>(std::clamp(
        c.ApertureBladeCount, Components::Camera::kApertureBladeCountMin,
        Components::Camera::kApertureBladeCountMax));
    e.ApertureRoundness = std::clamp(c.ApertureRoundness, 0.0f, 1.0f);
    e.ApertureRotation = std::clamp(c.ApertureRotation, 0.0f, 360.0f);
    e.AnamorphicSqueeze = std::clamp(c.AnamorphicSqueeze, 1.0f, 4.0f);
    e.FocusDebugMode = c.FocusDebugMode != 0 ? 1 : 0;
    e.FocusDebugAlpha = std::clamp(c.FocusDebugAlpha, 0.0f, 1.0f);
    // Focal length from the vertical FOV against the camera's sensor height:
    // f = (sensorHeight/2) / tan(fovY/2). Orthographic views keep the default.
    e.SensorHeightMm = std::max(c.SensorHeightMm, 1.0f);
    if (c.Perspective && c.FovY > 0.1f && c.FovY < 179.0f)
        e.FocalLengthMm = Components::CameraFocalLengthMmFromVerticalFov(
            c.FovY, e.SensorHeightMm);
    return e;
}

} // namespace GameEngine::Engine::Renderer
