#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"
#include "Rendering/CameraTypes.h"

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

class CameraSystem : public ECS::ISystem {
public:
    explicit CameraSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "CameraSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    RenderServices* m_RenderServices = nullptr; // non-owning

    // Cached identifiers for the primary ECS-driven game camera and view.
    GameEngine::Rendering::CameraId m_CameraId {0};
    GameEngine::Rendering::ViewId   m_ViewId   {0};
};

} } // namespace GameEngine::Engine::Renderer

