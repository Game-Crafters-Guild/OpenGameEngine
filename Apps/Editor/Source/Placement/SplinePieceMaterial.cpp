#include "Placement/SplinePieceMaterial.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

namespace GameEngine::Editor
{

bool EnsureOverrideMaterialRegistered(Engine::Renderer::RenderServices& renderServices,
                                      const Components::MaterialRef& overrideMaterial,
                                      OverrideMaterialCache& registered)
{
    const GUID guid = overrideMaterial.ToGuid();
    if (guid.IsNull())
        return false;
    if (registered.insert(guid).second)
    {
        Engine::Renderer::RegisterOneStandaloneMaterial(renderServices, guid);
        // RegisterOneStandaloneMaterial is void and two of its three failure
        // exits are silent; this is the one observable a failed override gets.
        if (!renderServices.Materials().Registry().Find(guid))
            Logger::Log::Warning(
                "Spline recipe override material {} could not be registered - "
                "pieces keep their embedded model material",
                guid.ToString());
    }
    // Re-checked every rebuild, not cached: if a later scene load or editor
    // interaction registers this GUID, the override starts applying without an
    // editor restart.
    return renderServices.Materials().Registry().Find(guid) != nullptr;
}

void BindPieceMaterial(Components::MeshRenderer& renderer,
                       const Engine::Renderer::ModelRenderResources& resources,
                       uint32 submeshIndex,
                       const Components::MaterialRef& overrideMaterial)
{
    // Seeding the material ahead of the fill is what makes PreserveExplicit keep
    // it. A null override leaves the field clear, and PreserveExplicit then fills
    // from the embedded model material on exactly the path FromModel takes.
    renderer.materialAssetGuid = overrideMaterial;
    Engine::Renderer::PopulateMeshRenderer(renderer, resources, submeshIndex,
                                           Engine::Renderer::MeshMaterialFill::PreserveExplicit);
}

} // namespace GameEngine::Editor
