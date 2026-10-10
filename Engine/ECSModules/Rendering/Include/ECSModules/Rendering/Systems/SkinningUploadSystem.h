#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;
class GPUAnimationDataStore;

// Uploads skinned bone palettes to GPU buffers; prepares per-instance skinning bindings for GPUScene
class SkinningUploadSystem : public ECS::ISystem {
public:
    explicit SkinningUploadSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "SkinningUploadSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    RenderServices* m_RenderServices = nullptr; // non-owning
    // Reused across frames to avoid per-frame allocation.
    std::vector<uint32> m_UniqueRuntimeIds;
};

} } // namespace GameEngine::Engine::Renderer

