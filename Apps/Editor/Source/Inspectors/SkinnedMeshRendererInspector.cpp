#include "Inspectors/SkinnedMeshRendererInspector.h"

#include "InspectorRegistry.h"

#include "Assets/AssetManager.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Name.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"

#include "Inspectors/InspectorUIHelpers.h"

namespace GameEngine
{

using InspectorUI::AddLine;
using InspectorUI::BoolToString;
using InspectorUI::ToHex;

namespace
{
std::string RigSourceLabel(const Components::SkeletonRef& rig)
{
    if (rig.sourceModelGuid.IsNull())
        return rig.skeletonId != 0 ? "(procedural)" : "(none)";
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    AssetMetadata metadata{};
    if (assetManager
        && assetManager->GetRegistry().TryGetAssetMetadata(rig.sourceModelGuid.ToGuid(), metadata)
        && !metadata.Path.empty())
        return metadata.Path.filename().string();
    return rig.sourceModelGuid.ToGuid().ToString();
}

std::string RigOwnerLabel(const ECS::World& world, ECS::EntityHandle entity, const Components::SkeletonRef& rig)
{
    if (rig.ownerMode == Components::SkeletonInstanceOwner::Self)
        return "this entity";
    if (!rig.instanceOwner.IsValid() || !world.IsValid(rig.instanceOwner))
        return "(missing)";
    if (rig.instanceOwner == entity)
        return "this entity";
    const auto* name = world.GetComponent<Components::Name>(rig.instanceOwner);
    return name && name->value[0] != '\0' ? std::string(name->View()) : std::to_string(rig.instanceOwner.id);
}
} // namespace

void RegisterSkinnedMeshRendererInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        {
            return;
        }

        auto* smr = ctx.World->GetComponent<Components::SkinnedMeshRenderer>(ctx.Entity);
        if (!smr)
        {
            AddLine(ctx.Parent, "(SkinnedMeshRenderer missing)");
            return;
        }

        AddLine(ctx.Parent, std::string("MeshId: ") + std::to_string(smr->meshId));
        // The rig is authored on the entity's SkeletonRef: the model it comes
        // from and the entity whose instance shares its pose. The numeric
        // store ids are per-session caches and say nothing to the artist.
        if (const auto* rig = ctx.World->GetComponent<Components::SkeletonRef>(ctx.Entity))
        {
            AddLine(ctx.Parent, std::string("Rig source: ") + RigSourceLabel(*rig));
            AddLine(ctx.Parent, std::string("Rig owner: ") + RigOwnerLabel(*ctx.World, ctx.Entity, *rig));
            AddLine(ctx.Parent, std::string("Rig runtime: ") + (rig->runtimeId != 0 ? "resolved" : "unresolved"));
        }
        else
        {
            AddLine(ctx.Parent, "Rig source: (no SkeletonRef)");
        }
        AddLine(ctx.Parent, std::string("RenderLayerMask: ") + ToHex(smr->renderLayerMask));
        AddLine(ctx.Parent, std::string("CastShadows: ") + BoolToString(smr->castShadows));
        AddLine(ctx.Parent, std::string("ReceiveShadows: ") + BoolToString(smr->receiveShadows));
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::SkinnedMeshRenderer>(std::move(fn));
}

} // namespace GameEngine
