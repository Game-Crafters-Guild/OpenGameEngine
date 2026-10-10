#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "ECS/Systems.h"
#include "Rendering/Core/Handle.h"
#include "Types/StringId.h"
#include "Types/Types.h"
#include "Video/VideoPlayer.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace GameEngine::Components
{
struct VideoTextureComponent;
}

namespace GameEngine::Rendering
{
class IDevice;
}

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Video
{

class VideoFrameUploader;

// Per-entity video playback state kept outside the ECS component (non-trivially-copyable).
struct VideoPlayerEntry
{
    VideoPlayer player;
    Rendering::TextureHandle texture;
    int textureWidth = 0;
    int textureHeight = 0;
    VideoPixelFormat textureFormat = VideoPixelFormat::RGBA8;

    // Raw videoPath this entry last loaded (or tried to). Entries are keyed by
    // entity handle, and a scene reload can reuse a handle — comparing against
    // the component's current path is what makes videoPath edits (inspector,
    // scene reload) take effect instead of being swallowed by a stale entry.
    std::string loadedPath;

    // Last material slot this entry's texture is bound to, so the override can
    // be released — component removed, material swapped, slot renamed — by
    // restoring the material's authored binding.
    GUID boundMaterialGuid{};
    StringId boundSlot{};
    // Last slot name that failed to resolve on the material (warn once per name).
    StringId warnedSlot{};
    // Set once this entry has warned that another entry already drives the
    // (material, slot) it wants. Cleared whenever this entry owns a binding.
    bool warnedSharedSlot = false;

    // Tick this entry was last matched by the component query. The reconcile
    // pass releases entries whose entity stopped carrying the component, and
    // stamping is what keeps that O(entries) instead of O(entries × live).
    uint64 lastSeenTick = 0;
};

// Drives VideoTextureComponent entities each frame:
//  - Creates/destroys VideoPlayer instances alongside component lifecycle
//  - Uploads whatever frame each player has ready to a persistent GPU texture
//  - Binds that texture over the named slot of the entity's MeshRenderer
//    material via TextureService::BindMaterialTexture (the bindless route the
//    draw path actually samples), restoring the authored binding on release
//
// The override lands on the MATERIAL, not the entity: every entity sharing that
// material asset shows the video. Give a mesh its own material to scope it.
// Only ONE entity may drive a given (material, slot); a second one is warned
// about and left unbound rather than fighting the first for the slot.
//
// A disabled entity (ECS::Disabled) is inert: it stops decoding, its GPU frame
// is released and the material returns to its authored texture — the same
// release the component's removal performs. Releasing the entry destroys the
// player, which JOINS its decode thread, so "stops decoding" covers the
// background work too. PLAYBACK POSITION IS NOT KEPT: a disable is a full release,
// so re-enabling reopens the source and starts again at t=0.
//
// This system never decodes: each player paces itself off the wall clock on its
// own thread and the tick samples whatever is ready, so a steady-state tick costs
// the same whether it arrives at 30 Hz or 3000 Hz.
//
// It is NOT free of blocking work, and it runs on the extraction wave. Two paths
// block the calling thread — both on entity lifecycle, neither per frame:
//   - LoadPlayer -> VideoPlayer::Load opens the container synchronously
//     (avformat_open_input plus a probe decode), on spawn and on every videoPath
//     edit.
//   - ReleaseEntry destroys the player, which JOINS its decode thread — and that
//     thread may be part-way through a colour conversion for a whole frame.
//     Component removal, cleared path, disable, entity destroy and scene close all
//     take this path.
// Neither can deadlock (the decode thread is not a job-system worker), but both
// stall the wave for as long as they take.
//
// Scheduled in SystemPhase::Extraction strictly after RenderExtraction and
// TerrainExtraction (see the Engine.cpp registration): this system writes
// material + TextureService binding state, and the material stack is not
// synchronized against the extraction-wave workers that read it.
class VideoTextureSystem : public ECS::ISystem
{
public:
    explicit VideoTextureSystem(Rendering::IDevice* device,
                                Engine::Renderer::RenderServices* renderServices);
    ~VideoTextureSystem() override;

    const char* GetName() const override { return "VideoTexture"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Queue submissions issued for frame uploads, and frames copied, since this
    // system was created. Config-independent: uploads track the sources' frame
    // rates and submissions track ticks in which any frame came due, so a tick
    // rate far above the sources' rates must leave both far below the tick count.
    uint64 GetUploadSubmitCount() const;
    uint64 GetUploadedFrameCount() const;

private:
    // Clears comp.playing when a fresh entry is created: that flag records that
    // the auto-start already fired, and its lifetime is the ENTRY's, not the
    // component's. A re-enabled entity gets a new entry, so playOnStart has to
    // be allowed to fire again or the reopened source would never play.
    VideoPlayerEntry& EnsureEntry(ECS::EntityHandle entity,
                                  Components::VideoTextureComponent& comp);
    void LoadPlayer(VideoPlayerEntry& entry, const Components::VideoTextureComponent& comp);
    // Create or re-create the entry's GPU texture when the source's dimensions or
    // decode format change. False when no usable texture exists.
    bool EnsureFrameTexture(VideoPlayerEntry& entry);
    // Copy the entry's ready frame into this tick's staging. The GPU copy itself
    // is recorded and submitted once, for every entry, at the end of Update.
    void StageFrame(VideoPlayerEntry& entry);
    void BindToMaterial(ECS::World& world, ECS::EntityHandle entity,
                        const Components::VideoTextureComponent& comp, VideoPlayerEntry& entry);
    // True when some entry other than `self` currently drives this slot.
    bool SlotDrivenByOtherEntry(const GUID& matGuid, StringId slot,
                                const VideoPlayerEntry& self) const;
    void RestoreBinding(VideoPlayerEntry& entry);
    // Give the material back its authored texture and park the GPU frame,
    // leaving the entry itself alive and inert.
    void ReleaseEntryResources(VideoPlayerEntry& entry);
    void ReleaseEntry(ECS::EntityHandle entity);

    // Drop every handle minted on a dead device generation, once per rebuild.
    // Handles are dropped, never discarded: the rebuild already destroyed the
    // GPU objects and reset the material bindless indices, so the next tick
    // re-creates the texture and re-asserts the override from scratch.
    void HealIfDeviceRebuilt();

    Rendering::IDevice* m_Device = nullptr;                       // not owned
    Engine::Renderer::RenderServices* m_RenderServices = nullptr; // not owned

    // One submission per tick for every video entity, out of persistent staging.
    std::unique_ptr<VideoFrameUploader> m_Uploader;

    uint64 m_DeviceRebuildGeneration = 0;
    uint64 m_Tick = 0;

    std::unordered_map<ECS::EntityHandle,
                       std::unique_ptr<VideoPlayerEntry>,
                       ECS::EntityHandleHash> m_Entries;
};

} // namespace GameEngine::Video
