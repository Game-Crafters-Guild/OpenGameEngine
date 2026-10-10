#pragma once

#include "Components/Rendering/Camera.h"
#include "Rendering/CameraTypes.h"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace GameEngine::Editor
{

// The projection half of the `get_camera` debug response.
//
// A pose alone cannot convert screen pixels to world meters — that needs the
// field of view, and the FOV every projection in this engine is built from is
// the VERTICAL one (MakePerspectiveLH_ZO_ReverseZ takes fovYRadians). A caller
// that reads a single "fov" number as horizontal measures a scene that is
// aspect-ratio wide of the truth, so this reports the vertical angle under a
// name that says so, and reports the horizontal angle itself rather than
// leaving the caller to derive it.
//
// Projection values come from the selected view's declared matrix, never from
// settings that may have been clamped or overridden on the way in. Declaration
// does not prove that the GPU completed or presented the frame.
//
// The viewport pixels are provenance for aspect, not its source. Aspect presets
// and letterboxing can make the matrix aspect differ from the panel ratio.
// Orthographic projections report orthoHeight and omit both angle fields.
nlohmann::json DescribeCameraProjection(const float proj[16],
                                        std::uint32_t viewportWidthPx,
                                        std::uint32_t viewportHeightPx);

// Game View uses its declared camera matrix, including roll, rather than the
// Scene View orbit pose. The world transform is a column-major 4x4 matrix.
nlohmann::json DescribeGameViewCamera(const Rendering::CameraData& camera,
                                      Components::ExposureMode mode,
                                      std::uint32_t viewportWidthPx,
                                      std::uint32_t viewportHeightPx);

// A completed GPU meter sample has its own frame provenance, independent of
// the latest camera declaration. Invalid scales have no reportable sample.
nlohmann::json DescribeMeteredExposure(float linearScale, std::uint64_t frameIndex);

} // namespace GameEngine::Editor
