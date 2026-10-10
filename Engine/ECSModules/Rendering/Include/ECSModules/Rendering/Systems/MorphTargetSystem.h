#pragma once

#include "AssetCore/GUID.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class ModelAsset;

namespace Engine::Renderer
{

class RenderServices;

class MorphTargetSystem : public ECS::ISystem
{
public:
    explicit MorphTargetSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "MorphTargetSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Test seam: the production path resolves models through the engine's
    // AssetManager, which standalone test hosts never initialize. Installing
    // a resolver lets the morph deep path (source registration, runtime-mesh
    // re-register, the value-gated write sites) run against an in-memory
    // ModelAsset fixture (fusion T17).
    void SetModelResolverForTest(std::function<ModelAsset*(const GUID&)> resolver)
    {
        m_ModelResolverForTest = std::move(resolver);
    }

private:
    struct RuntimeMeshRecord
    {
        GUID runtimeGuid;
    };

    RenderServices* m_RenderServices = nullptr;
    std::unordered_map<ECS::EntityId, RuntimeMeshRecord> m_RuntimeMeshes;
    std::vector<ECS::EntityId> m_SeenEntities;
    std::function<ModelAsset*(const GUID&)> m_ModelResolverForTest;
};

} // namespace Engine::Renderer
} // namespace GameEngine
