#pragma once

#include "ECS/Systems.h"

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::EZTreeECS
{

class EZTreeExtractionSystem : public ECS::ISystem
{
public:
    explicit EZTreeExtractionSystem(Engine::Renderer::RenderServices* renderServices = nullptr)
        : m_RenderServices(renderServices)
    {
    }

    void Update(ECS::World& world, float32 deltaTime) override;

private:
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    // Q6 slice 3b: last observed device rebuild generation. A rebuild frees the
    // GPU meshes this system registered but leaves the content-hash gate matching,
    // so it would never regenerate them. When the generation moves, one Update
    // forces regen for every tree (which re-registers the branch/leaf/trellis
    // submeshes from the procedurally regenerated geometry).
    uint64 m_LastDeviceRebuildGeneration = 0;
};

} // namespace GameEngine::EZTreeECS
