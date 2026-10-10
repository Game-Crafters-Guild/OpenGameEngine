#include "Assets/HlodMemberGather.h"

#include "Assets/ModelAsset.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Mathematics/Geometry.h"

#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include <string>

namespace GameEngine {
namespace Hlod {

namespace {

// FNV-1a fold of a 64-bit value into a running hash — the same primitive the
// .gehlod key uses, so the gathered content/handle keys and the cache's cluster
// hash share one arithmetic.
uint64 Fold(uint64 hash, uint64 value) {
    constexpr uint64 kFnvPrime = 1099511628211ull;
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFull;
        hash *= kFnvPrime;
    }
    return hash;
}

uint64 GuidFold(uint64 hash, const GUID& guid) {
    const auto& bytes = guid.GetData();
    constexpr uint64 kFnvPrime = 1099511628211ull;
    for (uint8 b : bytes) {
        hash ^= b;
        hash *= kFnvPrime;
    }
    return hash;
}

// Stable, save/load-surviving member identity. The SceneEntityTag is the scene's
// persistent per-entity id (SceneIO manages it from the `id=` header); deriving
// the member GUID from it lets a reloaded scene reconcile a baked cluster's
// members against the live entities (design §3.3 / F6). An untagged entity
// (runtime-spawned, no scene id) falls back to its runtime handle id — usable
// within a session (editor bake of a live scene) but not persistence-stable.
GUID MemberStableId(const Components::SceneEntityTag* tag, GameEngine::ECS::EntityHandle e) {
    if (tag != nullptr && tag->value[0] != '\0')
        return GUID::Derive(GUID::Null(), std::string("hlod-member:") + std::string(tag->View()));
    return GUID::Derive(GUID::Null(), "hlod-member-runtime:" + std::to_string(e.id));
}

} // namespace

GatheredMembers GatherHlodMembers(GameEngine::ECS::World& world, const ModelResolver& resolveModel) {
    GatheredMembers out;
    constexpr uint64 kFnvOffsetBasis = 14695981039346656037ull;

    // The rows the gather below never visits — an inactive entity or a switched-off renderer —
    // for the bake diagnostics. Archetype-level counts, no per-row work.
    {
        auto everyRenderer = world.Query<GameEngine::ECS::Read<GameEngine::Components::WorldTransform>,
                                         GameEngine::ECS::Read<GameEngine::Components::MeshRenderer>>();
        everyRenderer.IncludeDisabled();
        const std::size_t visited = world.Query<GameEngine::ECS::Read<GameEngine::Components::WorldTransform>,
                                                GameEngine::ECS::Read<GameEngine::Components::MeshRenderer>>()
                                        .Count();
        out.SkippedDisabled = static_cast<uint32>(everyRenderer.Count() - visited);
    }

    world.Query<
             GameEngine::ECS::Read<GameEngine::Components::WorldTransform>,
             GameEngine::ECS::Read<GameEngine::Components::MeshRenderer>,
             GameEngine::ECS::Optional<GameEngine::Components::LocalBounds>,
             GameEngine::ECS::Optional<GameEngine::Components::LODGroup>,
             GameEngine::ECS::Optional<GameEngine::Components::SkinnedMeshRenderer>,
             GameEngine::ECS::Optional<GameEngine::Components::MorphTargetWeights>,
             GameEngine::ECS::Optional<GameEngine::Components::SceneEntityTag>>()
        .Each([&](GameEngine::ECS::EntityHandle e,
                  const GameEngine::Components::WorldTransform& xf,
                  const GameEngine::Components::MeshRenderer& mr,
                  const GameEngine::Components::LocalBounds* localBounds,
                  const GameEngine::Components::LODGroup* /*lodGroup*/,
                  const GameEngine::Components::SkinnedMeshRenderer* skinned,
                  const GameEngine::Components::MorphTargetWeights* morph,
                  const GameEngine::Components::SceneEntityTag* tag) {
            // Skinned + morph members are out of v1 scope (the merge preserves
            // only the static interleaved stream + COLOR0/TEXCOORD1).
            if ((skinned != nullptr && skinned->skeletonId != 0)) {
                ++out.SkippedSkinned;
                return;
            }
            if (morph != nullptr) {
                ++out.SkippedMorph;
                return;
            }

            const GUID materialGuid = mr.materialAssetGuid.ToGuid();
            if (materialGuid.IsNull()) {
                ++out.SkippedNoMaterial;
                return;
            }

            const GUID modelGuid = mr.modelAssetGuid.ToGuid();
            const ModelAsset* model = modelGuid.IsNull() ? nullptr : resolveModel(modelGuid);
            if (model == nullptr || model->GetMeshCount() == 0) {
                ++out.SkippedNoModel;
                return;
            }

            const uint32 submeshIndex =
                Engine::Renderer::ResolveSubmeshIndex(modelGuid, *model, mr);
            const Mesh& mesh = model->GetMesh(submeshIndex);
            if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles || mesh.IsSkinned() ||
                mesh.HasMorphTargets()) {
                if (mesh.IsSkinned())
                    ++out.SkippedSkinned;
                else if (mesh.HasMorphTargets())
                    ++out.SkippedMorph;
                else
                    ++out.SkippedNonTriangle;
                return;
            }

            MemberInput member;
            member.StableId = MemberStableId(tag, e);
            member.MaterialGuid = materialGuid;
            // Content key: what changes when the member's baked geometry changes
            // (model reimport, submesh reselection). Handle key: which mesh the
            // member selected (identity, C3).
            member.MeshContentHash =
                Fold(Fold(kFnvOffsetBasis, model->GetSourceContentHash()), submeshIndex);
            member.MeshHandleKey = Fold(GuidFold(kFnvOffsetBasis, modelGuid), submeshIndex);

            for (int i = 0; i < 16; ++i)
                member.WorldMatrix[i] = xf.matrix[i];

            Mathematics::AABB worldAabb;
            if (localBounds != nullptr) {
                worldAabb = localBounds->Box.TransformToAABB(xf.matrix);
            } else {
                const Mathematics::BoundingBox box = Mathematics::BoundingBox::FromMinMax(
                    Mathematics::Vector3{mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]},
                    Mathematics::Vector3{mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]});
                worldAabb = box.TransformToAABB(xf.matrix);
            }
            member.WorldAabbMin[0] = worldAabb.min.x;
            member.WorldAabbMin[1] = worldAabb.min.y;
            member.WorldAabbMin[2] = worldAabb.min.z;
            member.WorldAabbMax[0] = worldAabb.max.x;
            member.WorldAabbMax[1] = worldAabb.max.y;
            member.WorldAabbMax[2] = worldAabb.max.z;

            member.ChosenLod = mesh.LODCount() - 1u; // coarsest authored/generated LOD
            // VB-budget estimate = what the bake actually copies: the chosen
            // level's own block when it has one (authored LOD / generated
            // attribute-honest shell), else the shared LOD0 vertices.
            const bool chosenOwnsVerts =
                member.ChosenLod >= 1u &&
                (member.ChosenLod - 1u) < mesh.ExtraLODVertices.size() &&
                !mesh.ExtraLODVertices[member.ChosenLod - 1u].empty();
            member.VertexCount = static_cast<uint32>(
                chosenOwnsVerts ? mesh.ExtraLODVertices[member.ChosenLod - 1u].size()
                                : mesh.Vertices.size());
            member.CastsShadow =
                mr.castShadows && (localBounds == nullptr || localBounds->CastShadows);

            out.Members.push_back(member);
            out.Geometry.push_back(&mesh);
            out.Entities.push_back(e);
        });

    return out;
}

} // namespace Hlod
} // namespace GameEngine
