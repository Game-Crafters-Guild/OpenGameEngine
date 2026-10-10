#pragma once

#include "Components/Rendering/Camera.h"
#include "ECS/Entity.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/CameraTypes.h"

#include <optional>
#include "Rendering/CameraDerivation.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::CameraData;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine {
namespace ECS { class World; }
} // namespace GameEngine

namespace GameEngine::Engine::Renderer {

/// Full camera state with computed matrices, suitable for rendering.
/// Distinct from Components::Camera (ECS component holding just fovY/nearZ/farZ parameters).
struct Camera
{
    Mathematics::Matrix4x4 worldTransform;
    Components::Camera params; // projection, clipping, culling, and post-process params
    ECS::EntityHandle entity;

    /// Compute view matrix (inverse of worldTransform).
    Mathematics::Matrix4x4 ComputeViewMatrix() const;

    /// Compute projection matrix for a given viewport aspect ratio.
    /// When orthoHeightOverride is set and the camera is orthographic, that vertical
    /// world-space extent is used instead of params.OrthographicSize (pixel-perfect mode).
    Mathematics::Matrix4x4 ComputeProjectionMatrix(float aspectRatio,
                                                   std::optional<float> orthoHeightOverride = std::nullopt) const;

    /// Compute combined view-projection matrix.
    Mathematics::Matrix4x4 ComputeViewProjectionMatrix(float aspectRatio,
                                                       std::optional<float> orthoHeightOverride = std::nullopt) const;

    /// Fill a CameraData struct ready for RenderServices::SetCameraData().
    Rendering::CameraData ToCameraData(float aspectRatio,
                                       std::optional<float> orthoHeightOverride = std::nullopt) const;
};

/// Find the active camera in the ECS world.
///
/// Currently returns the first entity with a Camera component and WorldTransform
/// (with Transform as fallback). Returns std::nullopt if no camera entity exists.
///
/// Future: support priority/tag-based camera selection (e.g., MainCamera component).
std::optional<Camera> FindActiveCamera(ECS::World& world);

} // namespace GameEngine::Engine::Renderer
