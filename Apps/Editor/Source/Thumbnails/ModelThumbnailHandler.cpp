#include "Thumbnails/ModelThumbnailHandler.h"
#include "Thumbnails/ModelThumbnailHandlerShared.h"

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/LensFlareDefinitionAsset.h"
#include "Rendering/Passes/TonemapPass.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "ECSModules/Rendering/Systems/SkinningUploadSystem.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Engine/Rendering/Systems/LensFlareExtractionSystem.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Assets/AnimationClip.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/UIManager.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <system_error>

namespace GameEngine
{

namespace
{
constexpr const char* kEngineListThumbPrefix = "editor_model_thumb_list_";
constexpr const char* kEngineThumbPrefix = "editor_model_thumb_";
constexpr const char* kEngineMaterialThumbPrefix = "editor_material_thumb_";
constexpr const char* kEngineLensFlareThumbPrefix = "editor_lensflare_thumb_";
constexpr const char* kEngineMaterialNoIblSuffix = "_noibl";
constexpr float kTwoPi = 6.28318530718f;

// Warms the material's base shader variant so a later synchronous compile on the
// main thread is a cache hit. Main thread only: the prewarm reads MaterialSystem's
// build context.
void PrewarmMaterialBaseShader(Engine::Renderer::RenderServices& services, Asset& asset)
{
    auto* matAsset = dynamic_cast<MaterialAsset*>(&asset);
    if (!matAsset || !matAsset->IsLoaded())
        return;
    services.Materials().PrewarmMaterialBaseShader(matAsset->GetDocument(), matAsset->GetPath());
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ModelThumbnailHandler::ModelThumbnailHandler(AssetManager* assetManager,
                                             Engine::Renderer::RenderServices* renderServices)
    : m_AssetManager(assetManager), m_RenderServices(renderServices),
      m_AnimationSystem(std::make_unique<Engine::Renderer::AnimationSystem>(renderServices)),
      m_HumanoidRetargetSystem(std::make_unique<Engine::Renderer::HumanoidRetargetSystem>(renderServices)),
      m_SkinningUploadSystem(std::make_unique<Engine::Renderer::SkinningUploadSystem>(renderServices)),
      m_TransformSystem(std::make_unique<Engine::Renderer::TransformHierarchySystem>()),
      m_ExtractionSystem(std::make_unique<Engine::Renderer::RenderExtractionSystem>(renderServices)),
      m_LensFlareExtractionSystem(
          std::make_unique<Engine::Renderer::LensFlareExtractionSystem>(renderServices))
{
    // Each resident slot can bind one UI texture, and the browser's PNG tiles,
    // icons and glyph pages share the same registry: the slots take at most
    // half of its default limit (a device can clamp the limit lower).
    static_assert(kMaxResidentSlots <= UI::UITextureRegistry::kDefaultMaxTextures / 2);
    s_Instance = this;
    if (m_RenderServices)
    {
        /* Buffer only — drained by BeginFrame. A thumbnail rendered while a
           pass variant was still compiling drew nothing (the publish-gate
           skips such draws) and cached the blank as ready; the drain in
           BeginFrame re-renders those slots when their variants land. */
        m_VariantPublishedSub =
            m_RenderServices->Materials().Variants().VariantPublished.Subscribe(
                [](const GUID& guid)
                {
                    std::lock_guard lock(s_PublishedVariantGuidsMutex);
                    s_PublishedVariantGuids.insert(guid);
                });
    }
    // Mark thumbnails dirty on model hot-reload / file change.
    if (m_AssetManager)
    {
        auto& dispatcher = m_AssetManager->GetEventDispatcher();
        m_AssetEventCallbackHandle = dispatcher.AddCallback([this](const AssetEvent& ev)
                                                            {
                if (ev.Type != AssetType::Model &&
                    ev.Type != AssetType::LensFlareDefinition &&
                    ev.Type != AssetType::Material)
                {
                    return;
                }

                const GUID& guid = ev.AssetGuid;
                if (guid.IsNull())
                {
                    return;
                }

                if (ev.EventType != AssetEventType::AssetReloaded && ev.EventType != AssetEventType::AssetModified)
                {
                    return;
                }

                m_DiskCacheRequested.erase(guid);
                if (m_PendingDiskCache && m_PendingDiskCache->guid == guid)
                    m_PendingDiskCache.reset();

                // A material's slots re-render on the lanes' primitive spawns.
                if (ev.Type == AssetType::Material)
                {
                    InvalidateMaterialThumbnail(guid, /*propertyValuesOnly=*/true);
                    return;
                }

                // Re-render every resident slot for this model (rotating preview and static list copies).
                for (auto& winPair : m_Windows)
                {
                    WindowState& ws = winPair.second;
                    for (size_t slotIdx = 0; slotIdx < ws.slots.size(); ++slotIdx)
                    {
                        Slot& slot = ws.slots[slotIdx];
                        if (!slot.occupied || slot.guid != guid)
                            continue;

                        const bool wasReady = slot.ready;
                        slot.ready = false;
                        slot.inFlight = false;
                        if (wasReady)
                            slot.needsUiDetachBeforeRender = true;
                        slot.framingBoundsValid = false;
                        slot.animClip.reset();
                        slot.animAvailableIndices.clear();
                        slot.animSelectedPos = -1;
                        slot.animLoadedPos = -1;
                        slot.cachedClipPos = -1;
                        slot.cachedClipIndex = 0;
                        slot.usingExternalPreviewClip = false;
                        for (RenderLane& lane : ws.lanes)
                        {
                            // Force the lane to re-spawn on its next dispatch.
                            // Clear only the GUID, NOT spawnedResult: the next
                            // dispatch's TeardownSpawnedModel needs the still-valid
                            // result to free the old entities' GPUScene instances.
                            // Dropping it here orphans them, leaving a frozen stale
                            // copy alongside the fresh spawn (double model on reload).
                            if (slot.isLensFlare && lane.spawnedLensFlareGuid == guid)
                            {
                                lane.spawnedLensFlareGuid = GUID{};
                                lane.isSpawnedLensFlare = false;
                            }
                            else if (!slot.isLensFlare && lane.spawnedModelGuid == guid)
                            {
                                lane.spawnedModelGuid = GUID{};
                            }
                            if (lane.inFlightSlot == slotIdx)
                            {
                                lane.inFlightSlot = kMaxResidentSlots;
                                lane.inFlightFrame = 0;
                            }
                        }

                        EnqueuePending(ws, guid, slot.listStaticThumb, slot.isMaterial,
                                       slot.previewIblEnabled, slot.isLensFlare);
                    }
                } });
    }
}

ModelThumbnailHandler::~ModelThumbnailHandler()
{
    // Handler-owned RenderGraph slot textures die with the handler (the device defers
    // the actual destroy behind the graphics timeline).
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    for (auto& [windowId, ws] : m_Windows)
    {
        if (!device)
            break;
        for (Slot& slot : ws.slots)
        {
            if (slot.deviceTex.IsValid())
            {
                device->DestroyTexture(slot.deviceTex);
                slot.deviceTex = {};
            }
        }
    }

    if (s_Instance == this)
        s_Instance = nullptr;
    if (m_AssetManager && m_AssetEventCallbackHandle != 0)
    {
        m_AssetManager->GetEventDispatcher().RemoveCallback(m_AssetEventCallbackHandle);
        m_AssetEventCallbackHandle = 0;
    }
}

// ---------------------------------------------------------------------------
// Static UI/orbit control methods (moved from header)
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::BeginFrame()
{
    // Every frame, whether or not a window ticks thumbnails: a browser that
    // shows only baked tiles never creates a window, and the queue would grow
    // by one model per tile bind.
    if (s_Instance)
        s_Instance->ReleaseGridSlotsServedByPng();
    s_MaterialPrewarmedThisFrame.clear();
    s_AngleAdvancedThisFrame = false;

    {
        std::unordered_set<GUID> published;
        {
            std::lock_guard lock(s_PublishedVariantGuidsMutex);
            published.swap(s_PublishedVariantGuids);
        }
        // Already unique: the buffer dedupes on insert.
        for (const GUID& guid : published)
            InvalidateMaterialThumbnail(guid, /*propertyValuesOnly=*/true);
    }

    // Measure the real frame interval and express it as a multiple of a 60 FPS
    // reference frame, so per-frame preview advances stay constant in wall-clock
    // time across render rates. Clamp the catch-up after a stall so resuming
    // doesn't produce a visible jump.
    {
        static std::chrono::steady_clock::time_point s_LastFrameTime{};
        static bool s_HasLastFrameTime = false;
        const auto now = std::chrono::steady_clock::now();
        if (s_HasLastFrameTime)
        {
            constexpr float kReferenceFps = 60.0f;
            constexpr float kMaxFrameScale = 4.0f;
            const float dtSeconds = std::chrono::duration<float>(now - s_LastFrameTime).count();
            s_PreviewFrameScale = std::clamp(dtSeconds * kReferenceFps, 0.0f, kMaxFrameScale);
        }
        else
        {
            s_PreviewFrameScale = 1.0f;
            s_HasLastFrameTime = true;
        }
        s_LastFrameTime = now;
    }

    const float delta = s_OrbitTargetVelocityY - s_OrbitVelocityY;
    if (std::fabs(delta) <= 1e-5f)
    {
        s_OrbitVelocityY = s_OrbitTargetVelocityY;
    }
    else
    {
        s_OrbitVelocityY += delta * kOrbitVelocityEaseLerp;
    }

    // Material shape change: mark all material slots dirty and re-enqueue.
    if (s_MaterialShapeChanged && s_Instance)
    {
        s_MaterialShapeChanged = false;
        for (auto& [windowId, ws] : s_Instance->m_Windows)
        {
            (void)windowId;
            for (RenderLane& lane : ws.lanes)
            {
                lane.isSpawnedMaterial = false;
                lane.spawnedMaterialGuid = GUID{};
            }
            for (Slot& slot : ws.slots)
            {
                if (!slot.occupied || !slot.isMaterial)
                    continue;
                const bool wasReady = slot.ready;
                slot.ready = false;
                slot.inFlight = false;
                if (wasReady)
                    slot.needsUiDetachBeforeRender = true;
                EnqueuePending(ws, slot.guid, false, true, slot.previewIblEnabled);
            }
        }
    }
}

void ModelThumbnailHandler::NoteAssetListScrollActivity()
{
    // One gesture is many events; each pushes the window out so the hold lasts
    // as long as the scroll does, plus the settle time.
    constexpr auto kScrollSettle = std::chrono::milliseconds(250);
    const auto deadline = std::chrono::steady_clock::now() + kScrollSettle;
    if (deadline > s_ListRenderHeldUntil)
        s_ListRenderHeldUntil = deadline;
    ++s_ScrollGeneration;
}

bool ModelThumbnailHandler::IsAssetBrowserScrolling()
{
    return std::chrono::steady_clock::now() < s_ListRenderHeldUntil;
}

void ModelThumbnailHandler::SetRotatePreviewsEnabled(bool enabled)
{
    s_RotatePreviewsEnabled = enabled;
    if (enabled)
    {
        const float resumeVelocity =
            (std::fabs(s_LastNonZeroOrbitVelocityY) > 1e-4f) ? s_LastNonZeroOrbitVelocityY : kDefaultOrbitVelocityY;
        s_OrbitVelocityY = resumeVelocity;
        s_OrbitTargetVelocityY = resumeVelocity;
    }
    else
    {
        s_OrbitVelocityY = 0.0f;
        s_OrbitTargetVelocityY = 0.0f;
        s_ClickPaused = false;
    }
}

void ModelThumbnailHandler::SetOrbitFocusFromEngineName(const std::string& engineName)
{
    GUID guid;
    bool listStatic = false;
    if (TryParseGuidFromEngineName(engineName, guid, listStatic) && !listStatic)
        s_OrbitFocusGuid = guid;
    ResetFocusedPreviewZoom();
}

void ModelThumbnailHandler::SetOrbitVelocity(float velY)
{
    s_OrbitVelocityY = velY;
    s_OrbitTargetVelocityY = velY;
    if (std::fabs(velY) > 1e-4f)
    {
        s_LastNonZeroOrbitVelocityY = velY;
        s_ClickPaused = false;
    }
}

void ModelThumbnailHandler::EaseToOrbitVelocity(float velY)
{
    s_OrbitTargetVelocityY = velY;
    if (std::fabs(velY) > 1e-4f)
    {
        s_LastNonZeroOrbitVelocityY = velY;
        s_ClickPaused = false;
    }
}

void ModelThumbnailHandler::PauseClickRotation()
{
    if (s_ClickPaused)
        return;

    s_ClickPaused = true;
    if (std::fabs(s_OrbitVelocityY) > 1e-4f)
        s_LastNonZeroOrbitVelocityY = s_OrbitVelocityY;
    s_OrbitVelocityY = 0.0f;
    s_OrbitTargetVelocityY = 0.0f;
}

void ModelThumbnailHandler::SetPreviewLightIntensity(float value)
{
    s_PreviewLightIntensity = std::max(0.1f, value);
}

void ModelThumbnailHandler::SetPreviewAmbientIntensity(float value)
{
    s_PreviewAmbientIntensity = std::max(0.0f, value);
}

void ModelThumbnailHandler::SetPreviewIblEnabled(bool enabled)
{
    if (s_PreviewIblEnabled == enabled)
        return;

    s_PreviewIblEnabled = enabled;
    if (!s_Instance)
        return;

    s_Instance->m_DiskCacheRequested.clear();

    for (auto& [windowId, ws] : s_Instance->m_Windows)
    {
        (void)windowId;
        for (RenderLane& lane : ws.lanes)
        {
            lane.isSpawnedMaterial = false;
            lane.spawnedMaterialGuid = GUID{};
        }

        for (Slot& slot : ws.slots)
        {
            // A baked tile shows its PNG, as every later session does.
            if (!slot.occupied || slot.showsBakedImage)
                continue;

            const bool wasReady = slot.ready;
            slot.ready = false;
            slot.inFlight = false;
            slot.needsOrbitRerender = true;
            slot.needsUiDetachBeforeRender = slot.needsUiDetachBeforeRender || wasReady;
            EnqueuePending(ws, slot.guid, slot.listStaticThumb, slot.isMaterial,
                           slot.previewIblEnabled, slot.isLensFlare);
        }
    }
}

void ModelThumbnailHandler::SetOrbitSpeedScale(float scale)
{
    const float clamped = std::clamp(scale, 0.1f, 4.0f);
    if (std::fabs(clamped - s_OrbitSpeedScale) < 1e-5f)
        return;

    // Adjust the live orbit velocity so slider changes take effect immediately.
    if (!s_ClickPaused && std::fabs(s_OrbitVelocityY) > 1e-5f)
    {
        const float prevScale = std::max(0.1f, s_OrbitSpeedScale);
        const float ratio = clamped / prevScale;
        s_OrbitVelocityY *= ratio;
        s_OrbitTargetVelocityY *= ratio;
        s_LastNonZeroOrbitVelocityY *= ratio;
    }
    s_OrbitSpeedScale = clamped;
}

// ---------------------------------------------------------------------------
// Animation helpers (static)
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::SetPreviewForcedLOD(uint32_t lod)
{
    if (s_PreviewForcedLOD == lod)
        return;
    s_PreviewForcedLOD = lod;

    // Force a one-shot re-render of the focused model preview so the new LOD is
    // visible immediately. The per-frame dispatch only re-renders the focused
    // slot while it's orbit-rotating or animating; when rotation is paused on a
    // static model, nothing would otherwise pick up the changed LOD.
    if (!s_Instance || s_OrbitFocusGuid.IsNull())
        return;
    ModelThumbnailHandler* self = s_Instance;
    const GUID focusedGuid = s_OrbitFocusGuid;
    for (auto& [windowId, ws] : self->m_Windows)
    {
        (void)windowId;
        auto itSlot = ws.guidToSlot.find(focusedGuid);
        if (itSlot == ws.guidToSlot.end() || itSlot->second >= ws.slots.size())
            continue;
        Slot& slot = ws.slots[itSlot->second];
        if (!slot.occupied)
            continue;
        // needsOrbitRerender lets DrainNextPendingSlot re-render an already-ready
        // slot; EnqueuePending schedules it for the next dispatch.
        slot.needsOrbitRerender = true;
        EnqueuePending(ws, focusedGuid, false);
    }
}

void ModelThumbnailHandler::AdjustFocusedPreviewZoom(float scrollY)
{
    if (std::fabs(scrollY) < 1e-3f)
        return;

    // Multiplicative dolly: negative scrollY zooms in (larger framing), matching
    // SceneViewController::UpdateDolly's sign convention.
    const float notches = std::clamp(scrollY, -kPreviewZoomMaxNotchesPerEvent, kPreviewZoomMaxNotchesPerEvent);
    const float factor = std::exp(-notches * kPreviewZoomScrollSpeed);
    const float next = std::clamp(s_PreviewZoomScale * factor, kPreviewZoomMin, kPreviewZoomMax);
    if (std::fabs(next - s_PreviewZoomScale) < 1e-5f)
        return;
    s_PreviewZoomScale = next;

    if (!s_Instance)
        return;

    auto markFocused = [](const GUID& focusedGuid)
    {
        if (focusedGuid.IsNull() || !s_Instance)
            return;
        for (auto& [windowId, ws] : s_Instance->m_Windows)
        {
            (void)windowId;
            auto itSlot = ws.guidToSlot.find(focusedGuid);
            if (itSlot == ws.guidToSlot.end() || itSlot->second >= ws.slots.size())
                continue;
            Slot& slot = ws.slots[itSlot->second];
            if (!slot.occupied)
                continue;
            slot.needsOrbitRerender = true;
            EnqueuePending(ws, focusedGuid, false);
        }
    };

    markFocused(s_OrbitFocusGuid);
    markFocused(s_MaterialOrbitFocusGuid);
}

void ModelThumbnailHandler::ResetFocusedPreviewZoom()
{
    s_PreviewZoomScale = 1.0f;
    s_LastAnimationStepTime = {};
    s_LastAnimationAttemptTime = {};
}

void ModelThumbnailHandler::StepFocusedAnimation(float deltaSteps)
{
    if (!s_Instance)
        return;
    if (s_OrbitFocusGuid.IsNull())
        return;

    const auto now = std::chrono::steady_clock::now();
    int intervalMs = kAnimationStepMinIntervalMs;
    if (s_LastAnimationAttemptTime.time_since_epoch().count() != 0)
    {
        const auto sinceAttemptMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - s_LastAnimationAttemptTime)
                                        .count();
        // Rapid successive calls (fast wheel) pull the gate toward the fast floor.
        if (sinceAttemptMs < kAnimationStepBurstWindowMs)
        {
            const float t = static_cast<float>(sinceAttemptMs) /
                            static_cast<float>(kAnimationStepBurstWindowMs);
            intervalMs = static_cast<int>(
                std::lround(static_cast<float>(kAnimationStepFastIntervalMs) * (1.0f - t) +
                            static_cast<float>(kAnimationStepMinIntervalMs) * t));
        }
    }
    s_LastAnimationAttemptTime = now;

    if (s_LastAnimationStepTime.time_since_epoch().count() != 0)
    {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - s_LastAnimationStepTime)
                                   .count();
        if (elapsedMs < intervalMs)
            return;
    }
    s_LastAnimationStepTime = now;

    ModelThumbnailHandler* self = s_Instance;
    const GUID focusedGuid = s_OrbitFocusGuid;

    for (auto& [windowId, ws] : self->m_Windows)
    {
        auto itSlot = ws.guidToSlot.find(focusedGuid);
        if (itSlot == ws.guidToSlot.end())
            continue;

        const size_t slotIdx = itSlot->second;
        if (slotIdx >= ws.slots.size())
            continue;

        Slot& slot = ws.slots[slotIdx];
        if (!slot.occupied)
            continue;

        // Step selection across discovered animation positions. If we haven't discovered
        // them yet, just adjust animSelectedPos as a position; TickRender will clamp
        // and wrap once the list is available.
        const int dir = (deltaSteps > 0.0f) ? 1 : -1;
        if (!slot.animAvailableIndices.empty())
        {
            const int count = static_cast<int>(slot.animAvailableIndices.size());
            int curPos = (slot.animSelectedPos < 0) ? 0 : slot.animSelectedPos;
            int newPos = ((curPos + dir) % count + count) % count;
            slot.animSelectedPos = newPos;
        }
        else
        {
            // No list yet; treat animSelectedPos as a position and nudge it.
            slot.animSelectedPos += dir;
        }
        // Invalidate loaded clip so TickRender reloads with the new selection.
        slot.animLoadedPos = -1;
        slot.cachedClipPos = -1;
        slot.animTime = 0.0f;

        // Force a re-render of this slot so the new pose becomes visible.
        slot.needsOrbitRerender = true;
        EnqueuePending(ws, focusedGuid, false);
    }
}

std::string ModelThumbnailHandler::GetFocusedAnimationLabel()
{
    if (!s_Instance)
        return {};
    if (s_OrbitFocusGuid.IsNull())
        return {};

    ModelThumbnailHandler* self = s_Instance;
    const GUID focusedGuid = s_OrbitFocusGuid;

    for (auto& [windowId, ws] : self->m_Windows)
    {
        auto itSlot = ws.guidToSlot.find(focusedGuid);
        if (itSlot == ws.guidToSlot.end())
            continue;

        const size_t slotIdx = itSlot->second;
        if (slotIdx >= ws.slots.size())
            continue;

        Slot& slot = ws.slots[slotIdx];
        if (slot.animAvailableIndices.empty())
            return {};

        const int count = static_cast<int>(slot.animAvailableIndices.size());
        int curPos = (slot.animSelectedPos < 0) ? 0 : slot.animSelectedPos;
        if (curPos < 0 || curPos >= count)
            curPos = 0;
        const uint32_t animIndex = slot.animAvailableIndices[static_cast<size_t>(curPos)];

        std::string animName;
        size_t totalOnModel = 0;
        if (const std::shared_ptr<ModelAsset> modelAsset = ImportedSlotModel(slot))
        {
            const auto& names = modelAsset->GetAnimationNames();
            totalOnModel = names.size();
            if (animIndex < names.size())
                animName = names[animIndex];
        }

        // Total is the model's animation count, not the probe list (capped separately in TickRender).
        if (totalOnModel == 0)
            totalOnModel = static_cast<size_t>(count);

        const size_t oneBased = static_cast<size_t>(animIndex) + 1u;
        totalOnModel = std::max(totalOnModel, oneBased);
        const std::string suffix = std::to_string(oneBased) + "/" + std::to_string(totalOnModel);
        if (animName.empty())
            return suffix;
        return animName + " - " + suffix;
    }

    return {};
}

size_t ModelThumbnailHandler::GetFocusedAnimationCount()
{
    if (!s_Instance)
        return 0;
    if (s_OrbitFocusGuid.IsNull())
        return 0;

    ModelThumbnailHandler* self = s_Instance;
    const GUID focusedGuid = s_OrbitFocusGuid;

    for (auto& [windowId, ws] : self->m_Windows)
    {
        auto itSlot = ws.guidToSlot.find(focusedGuid);
        if (itSlot == ws.guidToSlot.end())
            continue;

        const size_t slotIdx = itSlot->second;
        if (slotIdx >= ws.slots.size())
            continue;

        return ws.slots[slotIdx].animAvailableIndices.size();
    }

    return 0;
}

void ModelThumbnailHandler::SetExternalAnimationPreview(const std::filesystem::path& modelPath,
                                                        const std::shared_ptr<AnimationClip>& clip,
                                                        float currentTime)
{
    if (!s_Instance)
        return;

    // Resolve through AssetManager, which registers the model when nothing has
    // yet: a raw registry lookup finds nothing for an unregistered path, and the
    // preview would key on a null GUID.
    GUID previewGuid{};
    if (!modelPath.empty() && s_Instance->m_AssetManager)
        previewGuid = s_Instance->m_AssetManager->ResolveAssetGuid(modelPath);

    {
        std::lock_guard<std::mutex> lock(s_AnimationPreviewMutex);
        const bool guidChanged = previewGuid != s_AnimationPreviewGuid;
        const bool clipChanged = clip != s_AnimationPreviewClip;
        const bool timeChanged = std::fabs(currentTime - s_AnimationPreviewTime) > 1e-4f;
        if (!guidChanged && !clipChanged && !timeChanged)
            return;

        s_AnimationPreviewGuid = previewGuid;
        s_AnimationPreviewClip = clip;
        s_AnimationPreviewTime = currentTime;
    }

    for (auto& [windowId, ws] : s_Instance->m_Windows)
    {
        if (previewGuid.IsNull())
            continue;

        auto itSlot = ws.guidToSlot.find(previewGuid);
        if (itSlot == ws.guidToSlot.end())
            continue;

        const size_t slotIdx = itSlot->second;
        if (slotIdx >= ws.slots.size())
            continue;

        Slot& slot = ws.slots[slotIdx];
        slot.needsOrbitRerender = true;
        EnqueuePending(ws, previewGuid, false);
    }
}

void ModelThumbnailHandler::ClearExternalAnimationPreview()
{
    if (!s_Instance)
        return;
    GUID previousGuid{};
    {
        std::lock_guard<std::mutex> lock(s_AnimationPreviewMutex);
        if (s_AnimationPreviewGuid.IsNull() && !s_AnimationPreviewClip)
            return;

        previousGuid = s_AnimationPreviewGuid;
        s_AnimationPreviewGuid = GUID{};
        s_AnimationPreviewClip.reset();
        s_AnimationPreviewTime = 0.0f;
    }

    for (auto& [windowId, ws] : s_Instance->m_Windows)
    {
        auto itSlot = ws.guidToSlot.find(previousGuid);
        if (itSlot == ws.guidToSlot.end())
            continue;

        const size_t slotIdx = itSlot->second;
        if (slotIdx >= ws.slots.size())
            continue;

        Slot& slot = ws.slots[slotIdx];
        slot.animClip.reset();
        slot.animLoadedPos = -1;
        slot.cachedClipPos = -1;
        slot.usingExternalPreviewClip = false;
        slot.needsOrbitRerender = true;
        EnqueuePending(ws, previousGuid, false);
    }
}

// ---------------------------------------------------------------------------
// Material preview shape
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::SetMaterialPreviewShape(PreviewShape shape)
{
    if (shape == s_MaterialPreviewShape)
        return;
    s_MaterialPreviewShape = shape;
    s_MaterialShapeChanged = true;
}

void ModelThumbnailHandler::InvalidateMaterialThumbnail(const GUID& guid, bool propertyValuesOnly)
{
    if (guid.IsNull() || !s_Instance)
        return;

    for (auto& [windowId, ws] : s_Instance->m_Windows)
    {
        (void)windowId;
        for (Slot& slot : ws.slots)
        {
            if (!slot.occupied || !slot.isMaterial || slot.guid != guid)
                continue;

            const bool wasReady = slot.ready;
            slot.ready = false;
            slot.inFlight = false;
            slot.needsOrbitRerender = true;
            slot.needsUiDetachBeforeRender = wasReady;
            for (RenderLane& lane : ws.lanes)
            {
                if (!propertyValuesOnly)
                {
                    lane.isSpawnedMaterial = false;
                    lane.spawnedMaterialGuid = GUID{};
                }
                // The changed material settles again before its PNG is taken.
                if (lane.isSpawnedMaterial && lane.spawnedMaterialGuid == guid)
                    lane.spawnedSettleFrames = 0;
            }
            EnqueuePending(ws, guid, false, true, slot.previewIblEnabled);
        }
    }
}

void ModelThumbnailHandler::SetMaterialOrbitFocusFromEngineName(const std::string& engineName)
{
    GUID guid;
    bool previewIblEnabled = true;
    if (TryParseMaterialGuidFromEngineName(engineName, guid, &previewIblEnabled))
    {
        // The focus waits until the material's tile is cached (at most a few seconds): the
        // slot renders the square tile first (PromotePendingMaterialFocus), since the focused,
        // panel-shaped render is never cached and the tile and the Asset View share one slot.
        const bool focused = guid == s_MaterialOrbitFocusGuid && previewIblEnabled == s_MaterialOrbitFocusIblEnabled;
        const bool pending =
            guid == s_PendingMaterialFocusGuid && previewIblEnabled == s_PendingMaterialFocusIblEnabled;
        if (!focused && !pending)
        {
            s_MaterialOrbitFocusGuid = GUID{};
            s_PendingMaterialFocusGuid = guid;
            s_PendingMaterialFocusIblEnabled = previewIblEnabled;
            s_PendingMaterialFocusSince = std::chrono::steady_clock::now();
        }
    }
    ResetFocusedPreviewZoom();
}

void ModelThumbnailHandler::SetLensFlareFocusFromEngineName(const std::string& engineName)
{
    GUID guid;
    if (TryParseLensFlareGuidFromEngineName(engineName, guid))
        s_LensFlareFocusGuid = guid;
}

// ---------------------------------------------------------------------------
// Project switch / resolution
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::ResetForProjectSwitch()
{
    m_PendingDiskCache.reset();
    m_DiskCacheRequested.clear();
    m_BakeQueue.clear();
    m_BakeQueued.clear();
    m_BakeBatch = {};
    PublishFolderBakeReport();
    m_EmptyCaptureRetried.clear();
    // Lanes first: an import unloads only once no lane renders its model.
    for (auto& winPair : m_Windows)
    {
        WindowState& ws = winPair.second;
        for (RenderLane& lane : ws.lanes)
        {
            lane.inFlightSlot = kMaxResidentSlots;
            lane.inFlightFrame = 0;

            // Teardown ECS-spawned model/material and clear the lane's world.
            TeardownSpawnedModel(lane);
            TeardownSpawnedMaterial(lane);
            if (lane.world)
                lane.world->Clear();
            lane.orbitRootEntity = ECS::EntityHandle::Invalid();
            lane.keyLightEntity = ECS::EntityHandle::Invalid();
            lane.ambientLightEntity = ECS::EntityHandle::Invalid();
            lane.isSpawnedMaterial = false;
            lane.spawnedMaterialGuid = GUID{};
            lane.spawnedMaterialShapeGuid = GUID{};
            lane.primitiveEntity = ECS::EntityHandle::Invalid();
            lane.isSpawnedLensFlare = false;
            lane.spawnedLensFlareGuid = GUID{};
            lane.lensFlareEntity = ECS::EntityHandle::Invalid();

            // Ensure persistent submissions don't keep re-rendering after a switch.
            if (lane.viewId != 0 && m_RenderServices)
                m_RenderServices->GetWorldDrawBuilder().ClearView(lane.viewId);
        }
    }
    ReleaseAllModelImports();

    // We intentionally do NOT destroy GPU buffers or slot device textures here;
    // they are owned by the window/device and will be reused. We only clear
    // logical mappings so we don't display/resolve thumbnails for GUIDs that
    // belonged to a previous AssetManager session.
    for (auto& winPair : m_Windows)
    {
        WindowState& ws = winPair.second;

        ws.pending.clear();
        ws.pendingSet.clear();
        ws.guidToSlot.clear();
        ws.guidToListSlot.clear();
        ws.guidToMaterialSlot.clear();
        ws.guidToMaterialNoIblSlot.clear();
        ws.guidToLensFlareSlot.clear();

        for (Slot& s : ws.slots)
        {
            s.guid = GUID{};
            s.uiKey.clear();
            s.occupied = false;
            s.ready = false;
            s.inFlight = false;
            s.needsOrbitRerender = false;
            s.needsUiDetachBeforeRender = false;
            s.uiBound = false;
            s.lastUsed = 0;
            s.animClip.reset();
            s.animTime = 0.0f;
            s.usingExternalPreviewClip = false;
            s.animAvailableIndices.clear();
            s.animSelectedPos = -1;
            s.animLoadedPos = -1;
            s.cachedClipIndex = 0;
            s.cachedClipPos = -1;
            s.framingBoundsValid = false;
            s.listStaticThumb = false;
            s.isMaterial = false;
            s.isLensFlare = false;
            s.previewIblEnabled = true;
            s.materialRenderSettled = false;
            s.bakeOnly = false;
        }

        ws.lruCounter = 0;
    }
    s_LensFlareFocusGuid = GUID{};
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

std::string ModelThumbnailHandler::MakeEngineThumbnailId(const GUID& guid, bool listStatic)
{
    std::string compact = guid.ToCompactString();
    if (listStatic)
        return std::string("engine:") + kEngineListThumbPrefix + compact;
    return std::string("engine:") + kEngineThumbPrefix + compact;
}

std::string ModelThumbnailHandler::MakeUITextureKey(const GUID& guid, bool listStatic)
{
    if (listStatic)
        return std::string(kEngineListThumbPrefix) + guid.ToCompactString();
    return std::string(kEngineThumbPrefix) + guid.ToCompactString();
}

std::string ModelThumbnailHandler::MakeMaterialEngineThumbnailId(const GUID& guid,
                                                                 bool previewIblEnabled)
{
    std::string id = std::string("engine:") + kEngineMaterialThumbPrefix + guid.ToCompactString();
    if (!previewIblEnabled)
        id += kEngineMaterialNoIblSuffix;
    return id;
}

std::string ModelThumbnailHandler::MakeMaterialUITextureKey(const GUID& guid,
                                                            bool previewIblEnabled)
{
    std::string key = std::string(kEngineMaterialThumbPrefix) + guid.ToCompactString();
    if (!previewIblEnabled)
        key += kEngineMaterialNoIblSuffix;
    return key;
}

std::string ModelThumbnailHandler::MakeLensFlareEngineThumbnailId(const GUID& guid)
{
    return std::string("engine:") + kEngineLensFlareThumbPrefix + guid.ToCompactString();
}

std::string ModelThumbnailHandler::MakeLensFlareUITextureKey(const GUID& guid)
{
    return std::string(kEngineLensFlareThumbPrefix) + guid.ToCompactString();
}

bool ModelThumbnailHandler::TryParseLensFlareGuidFromEngineName(const std::string& engineName,
                                                                GUID& outGuid)
{
    const size_t prefixLen = std::strlen(kEngineLensFlareThumbPrefix);
    if (engineName.rfind(kEngineLensFlareThumbPrefix, 0) != 0)
        return false;
    const std::string suffix = engineName.substr(prefixLen);
    if (suffix.size() != GUID::kSize * 2)
        return false;
    for (char c : suffix)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    outGuid = GUID(suffix.c_str());
    return !outGuid.IsNull();
}

bool ModelThumbnailHandler::TryParseMaterialGuidFromEngineName(const std::string& engineName,
                                                               GUID& outGuid,
                                                               bool* outPreviewIblEnabled)
{
    const size_t prefixLen = std::strlen(kEngineMaterialThumbPrefix);
    if (engineName.rfind(kEngineMaterialThumbPrefix, 0) != 0)
        return false;

    std::string suffix = engineName.substr(prefixLen);
    bool previewIblEnabled = true;
    if (suffix.size() > std::strlen(kEngineMaterialNoIblSuffix) &&
        suffix.compare(suffix.size() - std::strlen(kEngineMaterialNoIblSuffix),
                       std::strlen(kEngineMaterialNoIblSuffix),
                       kEngineMaterialNoIblSuffix) == 0)
    {
        suffix.resize(suffix.size() - std::strlen(kEngineMaterialNoIblSuffix));
        previewIblEnabled = false;
    }

    if (suffix.size() != GUID::kSize * 2)
        return false;

    for (char c : suffix)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }

    outGuid = GUID(suffix.c_str());
    if (outGuid.IsNull())
        return false;
    if (outPreviewIblEnabled)
        *outPreviewIblEnabled = previewIblEnabled;
    return true;
}

void ModelThumbnailHandler::EnqueuePending(WindowState& ws, const GUID& guid, bool listStatic,
                                           bool isMaterial,
                                           std::optional<bool> previewIblEnabled,
                                           bool isLensFlare)
{
    if (guid.IsNull())
        return;
    const bool materialPreviewIblEnabled =
        isMaterial ? previewIblEnabled.value_or(GetPreviewIblEnabled()) : true;
    const PendingThumbRequest request{guid, listStatic, isMaterial, isLensFlare,
                                      materialPreviewIblEnabled, s_ScrollGeneration};
    if (ws.pendingSet.insert({guid, listStatic, isMaterial, isLensFlare, materialPreviewIblEnabled}).second)
    {
        ws.pending.push_back(request);
        return;
    }
    // Asked for again: the tile is on screen after the latest scroll.
    for (PendingThumbRequest& queued : ws.pending)
    {
        if (queued.Guid == guid && queued.ListStatic == listStatic &&
            queued.IsMaterial == isMaterial && queued.IsLensFlare == isLensFlare &&
            queued.PreviewIblEnabled == materialPreviewIblEnabled)
        {
            queued.ScrollGeneration = s_ScrollGeneration;
            return;
        }
    }
}

void ModelThumbnailHandler::RequeuePending(WindowState& ws, const PendingThumbRequest& request)
{
    if (ws.pendingSet.insert({request.Guid, request.ListStatic, request.IsMaterial,
                              request.IsLensFlare, request.PreviewIblEnabled})
            .second)
        ws.pending.push_back(request);
}

// ---------------------------------------------------------------------------
// Preview size / resolution configuration
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::SetPreviewSize(uint32_t widthPx, uint32_t heightPx)
{
    // Round up to a sensible minimum so tiny/zero panel sizes don't produce degenerate RTs.
    constexpr uint32_t kMinPreviewDim = 32u;
    constexpr uint32_t kMaxPreviewDim = 4096u;
    uint32_t w = (widthPx == 0 && heightPx == 0) ? 0u : std::clamp(widthPx, kMinPreviewDim, kMaxPreviewDim);
    uint32_t h = (widthPx == 0 && heightPx == 0) ? 0u : std::clamp(heightPx, kMinPreviewDim, kMaxPreviewDim);

    // Avoid churning RTs on every sub-pixel layout shift. Accept if dims differ by
    // more than a small threshold or become zero.
    const bool sizeChanged = (w != s_PreviewWidthPx) || (h != s_PreviewHeightPx);
    if (!sizeChanged)
        return;

    // The focused slot's device texture is recreated lazily in TickRender when
    // its cached dims don't match the desired dims; the per-dispatch
    // intermediates are sized from the slot.
    s_PreviewWidthPx = w;
    s_PreviewHeightPx = h;
}

void ModelThumbnailHandler::SetThumbnailResolution(int px)
{
    // Clamp to supported values (256 or 1024).
    const int next = (px <= 256) ? 256 : 1024;
    if (next == s_ThumbnailResolutionPx)
        return;

    s_ThumbnailResolutionPx = next;

    // Drop cached windows so logical textures are recreated at the new resolution.
    if (!s_Instance)
        return;

    ModelThumbnailHandler* self = s_Instance;
    for (auto& [windowId, ws] : self->m_Windows)
    {
        (void)windowId;
        // Mark all slots as needing detach so UI bindings are cleared. The
        // TickRender resize sweep will recreate backing textures in-place when
        // it observes slot.texWidth/texHeight no longer match desired dims.
        for (Slot& slot : ws.slots)
        {
            if (!slot.occupied || slot.showsBakedImage)
                continue;
            slot.ready = false;
            slot.inFlight = false;
            slot.needsUiDetachBeforeRender = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Path resolution / asset loading
// ---------------------------------------------------------------------------

bool ModelThumbnailHandler::ResolveModelThumbnailPath(const std::filesystem::path& assetPath,
                                                        std::filesystem::path& outResolved,
                                                        GUID& outGuid)
{
    outResolved = assetPath;
    outGuid = GUID{};

    if (!m_AssetManager)
    {
        return false;
    }

    std::filesystem::path resolvedPath = assetPath;
    const std::filesystem::path& assetsRoot = m_AssetManager->GetAssetRoot();
    if (resolvedPath.is_absolute())
    {
        std::error_code ec;
        std::filesystem::path relToRoot = std::filesystem::relative(resolvedPath, assetsRoot, ec);
        if (!ec && !relToRoot.empty() && relToRoot.native()[0] != L'.')
        {
            resolvedPath = m_AssetManager->ResolveAssetPath(relToRoot);
        }
    }
    else
    {
        resolvedPath = m_AssetManager->ResolveAssetPath(resolvedPath);
    }

    // Resolve through AssetManager, which registers the file when nothing has
    // yet: a raw registry lookup finds nothing for an unregistered path, and the
    // model thumbnail would never render (blank hierarchy icon after import).
    GUID guid = m_AssetManager->ResolveAssetGuid(resolvedPath);
    if (guid.IsNull() && resolvedPath != assetPath)
        guid = m_AssetManager->ResolveAssetGuid(assetPath);

    if (guid.IsNull())
    {
        Logger::Log::Warning("ModelThumbnailHandler: GUID null after resolve for '{}'", assetPath.string());
        return false;
    }

    outResolved = std::move(resolvedPath);
    outGuid = guid;
    return true;
}

// ---------------------------------------------------------------------------
// GetOrRequest
// ---------------------------------------------------------------------------

std::string ModelThumbnailHandler::GetOrRequest(const std::filesystem::path& assetPath,
                                                int desiredSize,
                                                std::function<void(const std::string& relPath)> onReady,
                                                bool StaticModelListThumbnail)
{
    (void)desiredSize;

    std::filesystem::path resolvedPath;
    GUID guid;
    if (!ResolveModelThumbnailPath(assetPath, resolvedPath, guid))
    {
        if (onReady)
            onReady(std::string());
        return std::string();
    }

    const std::string engineId = GetOrRequestByGuid(guid, StaticModelListThumbnail);

    if (onReady)
        onReady(engineId);
    return engineId;
}

std::string ModelThumbnailHandler::GetOrRequestByGuid(const GUID& guid, bool listStatic)
{
    return MakeEngineThumbnailId(guid, listStatic);
}

std::string ModelThumbnailHandler::GetOrRequestLensFlare(
    const std::filesystem::path& assetPath,
    int desiredSize,
    std::function<void(const std::string& relPath)> onReady)
{
    (void)desiredSize;
    std::filesystem::path resolvedPath;
    GUID guid;
    if (!ResolveModelThumbnailPath(assetPath, resolvedPath, guid))
    {
        if (onReady)
            onReady({});
        return {};
    }

    const std::string engineId = MakeLensFlareEngineThumbnailId(guid);
    if (onReady)
        onReady(engineId);
    return engineId;
}

void ModelThumbnailHandler::EnsureEngineThumbnailRequested(uint64_t windowId,
                                                           const std::string& engineName)
{
    // The UI retries unresolved textures. Keep cached bindings, but do not
    // allocate slots or start high-priority asset imports during a gesture.
    if (IsAssetBrowserScrolling())
        return;
    if (windowId == 0 || !m_AssetManager)
        return;

    GUID lensFlareGuid;
    if (TryParseLensFlareGuidFromEngineName(engineName, lensFlareGuid))
    {
        WindowState& ws = GetOrCreateWindowState(windowId);
        const size_t slotIdx = AcquireSlotForGuid(
            ws, lensFlareGuid, false, false, std::nullopt, true);
        Slot& slot = ws.slots[slotIdx];
        if (!slot.ready && !slot.inFlight)
        {
            if (!m_AssetManager->GetAsset(lensFlareGuid))
            {
                m_AssetManager->LoadAsset(
                    lensFlareGuid, AssetLoadResultCallback{}, AssetLoadPriority::High);
            }
            EnqueuePending(ws, lensFlareGuid, false, false, std::nullopt, true);
        }
        return;
    }

    // Material thumbnails route through the material slot map.
    GUID materialGuid;
    bool previewIblEnabled = true;
    if (TryParseMaterialGuidFromEngineName(engineName, materialGuid, &previewIblEnabled))
    {
        WindowState& ws = GetOrCreateWindowState(windowId);
        const size_t slotIdx = AcquireSlotForGuid(ws, materialGuid, false, true,
                                                  previewIblEnabled);
        Slot& slot = ws.slots[slotIdx];
        if (!slot.ready && !slot.inFlight)
        {
            // Bump the asset ahead of the bulk import queue so a visible icon
            // resolves promptly even while a large project is still importing.
            if (!m_AssetManager->GetAsset(materialGuid))
                m_AssetManager->LoadAsset(materialGuid, AssetLoadResultCallback{}, AssetLoadPriority::High);
            EnqueuePending(ws, materialGuid, false, true, previewIblEnabled);
        }
        return;
    }

    // Model thumbnails: parse the (list-aware) GUID and enqueue the matching
    // slot. A name that is not a thumbnail engine name parses out and is ignored.
    GUID guid;
    bool listStatic = false;
    if (!TryParseGuidFromEngineName(engineName, guid, listStatic))
        return;
    if (guid.IsNull() || m_AssetManager->IsLoadSuppressed(guid))
        return;

    RequestModelTile(GetOrCreateWindowState(windowId), guid, listStatic);
}

bool ModelThumbnailHandler::TryParseGuidFromEngineName(const std::string& engineName,
                                                         GUID& outGuid,
                                                         bool& outListStatic)
{
    outListStatic = false;
    size_t prefixLen = std::strlen(kEngineThumbPrefix);
    if (engineName.rfind(kEngineListThumbPrefix, 0) == 0)
    {
        outListStatic = true;
        prefixLen = std::strlen(kEngineListThumbPrefix);
    }
    else if (engineName.rfind(kEngineThumbPrefix, 0) != 0)
    {
        return false;
    }

    const std::string suffix = engineName.substr(prefixLen);
    if (suffix.size() != GUID::kSize * 2)
        return false;

    for (char c : suffix)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }

    outGuid = GUID(suffix.c_str());
    return !outGuid.IsNull();
}

ModelThumbnailHandler::WindowState& ModelThumbnailHandler::GetOrCreateWindowState(uint64_t windowId)
{
    auto it = m_Windows.find(windowId);
    if (it != m_Windows.end())
        return it->second;

    WindowState ws{};
    ws.slots.resize(kMaxResidentSlots);

    // Each lane gets its own tiny ECS world, isolated from the editor's
    // primary world and from the other lanes.
    for (RenderLane& lane : ws.lanes)
        lane.world = ModelThumbnailShared::MakeThumbnailWorld();

    auto [ins, _] = m_Windows.emplace(windowId, std::move(ws));
    return ins->second;
}

// ---------------------------------------------------------------------------
// Slot management / texture creation
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::ComputeDesiredSlotDims(const Slot& slot, const GUID& guid,
                                                   bool listStatic, bool isMaterial,
                                                   bool isLensFlare,
                                                   uint32_t& outW, uint32_t& outH)
{
    // The focused (non-list) slot tracks the live preview size so the Asset
    // View panel can be non-square; all other slots use the square fallback.
    outW = static_cast<uint32_t>(s_ThumbnailResolutionPx);
    outH = static_cast<uint32_t>(s_ThumbnailResolutionPx);
    const bool isMaterialFocus =
        isMaterial && !s_MaterialOrbitFocusGuid.IsNull() && guid == s_MaterialOrbitFocusGuid &&
        slot.previewIblEnabled == s_MaterialOrbitFocusIblEnabled;
    const bool isModelFocus =
        !isMaterial && !isLensFlare && !listStatic && !s_OrbitFocusGuid.IsNull() &&
        guid == s_OrbitFocusGuid;
    const bool isLensFlareFocus =
        isLensFlare && !s_LensFlareFocusGuid.IsNull() && guid == s_LensFlareFocusGuid;
    if ((isMaterialFocus || isModelFocus || isLensFlareFocus) &&
        s_PreviewWidthPx > 0 && s_PreviewHeightPx > 0)
    {
        outW = s_PreviewWidthPx;
        outH = s_PreviewHeightPx;
    }
}

void ModelThumbnailHandler::EnsureSlotDeviceTexture(Slot& slot, size_t slotIdx)
{
    if (slot.deviceTex.IsValid())
        return;
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    if (!device)
        return;

    // Acquire-time dims snapshot wins (focus may have moved since acquire);
    // fall back to dispatch-time computation when unset.
    uint32_t desiredW = slot.texWidth;
    uint32_t desiredH = slot.texHeight;
    if (desiredW == 0 || desiredH == 0)
        ComputeDesiredSlotDims(slot, slot.guid, slot.listStaticThumb, slot.isMaterial,
                               slot.isLensFlare, desiredW, desiredH);

    Rendering::TextureDesc td{};
    td.width = desiredW;
    td.height = desiredH;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    // Display-referred LINEAR, like the SceneView viewport. The UI composites in
    // linear and the editor's terminal FinalSRGBEncode pass owns the only OETF,
    // so the slot must stay linear — an sRGB-encoded slot would be encoded twice
    // (washed-out previews). RGBA16F avoids 8-bit linear banding in the darks.
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    // TransferSrc: the thumbnail is read back to CPU pixels to be cached on
    // disk, by a graph readback pass that restores the sampled layout after
    // the copy (RequestDeviceTextureReadbackRG).
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
               static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
               static_cast<uint32_t>(Rendering::TextureUsage::TransferSrc);
    const std::string debugName =
        std::string("Editor.ModelThumb.Slot") + std::to_string(slotIdx);
    td.debugName = debugName.c_str();
    slot.deviceTex = device->CreateTexture(td);
    slot.deviceTexInitialized = false;
    slot.texWidth = desiredW;
    slot.texHeight = desiredH;
}

size_t ModelThumbnailHandler::AcquireSlotForGuid(WindowState& ws, const GUID& guid,
                                                 bool listStatic, bool isMaterial,
                                                 std::optional<bool> previewIblEnabled,
                                                 bool isLensFlare)
{
    const bool materialPreviewIblEnabled =
        isMaterial ? previewIblEnabled.value_or(GetPreviewIblEnabled()) : true;
    auto& guidMap = isLensFlare
        ? ws.guidToLensFlareSlot
        : (isMaterial
            ? (materialPreviewIblEnabled ? ws.guidToMaterialSlot : ws.guidToMaterialNoIblSlot)
            : (listStatic ? ws.guidToListSlot : ws.guidToSlot));
    auto it = guidMap.find(guid);
    if (it != guidMap.end())
    {
        const size_t idx = it->second;
        ws.slots[idx].lastUsed = ++ws.lruCounter;
        ws.slots[idx].bakeOnly = false;
        return idx;
    }

    // Find a free slot first.
    size_t slotIdx = kMaxResidentSlots;
    for (size_t i = 0; i < ws.slots.size(); ++i)
    {
        if (!ws.slots[i].occupied)
        {
            slotIdx = i;
            break;
        }
    }

    bool victimHadLiveTexture = false;

    // Otherwise evict the least-recently-used slot.
    if (slotIdx == kMaxResidentSlots)
    {
        uint64_t bestScore = std::numeric_limits<uint64_t>::max();
        size_t bestIdx = kMaxResidentSlots;
        for (size_t i = 0; i < ws.slots.size(); ++i)
        {
            const Slot& s = ws.slots[i];
            // Never evict a slot currently being rendered; that can corrupt
            // in-flight bookkeeping and cause UI/read-after-write hazards.
            if (s.inFlight)
                continue;
            if (s.lastUsed < bestScore)
            {
                bestScore = s.lastUsed;
                bestIdx = i;
            }
        }
        if (bestIdx == kMaxResidentSlots)
        {
            // Defensive fallback: if all slots are marked in-flight, do not
            // stomp active bookkeeping. Keep using slot 0 as an overwrite slot.
            bestIdx = 0;
        }

        Slot& victim = ws.slots[bestIdx];
        victimHadLiveTexture = victim.occupied && victim.ready && !victim.inFlight;
        // A folder bake gives its slot to a tile and bakes again later.
        if (victim.occupied && victim.bakeOnly && m_BakeQueued.insert(victim.guid).second)
            m_BakeQueue.push_front({victim.guid, victim.isMaterial});
        ReleaseSlotAssignment(ws, bestIdx);
        slotIdx = bestIdx;
    }

    Slot& slot = ws.slots[slotIdx];
    // A baked image is no render target: the new owner gets a fresh one.
    if (slot.showsBakedImage)
    {
        if (Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr)
            device->DestroyTexture(slot.deviceTex);
        slot.deviceTex = {};
        slot.deviceTexInitialized = false;
        slot.showsBakedImage = false;
    }
    slot.bakedImagePending = false;
    slot.bakedImageRetryFrame = 0;
    slot.restoresBakedImage = false;
    // Texture creation is deferred to dispatch time (EnsureSlotDeviceTexture):
    // acquisition runs during pure-frame primitive generation, before the
    // frame exists. A reused slot keeps its existing device texture. The
    // DESIRED dims are still snapshotted NOW (HEAD parity): focus can move
    // between this acquire and the first dispatch, and the focused slot must
    // get its preview-size target from the acquire-time focus state.
    if (!slot.deviceTex.IsValid())
        ComputeDesiredSlotDims(
            slot, guid, listStatic, isMaterial, isLensFlare, slot.texWidth, slot.texHeight);

    slot.guid = guid;
    slot.framingBoundsValid = false;
    slot.listStaticThumb = listStatic;
    slot.isMaterial = isMaterial;
    slot.isLensFlare = isLensFlare;
    slot.previewIblEnabled = materialPreviewIblEnabled;
    slot.uiKey = isLensFlare
        ? MakeLensFlareUITextureKey(guid)
        : (isMaterial ? MakeMaterialUITextureKey(guid, materialPreviewIblEnabled)
                      : MakeUITextureKey(guid, listStatic));
    slot.occupied = true;
    slot.ready = false;
    slot.inFlight = false;
    slot.animAvailableIndices.clear();
    slot.animSelectedPos = -1;
    slot.animLoadedPos = -1;
    slot.cachedClipIndex = 0;
    slot.cachedClipPos = -1;
    slot.needsOrbitRerender = false;
    slot.gpuNotReadyRetries = 0;
    slot.renderGaveUp = false;
    slot.loadWaitSince = {};
    slot.loadWaitReported = false;
    slot.modelImport = {};
    slot.materialRenderSettled = false;
    slot.bakeOnly = false;
    slot.needsUiDetachBeforeRender = victimHadLiveTexture;
    slot.uiBound = false;
    slot.lastUsed = ++ws.lruCounter;
    guidMap[guid] = slotIdx;
    return slotIdx;
}

// ---------------------------------------------------------------------------
// IsEngineThumbnailReadyForWindow / RegisterReadyThumbnailsRG
// ---------------------------------------------------------------------------

bool ModelThumbnailHandler::IsEngineThumbnailReadyForWindow(uint64_t windowId,
                                                             const std::string& engineName) const
{
    auto itWin = m_Windows.find(windowId);
    if (itWin == m_Windows.end())
        return false;
    const WindowState& ws = itWin->second;

    // RenderGraph mode publishes per frame — there is no retained binding, so
    // readiness is purely "a completed render exists" (occupied && ready).
    // The old arm additionally requires the retained UI binding.
    const auto slotReady = [&ws](const Slot& slot)
    { return slot.occupied && slot.ready && (ws.rg2Mode || slot.uiBound); };

    {
        GUID lensFlareGuid;
        if (TryParseLensFlareGuidFromEngineName(engineName, lensFlareGuid))
        {
            auto itSlot = ws.guidToLensFlareSlot.find(lensFlareGuid);
            if (itSlot == ws.guidToLensFlareSlot.end() || itSlot->second >= ws.slots.size())
                return false;
            return slotReady(ws.slots[itSlot->second]);
        }
    }

    // Material thumbnail?
    {
        GUID materialGuid;
        bool previewIblEnabled = true;
        if (TryParseMaterialGuidFromEngineName(engineName, materialGuid, &previewIblEnabled))
        {
            const auto& materialSlotMap = previewIblEnabled
                ? ws.guidToMaterialSlot
                : ws.guidToMaterialNoIblSlot;
            auto itSlot = materialSlotMap.find(materialGuid);
            if (itSlot == materialSlotMap.end() || itSlot->second >= ws.slots.size())
                return false;
            return slotReady(ws.slots[itSlot->second]);
        }
    }

    GUID guid;
    bool listStatic = false;
    if (!TryParseGuidFromEngineName(engineName, guid, listStatic))
        return false;

    const auto& map = listStatic ? ws.guidToListSlot : ws.guidToSlot;
    auto itSlot = map.find(guid);
    if (itSlot == map.end() || itSlot->second >= ws.slots.size())
        return false;
    return slotReady(ws.slots[itSlot->second]);
}

bool ModelThumbnailHandler::RegisterReadyThumbnailsRG(uint64_t windowId, UIManager* ui,
                                                      Rendering::RenderGraph::RGFrame* pureFrame)
{
    if (windowId == 0 || !ui)
        return false;

    auto itWin = m_Windows.find(windowId);
    if (itWin == m_Windows.end())
        return false;

    WindowState& ws = itWin->second;
    bool changed = false;

    // GE_UI_REBIND_DIAG=1: same census as the old arm.
    static const bool sDiag = []() {
        const char* e = std::getenv("GE_UI_REBIND_DIAG");
        return e && e[0] == '1';
    }();
    if (sDiag)
    {
        static uint64_t sCounter = 0;
        if ((sCounter++ % 120ull) == 0ull)
        {
            uint32_t occupied = 0, ready = 0, inflight = 0;
            for (const Slot& s : ws.slots)
            {
                if (!s.occupied)
                    continue;
                ++occupied;
                if (s.ready)
                    ++ready;
                if (s.inFlight)
                    ++inflight;
            }
            const auto busyLanes = std::count_if(
                ws.lanes.begin(), ws.lanes.end(),
                [](const RenderLane& lane) { return lane.inFlightSlot != kMaxResidentSlots; });
            Logger::Log::Info(
                "[ThumbRegDiag] RenderGraph slots={} occupied={} ready={} inFlight={} evicted={} "
                "busyLanes={}/{} pending={} pngServedReleases={} arm={}",
                ws.slots.size(), occupied, ready, inflight, ws.evictedUiKeys.size(), busyLanes,
                ws.lanes.size(), ws.pending.size(), PendingGridSlotReleaseCount(),
                pureFrame ? "pure" : "hybrid");
        }
    }

    for (const std::string& key : ws.evictedUiKeys)
    {
        ui->RemoveExternalTexture(key);
        changed = true;
    }
    ws.evictedUiKeys.clear();

    for (size_t i = 0; i < ws.slots.size(); ++i)
    {
        Slot& slot = ws.slots[i];
        if (!slot.occupied || slot.uiKey.empty() || !slot.deviceTex.IsValid())
        {
            if (slot.uiBound)
            {
                ui->RemoveExternalTexture(slot.uiKey);
                slot.uiBound = false;
                changed = true;
            }
            continue;
        }
        // A baked image is a static texture that no graph pass writes: bound
        // once through the device path, never published per frame.
        if (slot.showsBakedImage)
        {
            if (!slot.uiBound)
                changed = true;
            ui->SetExternalTexture(slot.uiKey, slot.deviceTex, slot.texWidth, slot.texHeight,
                                   UI::UITextureSpace::SrgbAuthored());
            slot.uiBound = true;
            continue;
        }
        // Ready orbit/anim slots stay published while inFlight — the graph
        // edge orders encode before the UI sample. Unbinding those blanks
        // Asset View every anim frame. Reload marks ready=false; keep the
        // last completed frame bound until a replacement encode starts.
        if (!slot.ready)
        {
            if (slot.inFlight)
            {
                if (slot.uiBound)
                {
                    ui->RemoveExternalTexture(slot.uiKey);
                    slot.uiBound = false;
                    changed = true;
                }
                continue;
            }
            if (!slot.uiBound)
                continue;
        }

        const uint32_t naturalW =
            slot.texWidth > 0 ? slot.texWidth : static_cast<uint32_t>(s_ThumbnailResolutionPx);
        const uint32_t naturalH =
            slot.texHeight > 0 ? slot.texHeight : static_cast<uint32_t>(s_ThumbnailResolutionPx);

        // 'changed' reports bind/remove TRANSITIONS only (the sibling old-arm
        // contract): the per-frame publish and the idempotent re-registration
        // are not binding changes.
        if (!slot.uiBound)
            changed = true;

        if (pureFrame)
        {
            // Registration is idempotent while dims/space are unchanged; the
            // PUBLISH is the per-frame value RenderRG consumes — every ready
            // slot must publish on every pure frame (a missed publish renders
            // nothing that frame by design). The declared read in the UI pass
            // is the producer→sampler edge when this slot also rendered this
            // frame (dedup-by-physical lands on the same id).
            // The slot's own tonemap always emits tonemapped display-referred
            // linear [0,1] (TonemapParams.outEncoding stays 1; no thumbnail
            // path keys it on the display mode), so the truthful stamp is a
            // constant DisplayLinearSdr — constant because the producer is,
            // not because of what the display happens to be. In SDR the
            // composite is unchanged; under HDR output the slot follows the
            // UI white with the chrome around it.
            ui->SetExternalTextureRG(slot.uiKey, naturalW, naturalH,
                                      UI::UITextureSpace::DisplayLinearSdr(),
                                      Rendering::TextureFormat::R16G16B16A16_FLOAT);
            const std::string importName =
                std::string("Editor.ModelThumb.Slot") + std::to_string(i);
            const Rendering::RenderGraph::RGTexture tex = pureFrame->ImportExternalTexture(
                importName.c_str(), slot.deviceTex,
                slot.deviceTexInitialized ? Rendering::ResourceState::ShaderResource
                                          : Rendering::ResourceState::Undefined,
                Rendering::TextureFormat::R16G16B16A16_FLOAT);
            if (tex.IsValid())
                ui->PublishExternalTextureRG(slot.uiKey, *pureFrame, tex);
            slot.uiBound = true;
        }
        else
        {
            // Hybrid: stable device handle (single buffer — changes only on
            // resize/evict), so this is churn-free after the first bind; the
            // device overload's no-change early-out absorbs repeats.
            // Same producer, same space as the RenderGraph arm above.
            ui->SetExternalTexture(slot.uiKey, slot.deviceTex, naturalW, naturalH,
                                   UI::UITextureSpace::DisplayLinearSdr());
            slot.uiBound = true;
        }
    }

    return changed;
}

// ---------------------------------------------------------------------------
// TickRender -- top-level orchestrator
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::TickRenderRG(uint64_t windowId, Rendering::RenderGraph::RGFrame& frame)
{
    RenderArm arm{};
    arm.frame = &frame;
    TickRenderImpl(windowId, arm);
}

void ModelThumbnailHandler::DrainPrewarmReadyMaterials()
{
    std::unordered_set<GUID> ready;
    {
        std::lock_guard lock(s_PrewarmReadyMaterialsMutex);
        ready.swap(s_PrewarmReadyMaterials);
    }
    for (const GUID& guid : ready)
    {
        if (auto asset = m_AssetManager->GetAsset(guid))
            PrewarmMaterialBaseShader(*m_RenderServices, *asset);
    }
}

void ModelThumbnailHandler::TickRenderImpl(uint64_t windowId, RenderArm arm)
{
    if (windowId == 0 || !m_AssetManager || !m_RenderServices)
        return;

    if (m_StaleCacheSweepPending)
    {
        m_StaleCacheSweepPending = false;
        EngineCore::GetInstance().GetJobSystem().EnqueueWork(
            [root = m_DiskCacheRoot]() { RemoveStaleModelCacheFiles(root); }, JobSystem::JobPriority::Background);
    }

    DrainPrewarmReadyMaterials();

    auto itWin = m_Windows.find(windowId);
    if (itWin == m_Windows.end())
    {
        // A folder bake needs a window to render in: while no window shows
        // thumbnails, the first one to tick takes it.
        if (m_BakeQueue.empty() || !m_Windows.empty())
            return;
        GetOrCreateWindowState(windowId);
        itWin = m_Windows.find(windowId);
    }

    WindowState& ws = itWin->second;
    if (arm.frame)
        ws.rg2Mode = true; // a window ticks on exactly one arm per session

    // Advance frame counter.
    ws.frameCounter++;

    // Use shared orbit angle so rotation persists when switching between models.
    ws.orbitAngleY = s_OrbitAngleY;

    // Auto-rotation: only when toggle is on and not click-paused. When toggle is off,
    // angle still advances when velocity is non-zero so mouse drag rotation works.
    const bool autoRotate = s_RotatePreviewsEnabled && !s_ClickPaused;
    const bool hasVelocity = std::fabs(s_OrbitVelocityY) > 1e-4f;
    const bool shouldAdvanceAngle = autoRotate || hasVelocity;
    if (shouldAdvanceAngle && !s_AngleAdvancedThisFrame)
    {
        s_OrbitAngleY += s_OrbitVelocityY * s_PreviewFrameScale;
        if (s_OrbitAngleY > kTwoPi)
            s_OrbitAngleY -= kTwoPi;
        s_AngleAdvancedThisFrame = true;
    }
    ws.orbitAngleY = s_OrbitAngleY;

    for (RenderLane& lane : ws.lanes)
        FinalizeInFlightRender(ws, lane);

    ReleaseDeferredModelImports();

    // While the browser scrolls, only the focused preview renders. The queue
    // lanes, their settle retries and the PNG readback resume afterwards;
    // cached images stay bound.
    if (IsAssetBrowserScrolling())
    {
        DispatchPreviewLane(ws, arm, shouldAdvanceAngle);
        return;
    }

    // Drain a completed disk-cache readback (if any) before declaring new
    // renders; the PNG encode runs on a worker.
    PollDiskCacheReadback();
    const bool imagesArrived = ShowBakedImages();
    if (!HasSlotWork(ws, imagesArrived))
        return;
    ResizeSlotTextures(ws);

    // Stale-spawn cleanup. A lane's spawned model is only useful while some
    // slot still holds its guid; once no slot has that guid (because it was
    // LRU-evicted or the cache forgot it), the entities orphan in the lane's
    // world and TickThumbnailSystems continues to animate them whenever any
    // other slot dispatches in that lane.
    //
    // Important: we check slot.occupied + matching guid only — NOT uiBound
    // and NOT slot.ready. Animated slots toggle between ready/in-flight
    // and bound/unbound every frame as their texture is re-rendered, and
    // gating teardown on those would cause spawn-teardown-spawn flicker
    // for animated thumbnails (RegisterReadyThumbnails removes the
    // uiBound flag while a slot is in-flight, briefly opening a window
    // where the check would fire mid-cycle).
    //
    // The eviction path (AcquireSlotForGuid) already tears down explicitly
    // when the slot is recycled. This safety net catches any other path
    // that drops the guid from the slot bookkeeping without explicitly
    // tearing down. Texture cache stays cached, so re-selecting the asset
    // re-spawns quickly.
    for (RenderLane& lane : ws.lanes)
    {
        if (lane.spawnedModelGuid.IsNull())
            continue;
        const bool slotHoldsSpawnGuid =
            std::any_of(ws.slots.begin(), ws.slots.end(), [&lane](const Slot& slot)
                        { return slot.occupied && slot.guid == lane.spawnedModelGuid; });
        if (!slotHoldsSpawnGuid)
            TeardownSpawnedModel(lane);
    }

    // Persist finished renders to the PNG disk cache. Runs before the dispatch
    // paths because ready slots typically have no pending entry left.
    if (arm.frame)
        MaybeStartDiskCacheReadbackRG(ws, *arm.frame);
    ReleaseBakedModelImports(ws);
    RequestBakedImages(ws);
    FinishBakes(ws);
    StartQueuedBakes(ws);
    PublishFolderBakeReport();

    DispatchPreviewLane(ws, arm, shouldAdvanceAngle);
    DispatchQueueLanes(ws, arm);
}

// GE_THUMBNAIL_CACHE_IDLE_BEGIN
bool ModelThumbnailHandler::HasSlotWork(WindowState& ws, bool imagesArrived)
{
    bool busy = imagesArrived || !ws.pending.empty() || !m_BakeQueue.empty() || m_BakeBatch.active ||
                m_PendingDiskCache.has_value() || !s_OrbitFocusGuid.IsNull() ||
                !s_MaterialOrbitFocusGuid.IsNull() || !s_PendingMaterialFocusGuid.IsNull() ||
                !s_LensFlareFocusGuid.IsNull();
    for (const RenderLane& lane : ws.lanes)
    {
        busy = busy || lane.inFlightSlot != kMaxResidentSlots ||
               ThumbnailCachePolicy::IsSpawnSettling(lane.SpawnedGuid(), lane.SpawnedGuid(),
                                                     lane.spawnedSettleFrames);
    }
    if (busy)
    {
        ws.quietTicks = 0;
        return true;
    }
    if (ws.quietTicks >= kQuietTicksBeforeIdle)
        return false;
    ++ws.quietTicks;
    return true;
}
// GE_THUMBNAIL_CACHE_IDLE_END

// ---------------------------------------------------------------------------
// TickRender sub-steps
// ---------------------------------------------------------------------------

void ModelThumbnailHandler::ReleaseSlotAssignment(WindowState& ws, size_t slotIdx)
{
    Slot& slot = ws.slots[slotIdx];
    for (RenderLane& lane : ws.lanes)
    {
        if (lane.inFlightSlot == slotIdx)
        {
            lane.inFlightSlot = kMaxResidentSlots;
            lane.inFlightFrame = 0;
        }
    }
    if (!slot.occupied)
        return;

    if (!slot.uiKey.empty())
        ws.evictedUiKeys.push_back(slot.uiKey);
    if (slot.isLensFlare)
        ws.guidToLensFlareSlot.erase(slot.guid);
    else if (slot.isMaterial)
        (slot.previewIblEnabled ? ws.guidToMaterialSlot : ws.guidToMaterialNoIblSlot).erase(slot.guid);
    else if (slot.listStaticThumb)
        ws.guidToListSlot.erase(slot.guid);
    else
        ws.guidToSlot.erase(slot.guid);
    ws.pendingSet.erase({slot.guid, slot.listStaticThumb, slot.isMaterial, slot.isLensFlare,
                         slot.previewIblEnabled});

    // If a lane spawned its model for the slot, tear it down there too.
    // Otherwise the spawned ECS entities (mesh + skinning runtime) would orphan
    // in the lane's world: SkinningUploadSystem keeps animating them next time
    // TickThumbnailSystems runs for a different slot in the same world, even
    // though no thumbnail reads them anymore. SpawnOrUpdateModel's "if guid
    // changed" guard only catches the case where another model takes over; a
    // release without a successor needs explicit cleanup.
    for (RenderLane& lane : ws.lanes)
    {
        if (lane.spawnedSlot == slotIdx)
            TeardownSpawnedModel(lane);
    }
    // A lane that still renders the model keeps it: the import moves to the
    // deferred releases, which unload it once that lane finishes.
    if (!ReleaseSlotModel(slot))
        DeferModelRelease(slot);

    slot.ready = false;
    slot.needsOrbitRerender = false;
    slot.materialRenderSettled = false;
    slot.animClip.reset();
    slot.animTime = 0.0f;
    slot.framingBoundsValid = false;
    slot.isMaterial = false;
    slot.isLensFlare = false;
    slot.previewIblEnabled = true;
}

// ---------------------------------------------------------------------------
// Material thumbnail -- GetOrRequest
// ---------------------------------------------------------------------------

std::string ModelThumbnailHandler::GetOrRequestMaterial(
    const std::filesystem::path& assetPath,
    int /*desiredSize*/,
    std::function<void(const std::string&)> onReady)
{
    Logger::Log::Trace("MaterialThumb: GetOrRequestMaterial called for '{}'", assetPath.string());
    if (!m_AssetManager)
    {
        if (onReady)
            onReady({});
        return {};
    }

    std::filesystem::path resolvedPath = assetPath;
    if (resolvedPath.is_absolute())
    {
        const std::filesystem::path& assetsRoot = m_AssetManager->GetAssetRoot();
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(resolvedPath, assetsRoot, ec);
        if (!ec && !rel.empty() && rel.native()[0] != L'.')
            resolvedPath = m_AssetManager->ResolveAssetPath(rel);
    }
    else
    {
        resolvedPath = m_AssetManager->ResolveAssetPath(resolvedPath);
    }

    // Same register-and-resolve entry point as ResolveModelThumbnailPath, so
    // every resolver in this file shares one contract.
    GUID guid = m_AssetManager->ResolveAssetGuid(resolvedPath);
    if (guid.IsNull() && resolvedPath != assetPath)
        guid = m_AssetManager->ResolveAssetGuid(assetPath);

    if (guid.IsNull())
    {
        Logger::Log::Warning("MaterialThumb: GUID is null for resolved path '{}' (original '{}')",
            resolvedPath.string(), assetPath.string());
        if (onReady)
            onReady({});
        return {};
    }

    // Trigger load so MaterialBuildService can compile the shader. Once the
    // asset is loaded, kick off an async worker-thread compile of the base
    // shader variant. Without this, EnsureMaterialReady on a later frame stalls
    // the main thread for ~150ms per material inside CompileMaterialPipeline →
    // shaderc; a folder full of unique materials freezes the editor by the
    // sum of those compiles. The worker populates ShaderCompilationCache
    // (mutex-guarded) so the eventual sync GetOrCompile is a cache hit.
    //
    // A single click on a material typically lands in three GetOrRequest
    // calls (asset grid cell + inspector header icon + Asset View preview),
    // each on the same frame for the same GUID. Dedup the prewarm dispatch
    // and the load-trigger by frame so we don't queue three identical
    // JobSystem tasks for the same material. The set is cleared in
    // BeginFrame.
    if (m_RenderServices && s_MaterialPrewarmedThisFrame.insert(guid).second)
    {
        if (auto cachedAsset = m_AssetManager->GetAsset(guid))
        {
            PrewarmMaterialBaseShader(*m_RenderServices, *cachedAsset);
        }
        else if (!m_AssetManager->IsLoadSuppressed(guid))
        {
            // The callback runs on the worker that completes the load and captures
            // nothing of this handler: it records the material for the main-thread
            // drain (DrainPrewarmReadyMaterials), where the prewarm may read
            // MaterialSystem's build context.
            m_AssetManager->LoadAsset(guid,
                [guid](Result<SharedPtr<Asset>, AssetError> r)
                {
                    if (!r.IsOk() || !r.Value())
                        return;
                    std::lock_guard lock(s_PrewarmReadyMaterialsMutex);
                    s_PrewarmReadyMaterials.insert(guid);
                });
        }
    }

    const std::string engineId = MakeMaterialEngineThumbnailId(guid, GetPreviewIblEnabled());
    Logger::Log::Trace("MaterialThumb: returning engineId='{}' for guid={}", engineId, guid.ToCompactString());
    if (onReady)
        onReady(engineId);
    return engineId;
}

std::string ModelThumbnailHandler::RequestLiveMaterialPreview(uint64_t windowId,
                                                              const GUID& materialGuid,
                                                              uint32_t widthPx,
                                                              uint32_t heightPx,
                                                              bool previewIblEnabled)
{
    if (windowId == 0 || materialGuid.IsNull() || !m_AssetManager)
        return {};

    SetPreviewSize(widthPx, heightPx);
    s_MaterialOrbitFocusGuid = materialGuid;
    s_MaterialOrbitFocusIblEnabled = previewIblEnabled;
    s_OrbitFocusGuid = GUID{};

    WindowState& ws = GetOrCreateWindowState(windowId);
    const size_t slotIdx = AcquireSlotForGuid(ws, materialGuid, false, true,
                                              previewIblEnabled);
    if (slotIdx >= ws.slots.size())
        return {};

    Slot& slot = ws.slots[slotIdx];

    if (!m_AssetManager->GetAsset(materialGuid) && !m_AssetManager->IsLoadSuppressed(materialGuid))
        m_AssetManager->LoadAsset(materialGuid, AssetLoadResultCallback{}, AssetLoadPriority::High);

    // The material is now the focused preview: the preview lane renders it
    // while it is not ready, and again for every request once it is.
    if (slot.ready)
        slot.needsOrbitRerender = true;

    return MakeMaterialEngineThumbnailId(materialGuid, previewIblEnabled);
}

} // namespace GameEngine
