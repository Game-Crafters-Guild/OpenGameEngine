#include "Engine/Video/VideoTextureSystem.h"

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Video/VideoTextureComponent.h"
#include "Core/Engine.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Video/VideoFrameUploader.h"
#include "Rendering/Core/Device.h"

#include "Logger/Logger.h"

#include <filesystem>
#include <vector>

namespace GameEngine::Video
{

namespace
{
// Both decode formats this system accepts are 4-byte-per-texel.
constexpr size_t kBytesPerVideoPixel = 4;

Rendering::TextureFormat TextureFormatForVideo(VideoPixelFormat format)
{
    return format == VideoPixelFormat::BGRA8
        ? Rendering::TextureFormat::BGRA8_SRGB
        : Rendering::TextureFormat::RGBA8_SRGB;
}
} // namespace

VideoTextureSystem::VideoTextureSystem(Rendering::IDevice* device,
                                       Engine::Renderer::RenderServices* renderServices)
    : m_Device(device)
    , m_RenderServices(renderServices)
    , m_Uploader(std::make_unique<VideoFrameUploader>(device))
{
}

VideoTextureSystem::~VideoTextureSystem()
{
    // Engine teardown destroys the SystemManager (and its systems) before
    // RenderServices shuts down, so releasing through TextureService is valid
    // here; its Shutdown drains anything still parked.
    std::vector<ECS::EntityHandle> handles;
    handles.reserve(m_Entries.size());
    for (const auto& [handle, entry] : m_Entries)
        handles.push_back(handle);
    for (ECS::EntityHandle handle : handles)
        ReleaseEntry(handle);
}

// deltaTime is deliberately unused: each player paces itself off the wall clock
// on its own decode thread. Feeding the tick's duration back into playback is
// what made the cost of a tick proportional to the previous tick's duration.
void VideoTextureSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_Device || !m_RenderServices)
        return;

    HealIfDeviceRebuilt();

    ++m_Tick;
    m_Uploader->BeginFrame();

    // The query is also the idle early-out: a world with no
    // VideoTextureComponent matches no archetypes and does no per-entity work.
    //
    // Disabled entities are skipped, and skipping them is also what releases
    // them: their entries miss this tick's stamp and the reconcile below runs
    // the same release a removed component gets. That is the right meaning here
    // because the override lands on a SHARED material — an entity that kept its
    // binding while disabled would still be dictating what every OTHER entity
    // using that material renders, and one that kept decoding would charge a
    // full decode + sws_scale + upload per frame for an entity that is not in
    // the world. Re-enabling reopens the source from the start.
    auto q = world.Query<ECS::Write<Components::VideoTextureComponent>>();
    q
     .Each([&](ECS::EntityHandle entity, Components::VideoTextureComponent& comp)
    {
        VideoPlayerEntry& entry = EnsureEntry(entity, comp);
        entry.lastSeenTick = m_Tick;

        // A scene reload can hand a reused entity handle a different component
        // value, and the inspector edits videoPath in place — reload whenever
        // the entry's source diverges from the component.
        if (entry.loadedPath != comp.VideoPath())
        {
            LoadPlayer(entry, comp);
            comp.playing = false; // let playOnStart re-fire for the new source
        }

        if (!entry.player.IsLoaded())
        {
            // Cleared or unloadable videoPath — an inspector typo is the common
            // case. Hand the material back its authored texture and park the
            // GPU frame; without this the entry keeps this tick's stamp, so the
            // reconcile below never reaches it and the material is stranded on
            // the last decoded frame for the rest of the session.
            //
            // The ENTRY stays: its loadedPath is what stops LoadPlayer from
            // re-opening the same bad file (a real avformat_open_input, plus a
            // warning) on every single tick. Idempotent — the release work only
            // runs on the loaded -> unloaded transition.
            ReleaseEntryResources(entry);
            return;
        }

        // Sync inspector-editable playback state every tick (cheap stores). The
        // player scales its own pacing by playbackSpeed and re-anchors only when
        // the value actually changes, so restating it every tick is free.
        entry.player.SetLoop(comp.loop);
        entry.player.SetPlaybackSpeed(comp.playbackSpeed);

        // Auto-start
        if (comp.playOnStart && !comp.playing && !entry.player.IsPlaying())
        {
            entry.player.Play();
            comp.playing = true;
        }

        // Sample, never decode: the answer is a flag the decode thread set, and
        // most ticks it is false because most ticks are shorter than one video
        // frame interval.
        if (entry.player.PollNewFrame())
            StageFrame(entry);

        // Re-assert the binding every tick, not only on new frames: a material
        // document re-apply or hot-reload repaints the slot with the authored
        // texture, and the per-frame rebind heals it next tick.
        //
        // Epoch-free only while the rebind is IDEMPOTENT: Material::SetTexture
        // and SetBindlessTextureIndex early-out on unchanged values, which
        // holds exactly when one entry drives a given (material, slot). Two
        // entries alternating handles in one slot would MarkDirty on every
        // tick and repack the whole material SSBO every frame — BindToMaterial
        // enforces single ownership so that cannot happen.
        if (entry.texture.IsValid())
            BindToMaterial(world, entity, comp, entry);
    });

    // Every entity's copy in one command list, one queue submission. A tick in
    // which no frame came due submits nothing at all.
    m_Uploader->Submit();

    // Release entries whose entity no longer carries the component (removed,
    // destroyed, or the world was replaced by a scene load).
    std::vector<ECS::EntityHandle> toRemove;
    for (const auto& [handle, entry] : m_Entries)
    {
        if (entry->lastSeenTick != m_Tick)
            toRemove.push_back(handle);
    }
    for (ECS::EntityHandle handle : toRemove)
        ReleaseEntry(handle);
}

uint64 VideoTextureSystem::GetUploadSubmitCount() const
{
    return m_Uploader->GetSubmitCount();
}

uint64 VideoTextureSystem::GetUploadedFrameCount() const
{
    return m_Uploader->GetUploadCount();
}

void VideoTextureSystem::HealIfDeviceRebuilt()
{
    const uint64 generation = m_Device->GetDeviceRebuildGeneration();
    if (generation == m_DeviceRebuildGeneration)
        return;
    m_DeviceRebuildGeneration = generation;

    // The staging ring's buffers and persistent maps died with the old device,
    // and the timeline its slot tokens name no longer exists.
    m_Uploader->ReprovisionAfterDeviceRebuild();

    for (auto& slot : m_Entries)
    {
        // Dropped, not discarded: the rebuild destroyed the GPU objects and
        // TextureService re-seeded every material slot with its bindless
        // default, so there is nothing to retire and nothing to restore.
        //
        // Every field derived from the dropped BINDING is reset, slot
        // ownership included, so the next tick re-races it cleanly. warnedSlot
        // is deliberately kept: it memoises a slot NAME the material does not
        // declare, which a device rebuild does not change, and re-warning per
        // rebuild would be noise about an unchanged condition.
        VideoPlayerEntry& entry = *slot.second;
        entry.texture = {};
        entry.textureWidth = 0;
        entry.textureHeight = 0;
        entry.boundMaterialGuid = {};
        entry.boundSlot = {};
        entry.warnedSharedSlot = false;
    }
}

VideoPlayerEntry& VideoTextureSystem::EnsureEntry(ECS::EntityHandle entity,
                                                  Components::VideoTextureComponent& comp)
{
    if (auto it = m_Entries.find(entity); it != m_Entries.end())
        return *it->second;

    auto entry = std::make_unique<VideoPlayerEntry>();
    entry->player.SetLoop(comp.loop);
    entry->player.SetPlaybackSpeed(comp.playbackSpeed);
    LoadPlayer(*entry, comp);
    // A fresh player has never auto-started, whatever the serialised component
    // says. Without this reset an entity that was disabled and re-enabled (its
    // entry released in between) would reopen the source with playing already
    // true, so playOnStart could not re-fire, the player would never be told to
    // Play(), and its decode thread would publish nothing for the rest of the
    // session.
    comp.playing = false;
    return *m_Entries.emplace(entity, std::move(entry)).first->second;
}

void VideoTextureSystem::LoadPlayer(VideoPlayerEntry& entry,
                                    const Components::VideoTextureComponent& comp)
{
    entry.loadedPath.assign(comp.VideoPath());
    entry.player.Unload();
    if (comp.videoPath[0] == '\0')
        return;

    // Relative paths anchor to the project asset root, never the process
    // cwd — the executable does not run from the project directory.
    std::filesystem::path path{comp.VideoPath()};
    if (path.is_relative() && EngineCore::GetInstance().IsInitialized())
        path = EngineCore::GetInstance().GetAssetManager().GetAssetRoot() / path;
    if (!entry.player.Load(path.string()))
        LOG_WARNING("VideoTextureSystem: failed to load '{}'", path.string());
}

bool VideoTextureSystem::EnsureFrameTexture(VideoPlayerEntry& entry)
{
    const int w = entry.player.GetWidth();
    const int h = entry.player.GetHeight();
    if (w <= 0 || h <= 0)
        return false;

    // Recreate the GPU texture if dimensions or decode format changed.
    const VideoPixelFormat frameFormat = entry.player.GetFramePixelFormat();
    if (!entry.texture.IsValid() ||
        w != entry.textureWidth ||
        h != entry.textureHeight ||
        frameFormat != entry.textureFormat)
    {
        Rendering::TextureDesc desc{};
        desc.width       = static_cast<uint32_t>(w);
        desc.height      = static_cast<uint32_t>(h);
        desc.depth       = 1;
        desc.mipLevels   = 1;
        desc.arrayLayers = 1;
        desc.format      = static_cast<uint32_t>(TextureFormatForVideo(frameFormat));
        desc.usage       = static_cast<uint32_t>(
            Rendering::TextureUsage::ShaderResource | Rendering::TextureUsage::TransferDst);
        desc.sampleCount = 1;
        desc.persistent  = true;
        // Resident in ShaderResource from birth so every upload — including the
        // first — is a ShaderResource -> CopyDest -> ShaderResource round trip.
        // That source state is what orders the copy after the previous frame's
        // sampling of this same image (VideoFrameUploader documents the trap).
        desc.initialState = Rendering::ResourceState::ShaderResource;
        desc.debugName   = "VideoTexture";

        // Create BEFORE parking the old one. A failed create must leave the
        // material bound to a texture that still exists: retiring first would
        // hand the drain a texture whose bindless index the material is still
        // carrying (BindToMaterial is skipped while entry.texture is invalid).
        const Rendering::TextureHandle replacement = m_Device->CreateTexture(desc);
        if (!replacement.IsValid())
            return false;

        // Park the old texture (and its bindless entries) for the render-thread
        // discard drain — never IDevice::DestroyTexture from a wave worker. The
        // replacement is bound to the material later this same tick, before the
        // drain runs, so no material is left sampling the retired descriptor.
        if (entry.texture.IsValid())
            m_RenderServices->Textures().DiscardTextureDeferred(entry.texture);

        entry.texture = replacement;
        entry.textureWidth  = w;
        entry.textureHeight = h;
        entry.textureFormat = frameFormat;
    }
    return true;
}

void VideoTextureSystem::StageFrame(VideoPlayerEntry& entry)
{
    if (!EnsureFrameTexture(entry))
        return;

    // Acquiring hands this thread the decoded frame; the decode thread cannot
    // write it again until the next acquire, so the pointer stays valid for the
    // memcpy into staging below.
    size_t frameBytes = 0;
    const uint8_t* pixels = entry.player.AcquireFramePointer(&frameBytes);
    const size_t rowPitch = static_cast<size_t>(entry.textureWidth) * kBytesPerVideoPixel;
    if (!pixels || frameBytes < rowPitch * static_cast<size_t>(entry.textureHeight))
        return;

    // A refused stage (slot still in flight, ring full) simply does not show this
    // frame: the texture keeps its previous contents and the next published frame
    // lands normally. Nothing is left half-written.
    m_Uploader->Stage(entry.texture, pixels,
                      static_cast<uint32_t>(entry.textureWidth),
                      static_cast<uint32_t>(entry.textureHeight), rowPitch);
}

void VideoTextureSystem::BindToMaterial(ECS::World& world, ECS::EntityHandle entity,
                                        const Components::VideoTextureComponent& comp,
                                        VideoPlayerEntry& entry)
{
    const auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
    const GUID matGuid = mr ? mr->materialAssetGuid.ToGuid() : GUID{};
    const StringId slotId = HashStringId(comp.UniformName());

    // Material swapped or slot renamed: return the previous slot to its
    // authored binding before overriding the new one.
    if (!entry.boundMaterialGuid.IsNull() &&
        (entry.boundMaterialGuid != matGuid || entry.boundSlot != slotId))
    {
        RestoreBinding(entry);
    }

    if (matGuid.IsNull())
        return;

    Engine::Renderer::Material* mat = m_RenderServices->Materials().Registry().Find(matGuid);
    if (!mat)
        return;

    if (!mat->HasTextureSlot(slotId))
    {
        if (entry.warnedSlot != slotId)
        {
            entry.warnedSlot = slotId;
            LOG_WARNING("VideoTextureSystem: uniformName '{}' resolves to no texture slot on "
                        "material '{}' — use a well-known slot name (e.g. 'albedoMap') or a name "
                        "the material's surface declares via @texture.",
                        comp.UniformName(), mat->GetName());
        }
        return;
    }

    // One entry per (material, slot). Two video entities pointed at the same
    // material would each rebind their own handle into the slot every tick, and
    // every flip is a Material::MarkDirty — the global content epoch moves each
    // frame and PackMaterialSSBO repacks the whole scene's materials forever.
    // First claimant keeps the slot; the runner-up stays unbound (and keeps
    // decoding into its own texture) until the owner releases, at which point
    // this scan lets it take over on the next tick.
    if (SlotDrivenByOtherEntry(matGuid, slotId, entry))
    {
        if (!entry.warnedSharedSlot)
        {
            entry.warnedSharedSlot = true;
            LOG_WARNING("VideoTextureSystem: material '{}' slot '{}' is already driven by another "
                        "video entity — this one will not bind. The override lands on the "
                        "material, so give each video mesh its own material.",
                        mat->GetName(), comp.UniformName());
        }
        return;
    }

    m_RenderServices->Textures().BindMaterialTexture(mat, slotId, entry.texture);
    entry.boundMaterialGuid = matGuid;
    entry.boundSlot = slotId;
    entry.warnedSharedSlot = false;
}

bool VideoTextureSystem::SlotDrivenByOtherEntry(const GUID& matGuid, StringId slot,
                                                const VideoPlayerEntry& self) const
{
    for (const auto& mapped : m_Entries)
    {
        const VideoPlayerEntry* other = mapped.second.get();
        if (other == &self)
            continue;
        if (other->boundMaterialGuid == matGuid && other->boundSlot == slot)
            return true;
    }
    return false;
}

void VideoTextureSystem::RestoreBinding(VideoPlayerEntry& entry)
{
    if (entry.boundMaterialGuid.IsNull())
        return;
    if (Engine::Renderer::Material* mat =
            m_RenderServices->Materials().Registry().Find(entry.boundMaterialGuid))
    {
        m_RenderServices->Textures().RestoreMaterialTextureBinding(
            mat, entry.boundMaterialGuid, entry.boundSlot);
    }
    entry.boundMaterialGuid = {};
    entry.boundSlot = {};
}

void VideoTextureSystem::ReleaseEntryResources(VideoPlayerEntry& entry)
{
    RestoreBinding(entry);
    if (entry.texture.IsValid())
    {
        m_RenderServices->Textures().DiscardTextureDeferred(entry.texture);
        entry.texture = {};
    }
    entry.textureWidth = 0;
    entry.textureHeight = 0;
    entry.warnedSharedSlot = false;
}

void VideoTextureSystem::ReleaseEntry(ECS::EntityHandle entity)
{
    auto it = m_Entries.find(entity);
    if (it == m_Entries.end())
        return;
    ReleaseEntryResources(*it->second);
    m_Entries.erase(it);
}

} // namespace GameEngine::Video
