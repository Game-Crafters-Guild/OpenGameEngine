// ModelThumbnailHandler, part: The preview scene a lane renders: the spawned model, material or
// lens flare, its framing, lights, animation and camera, and the render-graph submission of one
// thumbnail.

#include "Thumbnails/ModelThumbnailHandler.h"

#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/SkinningUploadSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/LensFlareRenderFeature.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/Pipeline/Nodes/LensFlareRenderNode.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Engine/Rendering/Systems/LensFlareExtractionSystem.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/NoneCullingStrategy.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Thumbnails/ModelThumbnailHandlerShared.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace GameEngine
{

namespace
{
// Persistent render targets are keyed by size: lanes of one size share a set
// within a frame (the render graph orders their passes), and the focused
// preview's panel-shaped set never reallocates the square grid set.
std::string MakeSizedTargetName(const char* base, uint32_t width, uint32_t height)
{
    return std::string(base) + "." + std::to_string(width) + "x" + std::to_string(height);
}

// Absolute photographic EV100 the preview tonemap exposes at. The preview is lit
// by the physically-bright shared-environment IBL, so it needs a real exposure
// rather than a fixed 1.0 (== reference white), which clips to white. Tuned for
// thumbnail brightness against the operator this path actually runs — Neutral,
// which is what TonemapParams defaults to and the preview never overrides — and
// deliberately independent of Components::kDefaultManualExposureEv: this is a
// fixed preview light box, not a camera anyone authors.
constexpr float kThumbnailPreviewEv100 = 14.15f;
} // namespace

std::unique_ptr<ECS::World> ModelThumbnailShared::MakeThumbnailWorld()
{
    auto world = std::make_unique<ECS::World>();
    Engine::Renderer::RegisterRenderWorldHooks(*world);
    return world;
}

bool ModelThumbnailHandler::EnsureAssetLoaded(WindowState& ws, const GUID& guid, Slot& slot,
                                              std::shared_ptr<ModelAsset>& outModelAsset)
{
    std::shared_ptr<ModelAsset> modelAsset = AcquireSlotModel(slot, guid);
    if (!modelAsset)
    {
        // A failed import is load-suppressed and never retried.
        if (IsSlotModelImporting(slot))
            EnqueuePending(ws, guid, slot.listStaticThumb);
        return false;
    }
    if (!modelAsset->IsLoaded())
    {
        NoteLoadWait(slot, *modelAsset, "the asset never reports loaded");
        EnqueuePending(ws, guid, slot.listStaticThumb);
        return false;
    }

    // Wait until all embedded textures have been loaded before rendering
    // the thumbnail. This prevents showing untextured white models.
    for (const auto& img : modelAsset->GetEmbeddedImages())
    {
        if (!img.HasContent())
        {
            NoteLoadWait(slot, *modelAsset, "an embedded image never gets content");
            EnqueuePending(ws, guid, slot.listStaticThumb);
            return false;
        }
    }
    slot.loadWaitSince = {};
    slot.loadWaitReported = false;

    outModelAsset = std::move(modelAsset);
    return true;
}

void ModelThumbnailHandler::ComputeFramingBounds(Slot& slot, const ModelAsset& modelAsset)
{
    if (slot.framingBoundsValid)
        return;

    float bbMin[3];
    float bbMax[3];
    modelAsset.GetBoundingBox(bbMin, bbMax);
    if (!(bbMin[0] <= bbMax[0] && bbMin[1] <= bbMax[1] && bbMin[2] <= bbMax[2]))
    {
        bbMin[0] = bbMin[1] = bbMin[2] = -0.5f;
        bbMax[0] = bbMax[1] = bbMax[2] = 0.5f;
    }
    slot.framingMin[0] = bbMin[0];
    slot.framingMin[1] = bbMin[1];
    slot.framingMin[2] = bbMin[2];
    slot.framingMax[0] = bbMax[0];
    slot.framingMax[1] = bbMax[1];
    slot.framingMax[2] = bbMax[2];
    slot.framingBoundsValid = true;
}

void ModelThumbnailHandler::SpawnOrUpdateModel(RenderLane& lane, Slot& slot, const GUID& guid,
                                                const ModelAsset& modelAsset,
                                                const Mathematics::Matrix4x4& modelMatrix)
{
    if (!lane.world)
    {
        lane.world = ModelThumbnailShared::MakeThumbnailWorld();
    }
    ECS::World& thumbWorld = *lane.world;

    if (lane.isSpawnedLensFlare)
    {
        thumbWorld.Clear();
        lane.isSpawnedLensFlare = false;
        lane.spawnedLensFlareGuid = GUID{};
        lane.lensFlareEntity = ECS::EntityHandle::Invalid();
        lane.orbitRootEntity = ECS::EntityHandle::Invalid();
        lane.keyLightEntity = ECS::EntityHandle::Invalid();
        lane.ambientLightEntity = ECS::EntityHandle::Invalid();
    }

    if (lane.isSpawnedMaterial)
    {
        TeardownSpawnedMaterial(lane);
        thumbWorld.Clear();
        lane.orbitRootEntity = ECS::EntityHandle::Invalid();
        lane.keyLightEntity = ECS::EntityHandle::Invalid();
        lane.ambientLightEntity = ECS::EntityHandle::Invalid();
        lane.isSpawnedMaterial = false;
    }

    if (lane.spawnedModelGuid != guid)
    {
        TeardownSpawnedModel(lane);
        thumbWorld.Clear();
        lane.spawnedSettleFrames = 0; // restart the upload settle window for the new model
        lane.orbitRootEntity = ECS::EntityHandle::Invalid();
        lane.keyLightEntity = ECS::EntityHandle::Invalid();
        lane.ambientLightEntity = ECS::EntityHandle::Invalid();

        // Create persistent light entities.
        {
            lane.keyLightEntity = thumbWorld.CreateEntity();
            Components::Light keyLightComp{};
            keyLightComp.Type = Components::LightType::Directional;
            keyLightComp.Color[0] = 1.0f;
            keyLightComp.Color[1] = 1.0f;
            keyLightComp.Color[2] = 1.0f;
            keyLightComp.Intensity = 1.0f;
            keyLightComp.CastsShadows = false;
            thumbWorld.AddComponentImmediate(lane.keyLightEntity, keyLightComp);
            Components::WorldTransform keyLightXf{};
            thumbWorld.AddComponentImmediate(lane.keyLightEntity, keyLightXf);

            lane.ambientLightEntity = thumbWorld.CreateEntity();
            Components::Light ambientComp{};
            ambientComp.Type = Components::LightType::Ambient;
            ambientComp.Color[0] = 1.0f;
            ambientComp.Color[1] = 1.0f;
            ambientComp.Color[2] = 1.0f;
            ambientComp.Intensity = std::max(0.0f, GetPreviewAmbientIntensity());
            ambientComp.CastsShadows = false;
            thumbWorld.AddComponentImmediate(lane.ambientLightEntity, ambientComp);
            Components::WorldTransform ambientXf{};
            thumbWorld.AddComponentImmediate(lane.ambientLightEntity, ambientXf);
        }

        // Spawn model via ModelEntityFactory (per-submesh entities, materials, GPU meshes).
        Engine::Renderer::ModelEntityFactoryOptions previewOptions{};
        previewOptions.SpawnImportedCameras = false;
        previewOptions.SpawnImportedLights = false;
        previewOptions.SpawnImportedHelperNodes = false;
        lane.spawnedResult = Engine::Renderer::ModelEntityFactory::CreateFromModel(
            *m_RenderServices, thumbWorld, modelAsset, guid, "Preview", previewOptions);

        if (!lane.spawnedResult.IsValid())
        {
            slot.ready = true;
            lane.spawnedModelGuid = GUID{};
            return;
        }

        // Create orbit root entity carrying the framing transform; parent
        // the model's root entity to it so TransformHierarchySystem propagates.
        lane.orbitRootEntity = thumbWorld.CreateEntity();
        Components::Transform orbitXf{};
        std::memcpy(orbitXf.matrix, modelMatrix.Data(), sizeof(orbitXf.matrix));
        thumbWorld.AddComponentImmediate(lane.orbitRootEntity, orbitXf);

        // Parent model root to orbit root.
        Components::Parent parentComp{};
        parentComp.parent = lane.orbitRootEntity;
        auto* existingParent = thumbWorld.GetComponentForWrite<Components::Parent>(lane.spawnedResult.rootEntity);
        if (existingParent)
            existingParent->parent = lane.orbitRootEntity;
        else
            thumbWorld.AddComponentImmediate(lane.spawnedResult.rootEntity, parentComp);

        lane.spawnedModelGuid = guid;
    }
    else
    {
        // Same model -- update orbit root's Transform with new orbit yaw / scale.
        if (lane.orbitRootEntity.IsValid())
        {
            auto* orbitXf = thumbWorld.GetComponentForWrite<Components::Transform>(lane.orbitRootEntity);
            if (orbitXf)
            {
                std::memcpy(orbitXf->matrix, modelMatrix.Data(), sizeof(orbitXf->matrix));
            }
        }
    }

    DisablePreviewShadowing(lane);
}

void ModelThumbnailHandler::DisablePreviewShadowing(RenderLane& lane)
{
    if (!lane.world)
        return;

    ECS::World& thumbWorld = *lane.world;
    for (const auto& entity : lane.spawnedResult.submeshEntities)
    {
        if (!entity.IsValid())
            continue;

        if (auto* mesh = thumbWorld.GetComponentForWrite<Components::MeshRenderer>(entity))
        {
            mesh->castShadows = false;
            mesh->receiveShadows = false;
        }
        if (auto* skinned = thumbWorld.GetComponentForWrite<Components::SkinnedMeshRenderer>(entity))
        {
            skinned->castShadows = false;
            skinned->receiveShadows = false;
        }
        if (auto* bounds = thumbWorld.GetComponentForWrite<Components::LocalBounds>(entity))
        {
            bounds->CastShadows = false;
        }
    }
}

void ModelThumbnailHandler::RunAnimationClipManagement(RenderLane& lane, Slot& slot, const GUID& guid,
                                                        const ModelAsset& modelAsset)
{
    if (!modelAsset.HasAnimations() || modelAsset.GetSkeletonId() == 0)
        return;

    ECS::World& thumbWorld = *lane.world;

    GUID previewGuid{};
    std::shared_ptr<AnimationClip> previewClip;
    float previewTime = 0.0f;
    {
        std::lock_guard<std::mutex> lock(s_AnimationPreviewMutex);
        previewGuid = s_AnimationPreviewGuid;
        previewClip = s_AnimationPreviewClip;
        previewTime = s_AnimationPreviewTime;
    }

    const bool useExternalPreview = (!previewGuid.IsNull() &&
                                     slot.guid == previewGuid &&
                                     previewClip &&
                                     previewClip->IsLoaded());
    if (!useExternalPreview && slot.usingExternalPreviewClip)
    {
        slot.animClip.reset();
        slot.animLoadedPos = -1;
        slot.usingExternalPreviewClip = false;
    }

    if (useExternalPreview)
    {
        slot.animClip = previewClip;
        slot.animTime = previewTime;
        slot.usingExternalPreviewClip = true;
    }

    // Positions index the model's embedded clips. Its import already parsed each
    // one and registered it in the ClipStore under GetEmbeddedClipGuids(); the
    // thumbnail binds those clips and never parses the source file again.
    const Vector<GUID>& embeddedClips = modelAsset.GetEmbeddedClipGuids();
    if (!useExternalPreview && slot.animAvailableIndices.empty() && !embeddedClips.empty())
    {
        slot.animAvailableIndices.reserve(embeddedClips.size());
        for (size_t i = 0; i < embeddedClips.size(); ++i)
            slot.animAvailableIndices.push_back(static_cast<uint32_t>(i));
        slot.animSelectedPos = 0;
        slot.animLoadedPos = -1;
        slot.animTime = 0.0f;
    }

    // Clamp selected position into range.
    if (!slot.animAvailableIndices.empty())
    {
        const int count = static_cast<int>(slot.animAvailableIndices.size());
        if (slot.animSelectedPos < 0 || slot.animSelectedPos >= count)
        {
            int pos = ((slot.animSelectedPos % count) + count) % count;
            slot.animSelectedPos = pos;
            slot.animLoadedPos = -1;
            slot.animTime = 0.0f;
        }
    }

    // Bind the embedded clip at the selected position. A stack the import could
    // not turn into a clip (no animated channels) has no ClipStore entry, and
    // the model keeps its bind pose.
    if (!useExternalPreview && !slot.animAvailableIndices.empty() &&
        slot.animLoadedPos != slot.animSelectedPos)
    {
        const uint32_t animIndex = slot.animAvailableIndices[static_cast<size_t>(slot.animSelectedPos)];
        auto& clipStore = Engine::Renderer::ClipStore::Instance();
        const uint32_t clipIndex =
            animIndex < embeddedClips.size() ? clipStore.GetIndexIfPresent(embeddedClips[animIndex]) : 0u;
        slot.animClip = clipIndex != 0 ? clipStore.Get(clipIndex) : nullptr;
        slot.cachedClipIndex = clipIndex;
        slot.cachedClipPos = slot.animSelectedPos;
        slot.animTime = 0.0f;
        slot.animLoadedPos = slot.animSelectedPos;
    }

    // Advance animation time for orbit re-renders.
    if (slot.animClip && slot.animClip->IsLoaded() && slot.animClip->GetDuration() > 0.0f)
    {
        if (useExternalPreview)
        {
            const float dur = slot.animClip->GetDuration();
            while (slot.animTime < 0.0f)
                slot.animTime += dur;
            while (slot.animTime >= dur)
                slot.animTime -= dur;
        }
        else if (slot.needsOrbitRerender)
        {
            // Advance by real elapsed time (kThumbFrameDt * s_PreviewFrameScale
            // resolves to seconds) so playback runs at a constant speed across
            // render frame rates.
            constexpr float kThumbFrameDt = 1.0f / 60.0f;
            slot.animTime += kThumbFrameDt * s_PreviewFrameScale;
            const float dur = slot.animClip->GetDuration();
            while (slot.animTime >= dur)
                slot.animTime -= dur;
        }

        // An external preview clip is not a ClipStore entry of this model:
        // register it so AnimationSystem can sample it. Embedded clips were
        // bound by index above.
        if (slot.usingExternalPreviewClip && slot.cachedClipPos != slot.animLoadedPos)
        {
            auto& clipStore = Engine::Renderer::ClipStore::Instance();
            const std::string clipSuffix = "editor/thumbnail/anim_clip/" + std::to_string(slot.animLoadedPos);
            const GUID clipGuid = GUID::Derive(guid, clipSuffix);
            slot.cachedClipIndex = clipStore.RegisterRuntimeClip(clipGuid, slot.animClip);
            slot.cachedClipPos = slot.animLoadedPos;
        }

        if (slot.cachedClipIndex != 0)
        {
            for (const auto& entity : lane.spawnedResult.submeshEntities)
            {
                if (!entity.IsValid())
                    continue;
                auto* animRef = thumbWorld.GetComponentForWrite<Components::AnimatorRef>(entity);
                if (animRef)
                {
                    animRef->ClipIndex = slot.cachedClipIndex;
                    animRef->Time = slot.animTime;
                    animRef->Speed = 0.0f;
                    animRef->Flags = Components::AnimatorRef::kFlag_Loop;
                }
            }
        }
    }
}

void ModelThumbnailHandler::UpdatePreviewLights(RenderLane& lane, float orbitYaw)
{
    ECS::World& thumbWorld = *lane.world;
    // Ambient fill follows its own knob and has no orientation; refresh it
    // independently so it stays live even if the key light is absent.
    if (lane.ambientLightEntity.IsValid())
    {
        auto* ambientComp = thumbWorld.GetComponentForWrite<Components::Light>(lane.ambientLightEntity);
        if (ambientComp)
            ambientComp->Intensity = std::max(0.0f, GetPreviewAmbientIntensity());
    }

    if (!lane.keyLightEntity.IsValid())
        return;

    const float previewIntensity = std::clamp(GetPreviewLightIntensity(), 0.25f, 4.0f);
    auto* lightComp = thumbWorld.GetComponentForWrite<Components::Light>(lane.keyLightEntity);
    if (lightComp)
        lightComp->Intensity = previewIntensity;

    // Build a WorldTransform whose +Z axis is the desired light direction.
    const float cy = std::cos(orbitYaw);
    const float sy = std::sin(orbitYaw);
    constexpr float kBaseDirX = -0.267f;
    constexpr float kBaseDirY = -0.802f;
    constexpr float kBaseDirZ = -0.535f;
    const float dirX = cy * kBaseDirX + sy * kBaseDirZ;
    const float dirY = kBaseDirY;
    const float dirZ = -sy * kBaseDirX + cy * kBaseDirZ;

    // WorldTransform col2 = shine direction (extraction reads +col2).
    auto* lightXf = thumbWorld.GetComponentForWrite<Components::WorldTransform>(lane.keyLightEntity);
    if (lightXf)
    {
        lightXf->matrix[8] = dirX;
        lightXf->matrix[9] = dirY;
        lightXf->matrix[10] = dirZ;
        // Direct-writer contract (Transform.h): bump Version with the matrix.
        // Lights aren't version-gated today, but extraction's instance skip
        // makes the contract load-bearing for anything renderable. Thumbnail
        // worlds never enable the dirty feed, so the helper's emission is a
        // no-op here — but the contract site stays uniform.
        Components::BumpWorldTransform(thumbWorld, lane.keyLightEntity, *lightXf);
    }
}

void ModelThumbnailHandler::EnsureViewAndCamera(RenderLane& lane)
{
    if (lane.cameraId == 0)
        lane.cameraId = m_RenderServices->Views().AllocateCamera("Editor Model Thumbnail Camera");

    if (lane.viewId == 0)
    {
        lane.viewId = m_RenderServices->Views().AllocateView("Editor Model Thumbnail View", lane.cameraId,
                                                            Rendering::ViewPurpose::EditorPreview);
        // Thumbnails skip GPU frustum culling: their bucketer reads from
        // per-frame extraction (every entity in the thumbnail world is meant
        // to render). Skipping the culling dispatch saves a per-view compute
        // submit and a visibility-buffer slot.
        m_RenderServices->Views().SetViewCullingStrategy(
            lane.viewId, std::make_shared<Rendering::NoneCullingStrategy>());
    }
    else
    {
        m_RenderServices->Views().SetViewCamera(lane.viewId, lane.cameraId);
    }
}

void ModelThumbnailHandler::TickThumbnailSystems(ECS::World& thumbWorld)
{
    // Order mirrors RegisterRenderingSystems:
    // Animation -> HumanoidRetarget -> Skinning -> TransformHierarchy -> RenderExtraction.
    // deltaTime is 0 because the handler manages AnimatorRef.Time directly.
    //
    // HumanoidRetargetSystem runs after AnimationSystem so the cross-rig
    // auto-bootstrap (which adds HumanoidRetargeterComponent on detected
    // structural mismatch) sees its component the same frame the retarget
    // system processes it. Same-rig thumbnails are unaffected — the retarget
    // system's q.Each finds zero entities and returns immediately.
    m_AnimationSystem->Update(thumbWorld, 0.0f);
    m_HumanoidRetargetSystem->Update(thumbWorld, 0.0f);
    m_SkinningUploadSystem->Update(thumbWorld, 0.0f);
    m_TransformSystem->Update(thumbWorld, 0.0f);
    m_ExtractionSystem->Update(thumbWorld, 0.0f);
}

void ModelThumbnailHandler::SetupCameraMatrices(const RenderLane& lane, uint32_t attachmentW,
                                                uint32_t attachmentH)
{
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    constexpr float kFovRad = 45.0f * 3.1415926535f / 180.0f;
    const Vector3 eye(0.9f, 0.9f, 1.8f);
    const Vector3 target(0.0f, 0.0f, 0.0f);
    const Vector3 up(0.0f, 1.0f, 0.0f);

    const float aspect = (attachmentH > 0)
        ? static_cast<float>(attachmentW) / static_cast<float>(attachmentH)
        : 1.0f;
    constexpr float kNearZ = 0.1f;
    constexpr float kFarZ = 10.0f;

    Matrix4x4 viewM = GameEngine::Mathematics::MakeLookAtLH(eye, target, up);
    Matrix4x4 projM = GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(kFovRad, aspect, kNearZ, kFarZ);
    Matrix4x4 viewProjM = projM * viewM;

    Rendering::CameraData cam{};
    const float* viewSrc = viewM.Data();
    const float* projSrc = projM.Data();
    const float* viewProjSrc = viewProjM.Data();
    for (int i = 0; i < 16; ++i)
    {
        cam.view[i] = viewSrc[i];
        cam.proj[i] = projSrc[i];
        cam.viewProj[i] = viewProjSrc[i];
    }
    cam.cameraPos[0] = eye.x;
    cam.cameraPos[1] = eye.y;
    cam.cameraPos[2] = eye.z;
    cam.cameraPos[3] = 0.0f;
    m_RenderServices->Views().SetCameraData(lane.cameraId, cam);
}

bool ModelThumbnailHandler::IsSpawnedModelGpuReady(const RenderLane& lane) const
{
    // True once at least one of the spawned model's submeshes has a live GPU
    // scene instance — i.e. the extraction has uploaded it and the world pass
    // would have something to draw. Mirrors the teardown's instanceIndex check.
    if (!lane.world || !lane.spawnedResult.IsValid())
        return false;
    for (const ECS::EntityHandle& entity : lane.spawnedResult.submeshEntities)
    {
        if (!entity.IsValid())
            continue;
        const auto* meshGpu = lane.world->GetComponent<Components::MeshGPUData>(entity);
        if (meshGpu && meshGpu->instanceIndex != 0xFFFFFFFFu)
            return true;
    }
    return false;
}

bool ModelThumbnailHandler::AreSpawnedModelPipelinesCompiling(const RenderLane& lane) const
{
    if (!lane.world || !m_RenderServices)
        return false;
    for (const ECS::EntityHandle& entity : lane.spawnedResult.submeshEntities)
    {
        if (!entity.IsValid())
            continue;
        const auto* mr = lane.world->GetComponent<Components::MeshRenderer>(entity);
        if (mr && !mr->materialAssetGuid.IsNull() && IsMaterialPipelineCompiling(mr->materialAssetGuid.ToGuid()))
            return true;
    }
    return false;
}

bool ModelThumbnailHandler::IsMaterialPipelineCompiling(const GUID& materialGuid) const
{
    if (!m_RenderServices)
        return false;
    auto& materials = m_RenderServices->Materials();
    if (materials.IsBaseCompileInFlight(materialGuid))
        return true;
    const Engine::Renderer::Material* material = materials.Registry().Find(materialGuid);
    return material && materials.Variants().HasPipelineBuildsInFlight(*material);
}

bool ModelThumbnailHandler::AreSpawnedModelTexturesReady(const RenderLane& lane) const
{
    if (!lane.world || !m_RenderServices)
        return true;
    for (const ECS::EntityHandle& entity : lane.spawnedResult.submeshEntities)
    {
        if (!entity.IsValid())
            continue;
        const auto* mr = lane.world->GetComponent<Components::MeshRenderer>(entity);
        if (!mr || mr->materialAssetGuid.IsNull())
            continue;
        if (!m_RenderServices->Textures().IsMaterialTextureBindingComplete(mr->materialAssetGuid.ToGuid()))
            return false;
    }
    return true;
}

void ModelThumbnailHandler::SubmitThumbnailRenderRG(WindowState& ws, RenderLane& lane, Slot& slot,
                                                    size_t slotIdx,
                                                    const GUID& guid,
                                                    Rendering::RenderGraph::RGFrame& frame)
{
    namespace RenderGraph = Rendering::RenderGraph;

    if (lane.viewId == 0 || !slot.deviceTex.IsValid())
        return;

    // The in-flight COMMIT happens at the very end, after every declination
    // point — an aborted submit leaves the slot bookkeeping untouched so the
    // request stays retryable (a committed-then-declined render would finalize
    // ready on a never-written texture).

    // Re-enqueue on abort: pendingSet dedups, and the focused-orbit path
    // re-dispatches on its own (needsOrbitRerender survives an abort).
    const auto deferAndRetry = [&]
    {
        EnqueuePending(ws, slot.guid, slot.listStaticThumb, slot.isMaterial,
                       slot.previewIblEnabled, slot.isLensFlare);
    };

    // Model readiness: the spawn is in the thumbnail world, but its GPU scene
    // instance (extraction), its pipelines (async compile) and its material
    // textures (async upload) can all lag the spawn. Rendering before they land
    // records an empty or untextured draw — a blank thumbnail we would bake as
    // "ready" and (for an unselected slot) never revisit. Defer + retry until
    // fully ready, bounded so a geometry-less model still resolves. Skip when
    // there are no submeshes to wait on.
    if (!slot.isMaterial && !slot.isLensFlare &&
        !lane.spawnedResult.submeshEntities.empty())
    {
        // Hold while this model's materials compile: extraction creates no
        // instance for a draw whose base pipeline is not published yet, and a
        // draw whose pass variant or backend pipeline is still building is
        // skipped. Other lanes' compiles do not hold this one. The hold restarts
        // the settle count, so every settle render lands after the publish, and
        // it spends no retry budget: a pending compile finishes, and the bounded
        // retries below exist for a model that will never draw.
        if (AreSpawnedModelPipelinesCompiling(lane))
        {
            lane.spawnedSettleFrames = 0;
            slot.bakeClockStart = std::chrono::steady_clock::now();
            deferAndRetry();
            return;
        }
        const bool gpuReady = IsSpawnedModelGpuReady(lane);
        if (gpuReady && AreSpawnedModelTexturesReady(lane))
        {
            slot.gpuNotReadyRetries = 0;
            slot.renderGaveUp = false;
            // Fully ready: count post-ready frames so the bucketer twin (whose
            // draws consume the prior frame's slot buffers) has a primed pipeline
            // before we advance to the next model. The drain keeps the model
            // spawned until this reaches the settle target.
            if (ThumbnailCachePolicy::IsSpawnSettling(guid, lane.spawnedModelGuid, lane.spawnedSettleFrames))
                ++lane.spawnedSettleFrames;
        }
        else
        {
            // Not ready: instance not extracted yet, or geometry is in but the
            // material textures are still uploading. These need different
            // bounds — a geometry-less model never extracts (cap tight so it
            // can't head-of-line-block), but a heavy model whose geometry IS in
            // and is only waiting on async texture binding WILL finish (cap
            // loose so we don't bake it blank — the last/heaviest-tile bug).
            const uint32_t maxRetries =
                gpuReady ? ModelThumbnailShared::kThumbnailTextureReadyMaxRetries : ModelThumbnailShared::kThumbnailGpuReadyMaxRetries;
            if (slot.gpuNotReadyRetries < maxRetries)
            {
                // Defer + restart the settle tail so we don't bake a
                // blank/untextured slot.
                ++slot.gpuNotReadyRetries;
                lane.spawnedSettleFrames = 0;
                deferAndRetry();
                return;
            }
            // Bound reached: render whatever is there rather than defer
            // forever. That frame does not show the model, so it is not cached.
            slot.renderGaveUp = true;
        }
    }

    // A material settles like a model: its first draw requests the pass
    // variant, and the draws consume the prior frame's slot buffers. Hold while
    // its pipelines compile (spending no budget: a pending compile finishes),
    // then count fully-ready renders; EnsureMaterialReady already waited for
    // its texture bindings.
    bool materialSettled = false;
    if (slot.isMaterial)
    {
        if (IsMaterialPipelineCompiling(guid))
        {
            lane.spawnedSettleFrames = 0;
            slot.bakeClockStart = std::chrono::steady_clock::now();
            deferAndRetry();
            return;
        }
        if (ThumbnailCachePolicy::IsSpawnSettling(guid, lane.spawnedMaterialGuid, lane.spawnedSettleFrames))
            ++lane.spawnedSettleFrames;
        materialSettled =
            !ThumbnailCachePolicy::IsSpawnSettling(guid, lane.spawnedMaterialGuid, lane.spawnedSettleFrames);
    }

    // Drive the LOD shown by the inspector's preview slider. Scoped to the
    // focused model preview slot: grid/list thumbnails and material previews
    // keep auto-selection so an inspector LOD pick can't leak into them.
    {
        const bool focusedModelPreview =
            !slot.isMaterial && !slot.isLensFlare && !slot.listStaticThumb &&
            slot.guid == s_OrbitFocusGuid;
        m_RenderServices->Views().SetViewForcedLOD(
            lane.viewId,
            focusedModelPreview ? s_PreviewForcedLOD : 0xFFFFFFFFu);
    }

    m_RenderServices->WriteViewLightBuffer(lane.viewId);
    m_RenderServices->BuildWorldBatchKeysForView(lane.viewId);

    // Bucketer twin BEFORE the world pass: recording order gives the
    // DrawStreamOrdering edge its direction (Read-before-Write derives WAR —
    // the draws would consume frame N−1's slot buffers). A refusal means the
    // draw streams could not be scheduled this frame (no spine on this frame
    // incarnation) — defer rather than bake a blank slot.
    if (!m_RenderServices->ScheduleBucketerDispatchesForView(frame, lane.viewId))
    {
        deferAndRetry();
        return;
    }

    // Clear config on the ViewDesc — AddWorldPassForView reads it at
    // declaration (transparent background; reverse-Z far depth clear).
    {
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearColorValue[0] = 0.0f;
        clear.clearColorValue[1] = 0.0f;
        clear.clearColorValue[2] = 0.0f;
        clear.clearColorValue[3] = 0.0f;
        clear.clearDepth = true;
        clear.clearDepthValue = 0.0f;
        m_RenderServices->Views().SetViewClearConfig(lane.viewId, clear);
    }

    // Per-dispatch intermediates sized to the slot. The WORLD-pass targets
    // are POOL imports, never transients: BuildPassResourcesRG resolves
    // PhysicalTexture(depth) for ge_sceneDepth at declaration, which asserts
    // on a transient (its documented constraint). They're re-imported every
    // dispatch frame, so pool aging never bites; idle sizes age out.
    const uint32_t w = slot.texWidth > 0 ? slot.texWidth
                                         : static_cast<uint32_t>(s_ThumbnailResolutionPx);
    const uint32_t h = slot.texHeight > 0 ? slot.texHeight
                                          : static_cast<uint32_t>(s_ThumbnailResolutionPx);
    const uint32_t samples = m_RenderServices->GetDefaultMSAASampleCount();
    const bool msaa = samples > 1u;

    Rendering::TextureDesc td{};
    td.width = w;
    td.height = h;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;

    RenderGraph::RGTexture msaaColor{};
    if (msaa)
    {
        const std::string msaaName = MakeSizedTargetName("Editor.ModelThumb.RG.ColorMSAA", w, h);
        td.sampleCount = samples;
        // Matches the HDR resolve format below — a format mismatch is an
        // invalid MSAA resolve (VUID-06865).
        td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget);
        td.debugName = msaaName.c_str();
        msaaColor = frame.ImportPersistentTexture(msaaName.c_str(), td);
    }

    const std::string depthName = MakeSizedTargetName("Editor.ModelThumb.RG.Depth", w, h);
    td.sampleCount = msaa ? samples : 1u;
    td.format = static_cast<uint32_t>(m_RenderServices->GetDepthFormat());
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
               static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = depthName.c_str();
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(depthName.c_str(), td);

    const std::string hdrName = MakeSizedTargetName("Editor.ModelThumb.RG.HDRColor", w, h);
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
               static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = hdrName.c_str();
    const RenderGraph::RGTexture hdrResolve = frame.ImportPersistentTexture(hdrName.c_str(), td);

    if (!depth.IsValid() || !hdrResolve.IsValid() || (msaa && !msaaColor.IsValid()))
    {
        deferAndRetry();
        return;
    }

    // Camera UBO content is read at world-pass declaration (AllocUpload by
    // value) — write it first.
    SetupCameraMatrices(lane, w, h);

    Engine::Renderer::RenderServices::WorldPassTargetsRG targets{};
    targets.Color = msaa ? msaaColor : hdrResolve;
    targets.Depth = depth;
    if (msaa)
        targets.Resolve = hdrResolve;
    // Instanced, optionally IBL, but never Shadows or ForwardPlus. IBL gives the preview
    // the shared environment (the lit "material ball" look — diffuse irradiance +
    // prefiltered specular reflections; the feature seeds an always-valid fallback cube,
    // so it never goes black). The analytic directional key light still owns the crisp
    // highlight. Shadows are skipped (keyword-less views bind the dummy shadow array,
    // no ImportShadowMapArrayRG). ForwardPlus is skipped on purpose: this view runs no
    // cluster cull, so clustered lighting would read an empty light list and the key
    // light would vanish.
    Rendering::MaterialKeyword previewKeywords = Rendering::MaterialKeyword::Instanced;
    const bool previewIblEnabled = slot.isLensFlare
        ? false
        : (slot.isMaterial ? slot.previewIblEnabled : GetPreviewIblEnabled());
    bool graphLivePreview = false;
    if (slot.isMaterial && m_AssetManager)
    {
        if (const SharedPtr<Asset> asset = m_AssetManager->GetAsset(guid))
            graphLivePreview = Engine::Renderer::IsGraphLivePreviewMaterialPath(asset->GetPath());
    }
    // Graph live-preview materials keep the analytic key at exposure 1.0 even
    // when the IBL toggle is on. Photographic IBL (EV ~14) crushes authored
    // emissive; IBL at unity exposure clips the sphere to white. Node plates
    // still light through GraphPreviewAtlas IBL.
    const bool usePhotographicIbl = previewIblEnabled && !graphLivePreview;
    // Lens flares are authored in display-referred brightness and compensate
    // for the effective view exposure inside LensFlareRenderNode. Give the
    // isolated preview an explicit simple exposure so that compensation and
    // the thumbnail tonemap agree; inheriting an EV-based world exposure here
    // can otherwise amplify the flare by many stops and clip its color detail.
    constexpr float kSimpleLensFlarePreviewExposure = 1.0f;
    if (slot.isLensFlare)
    {
        Engine::Renderer::PostProcessSettings flarePreviewSettings{};
        flarePreviewSettings.Exposure = kSimpleLensFlarePreviewExposure;
        flarePreviewSettings.AutoExposureActive = false;
        m_RenderServices->Views().SetViewPostProcessOverride(
            lane.viewId, flarePreviewSettings);
    }
    else
    {
        m_RenderServices->Views().ClearViewPostProcessOverride(lane.viewId);
    }
    if (usePhotographicIbl)
        previewKeywords = previewKeywords | Rendering::MaterialKeyword::IBL;
    const auto world = m_RenderServices->AddWorldPassForView(
        frame, lane.viewId, targets, previewKeywords);
    if (!world.Pass.IsValid())
    {
        deferAndRetry();
        return;
    }

    // The texture the world pass ACTUALLY wrote (resolve collapse aside) —
    // the explicit replacement for GetEffectiveWorldColorResolveRef.
    const RenderGraph::RGTexture encodeSrc =
        world.EffectiveColor.IsValid() ? world.EffectiveColor : hdrResolve;

    // Append the runtime lens-flare pass to the thumbnail view before
    // tonemapping. LensFlareExtractionSystem writes a shared render feature,
    // so preserve the scene frame's feature data around this isolated preview
    // declaration. DeclareForView uploads and captures its instances
    // immediately; restoring here cannot alter the queued preview draw.
    if (slot.isLensFlare && lane.world && m_LensFlareExtractionSystem)
    {
        auto& flareFeature =
            m_RenderServices->EnsureFeature<Engine::Renderer::LensFlareRenderFeature>();
        std::vector<Engine::Renderer::ResolvedFlare> savedFlares = flareFeature.GetFlares();
        const float savedTime = flareFeature.GetTime();

        m_LensFlareExtractionSystem->Update(*lane.world, 0.0f);
        if (const Rendering::ViewDesc* viewDesc =
                m_RenderServices->Views().FindViewDesc(lane.viewId))
        {
            Engine::Renderer::Pipeline::ViewDeclare declaration(
                frame, *viewDesc, *m_RenderServices, "Editor.LensFlarePreview");
            declaration.ViewColor = encodeSrc;
            declaration.ViewResolve = encodeSrc;
            declaration.ViewDepth = depth;
            declaration.ViewDepthResolved = depth;
            declaration.RenderWidth = w;
            declaration.RenderHeight = h;

            Engine::Renderer::Pipeline::Nodes::LensFlareRenderNode flareNode;
            std::string nodeError;
            if (flareNode.Initialize(
                    "Editor.LensFlarePreview", R"({"output":"View.Resolve"})", &nodeError))
            {
                flareNode.DeclareForView(declaration);
            }
            else
            {
                Logger::Log::Warning(
                    "Lens-flare preview node initialization failed: {}", nodeError);
            }
        }

        flareFeature.SetFlares(std::move(savedFlares));
        flareFeature.SetTime(savedTime);
    }

    // Tonemap (the TonemapParams default operator, outEncoding=1 — display-referred
    // LINEAR, no OETF) straight
    // into the slot's device texture. The editor's terminal FinalSRGBEncode pass
    // owns the ONLY OETF, so the thumbnail must hand the UI compositor LINEAR
    // color exactly like the SceneView viewport does — a per-thumbnail sRGB
    // encode here double-encoded against that terminal pass and washed previews
    // out. The disk-cache PNG writer applies its own linear->sRGB, so it too
    // wants the linear source.
    Rendering::Passes::TonemapParams tmParams{};
    // Lens-flare fragments use RGB-on-black atlases, so derive a straight-alpha
    // coverage from their final light and normalize the color for the HDR UI
    // compositor. Other thumbnails preserve their source alpha unchanged.
    tmParams.preserveAlpha = slot.isLensFlare ? 2 : 1;
    // Exposure depends on the light source. With IBL the preview is lit by the
    // physically-bright shared environment, so a fixed exposure of 1.0
    // (== reference white) blows the model out to white -- expose it at the
    // preview's own kThumbnailPreviewEv100 instead. With IBL OFF the model is lit
    // only by the preview key/ambient lights (~reference-white scale), where that
    // EV would crush them to black; keep 1.0 there.
    tmParams.exposure = slot.isLensFlare
        ? kSimpleLensFlarePreviewExposure
        : (usePhotographicIbl
            ? Rendering::EvToLinearExposure(kThumbnailPreviewEv100)
            : 1.0f);

    // Tonemap straight into the slot's visible texture.
    const std::string importName = std::string("Editor.ModelThumb.Slot") + std::to_string(slotIdx);
    const RenderGraph::RGTexture slotTex = frame.ImportExternalTexture(
        importName.c_str(), slot.deviceTex,
        slot.deviceTexInitialized ? Rendering::ResourceState::ShaderResource
                                  : Rendering::ResourceState::Undefined,
        Rendering::TextureFormat::R16G16B16A16_FLOAT);
    if (!slotTex.IsValid())
    {
        deferAndRetry();
        return;
    }

    const RenderGraph::RGPass tonemapPass = Rendering::Passes::AddTonemapPassRG(
        frame, encodeSrc, slotTex, tmParams, "Editor.ModelThumb.Tonemap");
    if (!tonemapPass.IsValid())
    {
        // Missing tonemap shaders (or src==dst): nothing wrote the visible
        // slot. Retry next frame rather than latching an uninitialized
        // texture.
        deferAndRetry();
        return;
    }
    frame.MarkOutput(slotTex, RenderGraph::RGImageLayout::ShaderReadOnly);
    slot.deviceTexInitialized = true;

    // COMMIT — every declination point is behind us; the finalize debounce
    // keeps ready false until the encode has executed on a submitted frame.
    slot.needsOrbitRerender = false;
    if (slot.isMaterial)
        slot.materialRenderSettled = materialSettled;
    slot.inFlight = true;
    lane.inFlightSlot = slotIdx;
    lane.inFlightFrame = ws.frameCounter;
}

void ModelThumbnailHandler::TeardownSpawnedModel(RenderLane& lane)
{
    // Any path that drops the spawned model restarts the settle window; the next
    // model to spawn must earn its own warmup frames, never inherit a stale count.
    lane.spawnedSettleFrames = 0;
    lane.spawnedSlot = kMaxResidentSlots;

    if (!lane.spawnedResult.IsValid())
    {
        lane.spawnedModelGuid = GUID{};
        lane.spawnedResult = {};
        return;
    }

    // Free GPUScene instances for every spawned entity before destroying them.
    Rendering::GPUScene* scene = m_RenderServices ? m_RenderServices->GetGPUScene() : nullptr;
    if (scene && lane.world)
    {
        for (const auto& entity : lane.spawnedResult.submeshEntities)
        {
            if (!entity.IsValid())
                continue;
            auto* meshGpu = lane.world->GetComponentForWrite<Components::MeshGPUData>(entity);
            if (meshGpu && meshGpu->instanceIndex != 0xFFFFFFFFu)
            {
                scene->RemoveInstance(meshGpu->instanceIndex);
                meshGpu->instanceIndex = 0xFFFFFFFFu;
            }
        }
    }

    lane.spawnedModelGuid = GUID{};
    lane.spawnedResult = {};
}

void ModelThumbnailHandler::TeardownSpawnedMaterial(RenderLane& lane)
{
    if (!lane.isSpawnedMaterial || !lane.primitiveEntity.IsValid())
    {
        lane.isSpawnedMaterial = false;
        lane.spawnedMaterialGuid = GUID{};
        lane.spawnedMaterialShapeGuid = GUID{};
        lane.primitiveEntity = ECS::EntityHandle::Invalid();
        return;
    }

    Rendering::GPUScene* scene = m_RenderServices ? m_RenderServices->GetGPUScene() : nullptr;
    if (scene && lane.world)
    {
        auto* meshGpu = lane.world->GetComponentForWrite<Components::MeshGPUData>(lane.primitiveEntity);
        if (meshGpu && meshGpu->instanceIndex != 0xFFFFFFFFu)
        {
            scene->RemoveInstance(meshGpu->instanceIndex);
            meshGpu->instanceIndex = 0xFFFFFFFFu;
        }
    }

    lane.isSpawnedMaterial = false;
    lane.spawnedMaterialGuid = GUID{};
    lane.spawnedMaterialShapeGuid = GUID{};
    lane.primitiveEntity = ECS::EntityHandle::Invalid();
}

void ModelThumbnailHandler::SpawnOrUpdateLensFlareEntity(RenderLane& lane,
                                                          const GUID& flareGuid,
                                                          float orbitYaw)
{
    if (!lane.world)
    {
        lane.world = ModelThumbnailShared::MakeThumbnailWorld();
    }

    ECS::World& thumbWorld = *lane.world;
    const bool needsSpawn = !lane.isSpawnedLensFlare ||
                            lane.spawnedLensFlareGuid != flareGuid ||
                            !lane.lensFlareEntity.IsValid();
    if (needsSpawn)
    {
        TeardownSpawnedModel(lane);
        TeardownSpawnedMaterial(lane);
        thumbWorld.Clear();
        lane.orbitRootEntity = ECS::EntityHandle::Invalid();
        lane.keyLightEntity = ECS::EntityHandle::Invalid();
        lane.ambientLightEntity = ECS::EntityHandle::Invalid();

        lane.lensFlareEntity = thumbWorld.CreateEntity();

        Components::LensFlareSource source{};
        source.Flare.Set(flareGuid);
        // Presets are authored as additive layers for an HDR scene. A neutral
        // preview has no scene luminance behind it, so use a display-friendly
        // source level that preserves color and structure in the generated
        // runtime thumbnail.
        source.Intensity = 0.8f;
        source.Occlude = false;
        source.SunMode = false;
        thumbWorld.AddComponentImmediate(lane.lensFlareEntity, source);
        thumbWorld.AddComponentImmediate(lane.lensFlareEntity,
                                         Components::WorldTransform{});

        lane.isSpawnedLensFlare = true;
        lane.spawnedLensFlareGuid = flareGuid;
    }

    // Place the source slightly left and above center so the authored optical
    // trail is visible instead of collapsing onto the source.  Point the
    // source's forward axis at the preview camera so angle-limited presets
    // retain their intended brightness.
    constexpr float baseSourceX = -0.32f;
    constexpr float sourceY = 0.075f;
    constexpr float baseSourceZ = 0.106f;
    constexpr float eyeX = 0.9f;
    constexpr float eyeY = 0.9f;
    constexpr float eyeZ = 1.8f;
    const float cosYaw = std::cos(orbitYaw);
    const float sinYaw = std::sin(orbitYaw);
    const float sourceX = baseSourceX * cosYaw + baseSourceZ * sinYaw;
    const float sourceZ = -baseSourceX * sinYaw + baseSourceZ * cosYaw;
    const float dx = eyeX - sourceX;
    const float dy = eyeY - sourceY;
    const float dz = eyeZ - sourceZ;
    const float invLength = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);

    auto* worldTransform =
        thumbWorld.GetComponentForWrite<Components::WorldTransform>(lane.lensFlareEntity);
    if (!worldTransform)
        return;
    worldTransform->matrix[8] = dx * invLength;
    worldTransform->matrix[9] = dy * invLength;
    worldTransform->matrix[10] = dz * invLength;
    worldTransform->matrix[12] = sourceX;
    worldTransform->matrix[13] = sourceY;
    worldTransform->matrix[14] = sourceZ;
    ++worldTransform->Version;
}

void ModelThumbnailHandler::SpawnOrUpdateMaterialEntity(RenderLane& lane,
                                                         const GUID& materialGuid,
                                                         const Mathematics::Matrix4x4& orbitMatrix)
{
    if (!lane.world)
    {
        lane.world = ModelThumbnailShared::MakeThumbnailWorld();
    }
    ECS::World& thumbWorld = *lane.world;

    if (lane.isSpawnedLensFlare)
    {
        thumbWorld.Clear();
        lane.isSpawnedLensFlare = false;
        lane.spawnedLensFlareGuid = GUID{};
        lane.lensFlareEntity = ECS::EntityHandle::Invalid();
        lane.orbitRootEntity = ECS::EntityHandle::Invalid();
        lane.keyLightEntity = ECS::EntityHandle::Invalid();
        lane.ambientLightEntity = ECS::EntityHandle::Invalid();
    }

    const GUID shapeGuid = [&]() -> GUID {
        switch (s_MaterialPreviewShape)
        {
            case PreviewShape::Sphere:  return Engine::Renderer::PrimitiveGenerator::SphereGuid();
            case PreviewShape::Cube:    return Engine::Renderer::PrimitiveGenerator::CubeGuid();
            case PreviewShape::Plane:   return Engine::Renderer::PrimitiveGenerator::PlaneGuid();
        }
        return Engine::Renderer::PrimitiveGenerator::SphereGuid();
    }();

    const bool sameSetup = lane.isSpawnedMaterial &&
                           lane.spawnedMaterialGuid == materialGuid &&
                           lane.spawnedMaterialShapeGuid == shapeGuid;

    if (!sameSetup)
    {
        // Clear any previously spawned entity (model or material).
        TeardownSpawnedModel(lane);
        TeardownSpawnedMaterial(lane);
        thumbWorld.Clear();
        lane.orbitRootEntity = ECS::EntityHandle::Invalid();
        lane.keyLightEntity = ECS::EntityHandle::Invalid();
        lane.ambientLightEntity = ECS::EntityHandle::Invalid();

        // Lights.
        {
            lane.keyLightEntity = thumbWorld.CreateEntity();
            Components::Light keyLight{};
            keyLight.Type = Components::LightType::Directional;
            keyLight.Color[0] = keyLight.Color[1] = keyLight.Color[2] = 1.0f;
            keyLight.Intensity = 1.0f;
            keyLight.CastsShadows = false;
            thumbWorld.AddComponentImmediate(lane.keyLightEntity, keyLight);
            thumbWorld.AddComponentImmediate(lane.keyLightEntity, Components::WorldTransform{});

            lane.ambientLightEntity = thumbWorld.CreateEntity();
            Components::Light ambient{};
            ambient.Type = Components::LightType::Ambient;
            ambient.Color[0] = ambient.Color[1] = ambient.Color[2] = 1.0f;
            ambient.Intensity = 0.05f;
            ambient.CastsShadows = false;
            thumbWorld.AddComponentImmediate(lane.ambientLightEntity, ambient);
            thumbWorld.AddComponentImmediate(lane.ambientLightEntity, Components::WorldTransform{});
        }

        // Orbit root entity carries the framing transform.
        lane.orbitRootEntity = thumbWorld.CreateEntity();
        Components::Transform orbitXf{};
        std::memcpy(orbitXf.matrix, orbitMatrix.Data(), sizeof(orbitXf.matrix));
        thumbWorld.AddComponentImmediate(lane.orbitRootEntity, orbitXf);

        // Primitive entity.
        lane.primitiveEntity = thumbWorld.CreateEntity();
        Components::MeshRenderer mr =
            Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(
                m_RenderServices, shapeGuid, materialGuid);
        mr.renderLayerMask = 1u;
        mr.castShadows = false;
        mr.receiveShadows = false;
        thumbWorld.AddComponentImmediate(lane.primitiveEntity, mr);
        auto localBounds = Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(m_RenderServices, shapeGuid);
        localBounds.CastShadows = false;
        thumbWorld.AddComponentImmediate(lane.primitiveEntity, localBounds);

        // Local identity transform — orbit root drives world position.
        Components::Transform localXf{};
        localXf.matrix[0] = localXf.matrix[5] = localXf.matrix[10] = localXf.matrix[15] = 1.0f;
        thumbWorld.AddComponentImmediate(lane.primitiveEntity, localXf);

        Components::Parent parentComp{};
        parentComp.parent = lane.orbitRootEntity;
        thumbWorld.AddComponentImmediate(lane.primitiveEntity, parentComp);

        lane.isSpawnedMaterial = true;
        lane.spawnedMaterialGuid = materialGuid;
        lane.spawnedMaterialShapeGuid = shapeGuid;
    }
    else
    {
        // Same material and shape — update orbit root transform for rotation.
        if (lane.orbitRootEntity.IsValid())
        {
            auto* orbitXf = thumbWorld.GetComponentForWrite<Components::Transform>(lane.orbitRootEntity);
            if (orbitXf)
            {
                std::memcpy(orbitXf->matrix, orbitMatrix.Data(), sizeof(orbitXf->matrix));
            }
        }
    }
}

bool ModelThumbnailHandler::EnsureMaterialReady(WindowState& ws, const GUID& guid,
                                                const Slot& slot)
{
    if (!m_AssetManager || !m_RenderServices)
        return false;

    if (m_AssetManager->IsLoadSuppressed(guid))
        return false;

    SharedPtr<Asset> asset = m_AssetManager->GetAsset(guid);
    if (!asset)
    {
        m_AssetManager->LoadAsset(guid, AssetLoadResultCallback{});
        EnqueuePending(ws, guid, false, true, slot.previewIblEnabled);
        return false;
    }

    // Once the asset is loaded, register it into MaterialRegistry so the
    // GPU pipeline is compiled and the material can be rendered.
    if (!m_RenderServices->Materials().Registry().Find(guid))
    {
        if (auto* matAsset = dynamic_cast<MaterialAsset*>(asset.get()))
        {
            if (matAsset->IsLoaded())
            {
                Logger::Log::Trace("MaterialThumb: registering material guid={}", guid.ToCompactString());
                auto* registered = m_RenderServices->Materials().RegisterMaterialFromDocument(guid, matAsset->GetDocument());
                if (!registered)
                    Logger::Log::Warning("MaterialThumb: RegisterMaterialFromDocument returned null for guid={}", guid.ToCompactString());
            }
            else
            {
                Logger::Log::Trace("MaterialThumb: asset not yet loaded, state={} guid={}", (int)matAsset->GetState(), guid.ToCompactString());
                EnqueuePending(ws, guid, false, true, slot.previewIblEnabled);
                return false;
            }
        }
        else
        {
            Logger::Log::Warning("MaterialThumb: asset is not a MaterialAsset, guid={}", guid.ToCompactString());
            return false;
        }
    }

    // Material is ready once it appears in the MaterialRegistry (compiled + uploaded).
    auto* mat = m_RenderServices->Materials().Registry().Find(guid);
    if (!mat)
    {
        EnqueuePending(ws, guid, false, true, slot.previewIblEnabled);
        return false;
    }

    // Defer until the base pipeline compile has published: registration kicks
    // the shader compile async, and a render before it lands draws nothing --
    // the slot would be marked ready holding a blank texture forever.
    if (!mat->GetGraphicsPipelineId().IsValid())
    {
        EnqueuePending(ws, guid, false, true, slot.previewIblEnabled);
        return false;
    }

    // Defer the render until every async texture load has uploaded and bound.
    // Otherwise the first render uses the material's bindless white defaults
    // and the user sees a textureless thumbnail until something dirties the
    // slot. The slot stays in the pending queue and re-checks next frame.
    if (!m_RenderServices->Textures().IsMaterialTextureBindingComplete(guid))
    {
        EnqueuePending(ws, guid, false, true, slot.previewIblEnabled);
        return false;
    }

    return true;
}

} // namespace GameEngine
