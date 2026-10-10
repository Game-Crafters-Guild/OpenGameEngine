// ModelEntityFactory implementation: creates ECS entities from ModelAsset
// with full rendering component wiring and GPU resource registration.

#include "Engine/Rendering/ModelEntityFactory.h"

#include "Assets/ModelAsset.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h" // ensure AddComponentImmediate<T> template instantiations
#include "ECS/Entity.h"       // ComponentBundle
#include "ECS/World.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Types/StringId.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{
namespace Engine::Renderer
{

namespace
{

// True when the file owns the transform of the model's one mesh: its node transform is not
// identity (the Damaged Helmet's +X-up correction), or its node is animated, so a clip writes
// the node's pose over its entity's Transform. That entity then sits below the model root, so
// the root's Transform, the one a game sets to place the model, starts at identity and stays
// the game's.
bool SoleMeshIsPlacedByItsNode(const ModelAsset& modelAsset)
{
    constexpr float kIdentity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    if (modelAsset.GetMeshCount() != 1)
        return false;
    const Mesh& mesh = modelAsset.GetMesh(0);
    if (mesh.IsSkinned() || mesh.SourceNodeIndex < 0)
        return false;
    const bool nodeIsAnimated = modelAsset.GetSkeletonId() != 0;
    return nodeIsAnimated || std::memcmp(mesh.SourceNodeTransform, kIdentity, sizeof(kIdentity)) != 0;
}

} // namespace

ModelEntityResult ModelEntityFactory::CreateFromModel(
    RenderServices& rs,
    ECS::World& world,
    const ModelAsset& modelAsset,
    const GUID& modelGuid,
    const std::string& rootName,
    const ModelEntityFactoryOptions& options)
{
    ModelEntityResult result{};

    if (!modelAsset.IsLoaded())
        return result;

    const uint32 meshCount = modelAsset.GetMeshCount();
    const bool spawnImportedCameras = options.SpawnImportedCameras && !modelAsset.GetImportedCameras().empty();
    const bool spawnImportedLights = options.SpawnImportedLights && !modelAsset.GetImportedLights().empty();
    const bool spawnImportedHelperNodes = options.SpawnImportedHelperNodes && !modelAsset.GetImportedSceneNodes().empty();
    const bool hasSceneExtras = spawnImportedCameras || spawnImportedLights || spawnImportedHelperNodes;
    if (meshCount == 0 && !hasSceneExtras)
        return result;

    using Clock = std::chrono::high_resolution_clock;
    const auto tTotal = Clock::now();
    auto msElapsed = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

    // --- Register GPU meshes and materials ---
    auto& meshReg = rs.GetMeshGPURegistry();

    ModelRenderResources renderRes{};
    renderRes.modelGuid = modelGuid;
    renderRes.modelAsset = &modelAsset;

    const auto tMesh = Clock::now();
    renderRes.meshHandles = meshReg.RegisterModelMeshes(modelGuid, modelAsset);
    const double meshMs = msElapsed(tMesh);

    const auto tConvert = Clock::now();
    renderRes.materials = ModelMaterialBridge::ConvertAll(
        modelGuid, modelAsset.GetMaterials());
    const double convertMs = msElapsed(tConvert);

    const auto& embeddedImages = modelAsset.GetEmbeddedImages();

    double materialRegMs = 0.0;
    double embeddedTexMs = 0.0;
    for (const auto& cm : renderRes.materials)
    {
        const auto tMatReg = Clock::now();
        Material* mat = rs.RegisterAndPrewarmMaterial(cm.derivedGuid, cm.document);
        materialRegMs += msElapsed(tMatReg);

        const auto tEmb = Clock::now();
        rs.Textures().ResolveEmbeddedTextures(mat, cm.document, modelGuid, embeddedImages);
        embeddedTexMs += msElapsed(tEmb);
    }

    // --- Determine if skinned ---
    bool hasSkinning = false;
    for (uint32 i = 0; i < meshCount; ++i)
    {
        if (modelAsset.GetMesh(i).IsSkinned())
        {
            hasSkinning = true;
            break;
        }
    }
    result.skinned = hasSkinning;

    // Allocate a per-instance skeleton runtime so each model instance can
    // animate independently even if they share the same skeleton data. Every
    // SkeletonRef written below takes its own reference (SkeletonStore.h
    // ownership rule); this scope holds the construction reference so a
    // failure path that writes none still releases the runtime.
    uint32 instanceRuntimeId = 0;
    if (hasSkinning && modelAsset.GetSkeletonId() != 0)
    {
        auto& skStore = Engine::Renderer::SkeletonStore::Instance();
        instanceRuntimeId = skStore.CreateRuntime(modelAsset.GetSkeletonId());
    }
    struct ConstructionRef
    {
        uint32 RuntimeId;
        ~ConstructionRef()
        {
            if (RuntimeId != 0)
                Engine::Renderer::SkeletonStore::Instance().ReleaseRuntime(RuntimeId);
        }
    } constructionRef{instanceRuntimeId};

    // Runtime identity is assigned before any model entities can be consumed.
    // The owner is filled after creating the container (or the sole submesh).
    if (auto* runtime = Engine::Renderer::SkeletonStore::Instance().GetRuntime(instanceRuntimeId))
    {
        runtime->Source.ModelGuid = modelGuid;
        runtime->Source.WorldId = world.GetWorldId();
        runtime->Source.WorldGeneration = world.GetLifecycleResetGeneration();
    }

    // Every SkeletonRef this factory writes holds one reference.
    const auto retainForSkeletonRef = [&]()
    {
        if (instanceRuntimeId != 0)
            Engine::Renderer::SkeletonStore::Instance().RetainRuntime(instanceRuntimeId);
    };

    const auto makeSkeletonRef = [&]()
    {
        Components::SkeletonRef ref{};
        ref.skeletonId = modelAsset.GetSkeletonId();
        ref.runtimeId = instanceRuntimeId;
        ref.runtimeGeneration = Engine::Renderer::SkeletonStore::Instance().GetRuntimeGeneration(instanceRuntimeId);
        ref.sourceModelGuid.Set(modelGuid);
        ref.instanceOwner = result.rootEntity;
        if (ref.instanceOwner.IsValid())
            ref.ownerMode = Components::SkeletonInstanceOwner::Entity;
        return ref;
    };

    // Build a ComponentBundle per entity so each entity lands in its target
    // archetype with a single migration, rather than N sequential ones.
    bool hasExtraPlacements = false;
    for (uint32 i = 0; i < meshCount && !hasExtraPlacements; ++i)
        hasExtraPlacements = !modelAsset.GetMesh(i).ExtraPlacements.empty();
    const bool multiSubmesh = meshCount > 1 || hasExtraPlacements;
    const bool soleMeshPlacedByNode = SoleMeshIsPlacedByItsNode(modelAsset);
    const bool needsContainerRoot = multiSubmesh || hasSceneExtras || soleMeshPlacedByNode;
    const std::string baseName = rootName.empty() ? "Model" : rootName;

    // `rootEntity` receives the Animator: the container root when there is
    // one, or else the sole submesh. Bake it in up-front so it's part of the
    // entity's initial archetype.
    const bool needsRootAnimator = modelAsset.HasAnimations() && modelAsset.GetSkeletonId() != 0;

    const auto tEntityCreate = Clock::now();

    if (needsContainerRoot)
    {
        ECS::ComponentBundle rootBundle;
        rootBundle.Add(Components::Transform{});

        Components::Name rootNm{};
        std::strncpy(rootNm.value, baseName.c_str(), sizeof(rootNm.value) - 1);
        rootNm.value[sizeof(rootNm.value) - 1] = '\0';
        rootBundle.Add(rootNm);

        rootBundle.Add(ComputeModelBounds(modelAsset));

        if (needsRootAnimator)
        {
            Components::Animator animator{};
            animator.autoPlayOnEnterPlayMode = true;
            if (!modelAsset.GetEmbeddedClipGuids().empty())
                animator.SelectEmbeddedClip(modelAsset.GetGUID(), 0);
            rootBundle.Add(animator);
        }

        result.rootEntity = world.CreateFromBundleHandle(rootBundle);
        if (!result.rootEntity.IsValid())
            return result;
        if (auto* runtime = Engine::Renderer::SkeletonStore::Instance().GetRuntime(instanceRuntimeId))
            runtime->Source.InstanceOwner = result.rootEntity;
    }

    std::unordered_map<int32, ECS::EntityHandle> sceneNodeEntities;
    if (spawnImportedHelperNodes)
    {
        const auto& importedNodes = modelAsset.GetImportedSceneNodes();
        sceneNodeEntities.reserve(importedNodes.size());

        // Bone-ness is membership, not creation order: a parent index absent
        // from importedNodes is a skeleton bone (bones are never imported as
        // helper nodes), while a parent that is present but not yet created
        // this pass must defer, not fall back to the root with its world
        // transform.
        std::unordered_set<int32> importedNodeIndices;
        importedNodeIndices.reserve(importedNodes.size());
        for (const auto& imported : importedNodes)
        {
            if (imported.SourceNodeIndex >= 0)
                importedNodeIndices.insert(imported.SourceNodeIndex);
        }

        bool madeProgress = true;
        while (sceneNodeEntities.size() < importedNodes.size() && madeProgress)
        {
            madeProgress = false;
            for (const auto& imported : importedNodes)
            {
                if (imported.SourceNodeIndex < 0 || sceneNodeEntities.find(imported.SourceNodeIndex) != sceneNodeEntities.end())
                    continue;
                ECS::EntityHandle parentEntity = result.rootEntity;
                const bool parentIsImportedBone =
                    imported.ParentSourceNodeIndex >= 0
                    && !importedNodeIndices.contains(imported.ParentSourceNodeIndex)
                    && modelAsset.GetSkeletonId() != 0;
                if (imported.ParentSourceNodeIndex >= 0)
                {
                    const auto parentIt = sceneNodeEntities.find(imported.ParentSourceNodeIndex);
                    if (parentIt == sceneNodeEntities.end() && !parentIsImportedBone)
                        continue;
                    if (parentIt != sceneNodeEntities.end())
                        parentEntity = parentIt->second;
                }

                ECS::ComponentBundle bundle;
                Components::Transform xf{};
                std::memcpy(xf.matrix,
                            (imported.ParentSourceNodeIndex >= 0 && !parentIsImportedBone)
                                ? imported.LocalTransform
                                : imported.Transform,
                            sizeof(xf.matrix));
                bundle.Add(xf);

                Components::Name nm{};
                const std::string nameStr = imported.Name.empty() ? (baseName + "_node") : imported.Name;
                std::strncpy(nm.value, nameStr.c_str(), sizeof(nm.value) - 1);
                nm.value[sizeof(nm.value) - 1] = '\0';
                bundle.Add(nm);

                if (parentEntity.IsValid())
                {
                    Components::Parent parent{};
                    parent.parent = parentEntity;
                    bundle.Add(parent);
                }

                bool bundleHasSkeletonRef = false;
                if (parentIsImportedBone || (modelAsset.GetSkeletonId() != 0 && imported.SourceNodeIndex >= 0))
                {
                    const auto skeletonRef = makeSkeletonRef();
                    bundle.Add(skeletonRef);
                    bundleHasSkeletonRef = true;

                    Components::AnimatorRef animator{};
                    animator.Flags = Components::AnimatorRef::kFlag_Loop;
                    bundle.Add(animator);

                    Components::AnimatedNodeRef nodeRef{};
                    nodeRef.nodeIndex = static_cast<uint32>(imported.SourceNodeIndex);
                    bundle.Add(nodeRef);
                }

                ECS::EntityHandle entity = world.CreateFromBundleHandle(bundle);
                if (entity.IsValid())
                {
                    if (bundleHasSkeletonRef)
                        retainForSkeletonRef();
                    sceneNodeEntities[imported.SourceNodeIndex] = entity;
                    madeProgress = true;
                }
            }
        }
    }

    for (uint32 i = 0; i < meshCount; ++i)
    {
        const auto& mesh = modelAsset.GetMesh(i);
        const auto* gpuEntry = (i < renderRes.meshHandles.size()) ? meshReg.Find(renderRes.meshHandles[i]) : nullptr;

        // One entity per node that draws the mesh: placement 0 is the mesh's own
        // node, the rest its ExtraPlacements, all sharing the GPU mesh.
        const size_t placementCount = 1u + mesh.ExtraPlacements.size();
        for (size_t placementIndex = 0; placementIndex < placementCount; ++placementIndex)
        {
            const MeshPlacement* extraPlacement =
                placementIndex == 0 ? nullptr : &mesh.ExtraPlacements[placementIndex - 1u];
            const int32 sourceNodeIndex = extraPlacement ? extraPlacement->SourceNodeIndex : mesh.SourceNodeIndex;

            ECS::ComponentBundle bundle;
            bool bundleHasSkeletonRef = false;

            // Transform.
            Components::Transform xf{};
            if (!mesh.IsSkinned() && sourceNodeIndex >= 0)
            {
                if (sceneNodeEntities.find(sourceNodeIndex) != sceneNodeEntities.end())
                    std::memcpy(xf.matrix,
                                extraPlacement ? extraPlacement->SourceNodeLocalTransform : mesh.SourceNodeLocalTransform,
                                sizeof(xf.matrix));
                else
                    std::memcpy(xf.matrix,
                                extraPlacement ? extraPlacement->SourceNodeTransform : mesh.SourceNodeTransform,
                                sizeof(xf.matrix));
            }
            bundle.Add(xf);

            // Name.
            {
                Components::Name nm{};
                std::string nameStr = multiSubmesh
                                          ? (baseName + "_submesh" + std::to_string(i))
                                          : baseName;
                if (extraPlacement)
                    nameStr += "_" + std::to_string(placementIndex);
                std::strncpy(nm.value, nameStr.c_str(), sizeof(nm.value) - 1);
                nm.value[sizeof(nm.value) - 1] = '\0';
                bundle.Add(nm);
            }

            // Parent (if this model uses a container root).
            const auto sceneParentIt = sourceNodeIndex >= 0
                ? sceneNodeEntities.find(sourceNodeIndex)
                : sceneNodeEntities.end();
            if (sceneParentIt != sceneNodeEntities.end() && sceneParentIt->second.IsValid())
            {
                Components::Parent parent{};
                parent.parent = sceneParentIt->second;
                bundle.Add(parent);
            }
            else if (needsContainerRoot && result.rootEntity.IsValid())
            {
                Components::Parent parent{};
                parent.parent = result.rootEntity;
                bundle.Add(parent);
            }

            // Rendering component(s).
            auto addMorphTargetWeights = [&](const Components::MeshRenderer& mr)
            {
                if (!mesh.HasMorphTargets())
                    return;

                Components::MorphTargetWeights morph{};
                morph.weightCount = std::min<uint32>(
                    static_cast<uint32>(mesh.MorphTargets.size()),
                    Components::MorphTargetWeights::kMaxWeights);
                for (uint32 wi = 0; wi < morph.weightCount && wi < mesh.MorphTargetDefaultWeights.size(); ++wi)
                    morph.weights[wi] = mesh.MorphTargetDefaultWeights[wi];
                morph.version = 2;
                morph.sourceMeshId = i;
                morph.sourceMeshGpuHandleId = mr.meshGpuHandleId;
                modelGuid.WriteBytes(morph.sourceModelGuid);
                bundle.Add(morph);
            };

            if (mesh.IsSkinned())
            {
                Components::SkinnedMeshRenderer smr{};
                smr.meshId = 0;
                smr.skeletonId = modelAsset.GetSkeletonId();
                bundle.Add(smr);

                // Skinned meshes also get a MeshRenderer for GPU submission via the
                // MeshGPU/Material path (bind-pose fallback when animation is inactive).
                Components::MeshRenderer mr{};
                mr.meshId = i;
                PopulateMeshRenderer(mr, renderRes, i, MeshMaterialFill::FromModel);
                bundle.Add(mr);
                addMorphTargetWeights(mr);

                // Skeleton ref.
                if (smr.skeletonId != 0)
                {
                    const auto sref = makeSkeletonRef();
                    bundle.Add(sref);
                    bundleHasSkeletonRef = true;
                }

                // Animator ref (default: no clip assigned yet).
                Components::AnimatorRef anim{};
                anim.Flags = Components::AnimatorRef::kFlag_Loop;
                bundle.Add(anim);
            }
            else
            {
                Components::MeshRenderer mr{};
                mr.meshId = i;
                PopulateMeshRenderer(mr, renderRes, i, MeshMaterialFill::FromModel);
                bundle.Add(mr);
                addMorphTargetWeights(mr);

                if (modelAsset.GetSkeletonId() != 0 && sourceNodeIndex >= 0)
                {
                    const auto skeletonRef = makeSkeletonRef();
                    bundle.Add(skeletonRef);
                    bundleHasSkeletonRef = true;

                    Components::AnimatorRef animator{};
                    animator.Flags = Components::AnimatorRef::kFlag_Loop;
                    bundle.Add(animator);

                    Components::AnimatedNodeRef nodeRef{};
                    nodeRef.nodeIndex = static_cast<uint32>(sourceNodeIndex);
                    bundle.Add(nodeRef);
                }
            }

            // Bounds.
            if (gpuEntry)
            {
                Components::LocalBounds lb{};
                lb.Box = gpuEntry->bounds;
                bundle.Add(lb);
            }

            // Pre-allocate the GPUScene-index cache so RenderExtractionSystem
            // doesn't trigger a first-frame archetype migration to add it.
            bundle.Add(Components::MeshGPUData{});

            // Without a container root the sole submesh entity becomes the root,
            // so the root Animator folds into this bundle.
            if (!needsContainerRoot && needsRootAnimator)
            {
                Components::Animator animator{};
                animator.autoPlayOnEnterPlayMode = true;
                if (!modelAsset.GetEmbeddedClipGuids().empty())
                    animator.SelectEmbeddedClip(modelAsset.GetGUID(), 0);
                bundle.Add(animator);
            }

            ECS::EntityHandle entity = world.CreateFromBundleHandle(bundle);
            if (!entity.IsValid())
                continue;

            if (bundleHasSkeletonRef)
                retainForSkeletonRef();

            if (!needsContainerRoot)
            {
                if (auto* runtime = Engine::Renderer::SkeletonStore::Instance().GetRuntime(instanceRuntimeId))
                    runtime->Source.InstanceOwner = entity;
            }

            result.submeshEntities.push_back(entity);
        }
    }

    auto addName = [](ECS::ComponentBundle& bundle, const std::string& nameStr)
    {
        Components::Name nm{};
        std::strncpy(nm.value, nameStr.c_str(), sizeof(nm.value) - 1);
        nm.value[sizeof(nm.value) - 1] = '\0';
        bundle.Add(nm);
    };

    auto addParent = [&](ECS::ComponentBundle& bundle)
    {
        if (!needsContainerRoot || !result.rootEntity.IsValid())
            return;
        Components::Parent parent{};
        parent.parent = result.rootEntity;
        bundle.Add(parent);
    };

    if (spawnImportedCameras)
    {
        for (const auto& imported : modelAsset.GetImportedCameras())
        {
            if (imported.SourceNodeIndex >= 0)
            {
                const auto it = sceneNodeEntities.find(imported.SourceNodeIndex);
                if (it != sceneNodeEntities.end() && it->second.IsValid())
                {
                    Components::Camera camera{};
                    camera.Perspective = imported.Perspective;
                    camera.FovY = imported.FovY;
                    camera.OrthographicSize = imported.OrthographicSize;
                    camera.NearZ = imported.NearZ;
                    camera.FarZ = imported.FarZ;
                    world.AddComponentImmediate(it->second, camera);
                    continue;
                }
            }

            ECS::ComponentBundle bundle;
            Components::Transform xf{};
            std::memcpy(xf.matrix, imported.Transform, sizeof(xf.matrix));
            bundle.Add(xf);
            addName(bundle, imported.Name.empty() ? (baseName + "_camera") : imported.Name);
            addParent(bundle);

            Components::Camera camera{};
            camera.Perspective = imported.Perspective;
            camera.FovY = imported.FovY;
            camera.OrthographicSize = imported.OrthographicSize;
            camera.NearZ = imported.NearZ;
            camera.FarZ = imported.FarZ;
            bundle.Add(camera);

            world.CreateFromBundleHandle(bundle);
        }
    }

    if (spawnImportedLights)
    {
        for (const auto& imported : modelAsset.GetImportedLights())
        {
            ECS::ComponentBundle bundle;
            Components::Transform xf{};
            std::memcpy(xf.matrix, imported.Transform, sizeof(xf.matrix));
            bundle.Add(xf);
            addName(bundle, imported.Name.empty() ? (baseName + "_light") : imported.Name);
            addParent(bundle);

            Components::Light light{};
            switch (imported.Type)
            {
            case ImportedLightType::Directional: light.Type = Components::LightType::Directional; break;
            case ImportedLightType::Spot:        light.Type = Components::LightType::Spot;        break;
            case ImportedLightType::Area:        light.Type = Components::LightType::Area;        break;
            case ImportedLightType::Volume:      light.Type = Components::LightType::Volume;      break;
            case ImportedLightType::Point:
            default:                             light.Type = Components::LightType::Point;       break;
            }
            switch (imported.AreaShape)
            {
            case ImportedAreaLightShape::Sphere:   light.AreaShape = Components::AreaLightShape::Sphere;   break;
            case ImportedAreaLightShape::Disc:     light.AreaShape = Components::AreaLightShape::Disc;     break;
            case ImportedAreaLightShape::Cylinder: light.AreaShape = Components::AreaLightShape::Cylinder; break;
            case ImportedAreaLightShape::Rectangle:
            default:                               light.AreaShape = Components::AreaLightShape::Rectangle; break;
            }
            light.Color[0] = imported.Color[0];
            light.Color[1] = imported.Color[1];
            light.Color[2] = imported.Color[2];
            light.Intensity = imported.Intensity;
            light.Range = imported.Range;
            light.InnerAngle = imported.InnerAngle;
            light.OuterAngle = imported.OuterAngle;
            light.AreaWidth = imported.AreaWidth;
            light.AreaHeight = imported.AreaHeight;
            light.AreaRadius = imported.AreaRadius;
            light.Falloff = Components::LightFalloff::Custom;
            light.Decay = imported.Decay;
            light.CastsLight = imported.CastsLight;
            light.CastsShadows = imported.CastsShadows;
            if (imported.SourceNodeIndex >= 0)
            {
                const auto it = sceneNodeEntities.find(imported.SourceNodeIndex);
                if (it != sceneNodeEntities.end() && it->second.IsValid())
                {
                    world.AddComponentImmediate(it->second, light);
                    continue;
                }
            }

            bundle.Add(light);
            world.CreateFromBundleHandle(bundle);
        }
    }

    // Without a container root (one rigid submesh drawn once, at an unanimated
    // identity node, no imported scene extras), root = the renderable entity.
    if (!needsContainerRoot && !result.submeshEntities.empty())
    {
        result.rootEntity = result.submeshEntities[0];
    }

    const double entityCreateMs = msElapsed(tEntityCreate);
    const double totalMs = msElapsed(tTotal);
    Logger::Log::Info(
        "[ModelLoad] '{}' ({} meshes, {} mats, {} embedded imgs, {} cameras, {} lights, {} helpers): "
        "TOTAL {:.1f}ms | mesh {:.1f}ms | matConvert {:.1f}ms | matRegister {:.1f}ms | embeddedTex {:.1f}ms | entityCreate {:.2f}ms",
        rootName, meshCount, renderRes.materials.size(), embeddedImages.size(),
        modelAsset.GetImportedCameras().size(), modelAsset.GetImportedLights().size(),
        modelAsset.GetImportedSceneNodes().size(),
        totalMs, meshMs, convertMs, materialRegMs, embeddedTexMs, entityCreateMs);

    return result;
}

} // namespace Engine::Renderer
} // namespace GameEngine
