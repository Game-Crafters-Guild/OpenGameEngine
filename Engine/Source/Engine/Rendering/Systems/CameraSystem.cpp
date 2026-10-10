#include "ECSModules/Rendering/Systems/CameraSystem.h"

#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/PlanetCameraFraming.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine { namespace Engine::Renderer {
using namespace ::GameEngine::Rendering;

using GameEngine::Rendering::Matrix4x4;

void CameraSystem::Update(ECS::World& world, float32 /*deltaTime*/) {
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    // Locate the first ECS camera with a world transform. In the future this
    // can be extended with tags/priorities; for now we treat the first match
    // as the primary "game" camera.
    GameEngine::Components::WorldTransform worldTransform{};
    GameEngine::Components::Camera cameraComp{};
    bool foundCamera = false;

    {
        auto q = world.Query<
            GameEngine::ECS::Read<GameEngine::Components::WorldTransform>,
            GameEngine::ECS::Read<GameEngine::Components::Camera>>();

        q.Each([&](GameEngine::ECS::EntityHandle /*e*/,
                   const GameEngine::Components::WorldTransform& xf,
                   const GameEngine::Components::Camera& cam) {
            if (foundCamera)
                return;
            worldTransform = xf;
            cameraComp = cam;
            foundCamera = true;
        });
    }

    if (!foundCamera) {
        return; // No ECS camera this frame; leave existing camera/view as-is.
    }

    // An enabled spherical (planet) terrain auto-extends the far plane to frame the globe,
    // never below the user's setting — the same never-shrink derivation the editor Scene
    // View applies, shared so the game / Player camera doesn't clip a planet from orbit.
    // Transient: reverts once the planet is disabled or deleted.
    cameraComp.FarZ = ExpandFarClipForSphericalTerrain(world, cameraComp.FarZ);

    uint32 viewportW = 0;
    uint32 viewportH = 0;
    if (auto* device = rs->GetDevice())
    {
        device->GetSwapchainSize(viewportW, viewportH);
    }

    Matrix4x4 worldM;
    float* worldData = worldM.Data();
    for (int i = 0; i < 16; ++i)
    {
        worldData[i] = worldTransform.matrix[i];
    }

    Camera activeCamera{};
    activeCamera.worldTransform = worldM;
    activeCamera.params = cameraComp;

    if (m_CameraId == 0) {
        m_CameraId = rs->Views().AllocateCamera("Game Camera");
    }

    if (m_ViewId == 0) {
        // View targets (color/depth/resolve) are intentionally left for the
        // application/render-setup layer to configure via SetViewTargets or
        // SetViewTargetsFromRefs, since they depend on RenderGraph surfaces.
        m_ViewId = rs->Views().AllocateView("Game View", m_CameraId);
        // IMPORTANT: a view with no targets should not participate in pipeline validation
        // or scheduling. Leave it disabled until a render-setup layer binds targets.
        rs->Views().SetViewRenderLayerMask(m_ViewId, 0u);
        rs->Views().SetViewWorldId(m_ViewId, world.GetWorldId());
    } else {
        rs->Views().SetViewCamera(m_ViewId, m_CameraId);
    }

    ApplyActiveCameraAspect(*rs, m_ViewId, m_CameraId, activeCamera, viewportW, viewportH);
    rs->Views().SetCameraPostProcessMask(m_CameraId, cameraComp.PostProcessMask);
}

} } // namespace GameEngine::Engine::Renderer
