#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "Rendering/Core/Handle.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
class CommandList;
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::TerrainECS
{

/// The GPU side of the height pages: the field's physical cache texture (R32F, square-packed
/// 130^2 slots, shared by every paged terrain) and each paged terrain's page-table words.
///
/// The main thread publishes a frame (Publish, after TerrainHeightPages::Update); the render side
/// latches the published state once per render-graph frame (Latch, from the terrain upload node)
/// and stages its slot uploads. The tables latched with uploads become readable (LatchedTable) only
/// when the upload pass that records those uploads is declared (CommitLatch); until then, and
/// whenever the staging cannot be created, readers keep the last committed tables, whose slots all
/// hold their pages. A consumer declared before the upload node therefore reads the previous
/// committed tables, one frame late. A released slot is quarantined for the frames in flight
/// before a new page is uploaded into it, and the quarantine counts main-thread updates, not
/// commits: while each frame's uploads commit within the frames in flight, those tables name no
/// slot this frame writes. Staging that fails for that many frames in a row, with a consumer
/// declared before the upload node and the scheduler running the copy first, can let the copy
/// overwrite a slot the committed tables still name. A device rebuild clears the quarantine with
/// the cache, which is correct: the new device has no frames in flight.
class TerrainHeightPageFeature : public Engine::Renderer::IRenderFeature
{
public:
    ~TerrainHeightPageFeature() override;

    /// Main thread: this frame's uploads (moved out of `pages`) and the words of every paged
    /// terrain whose table changed since the last publish.
    void Publish(TerrainHeightPages& pages);

    /// Render thread, once per render-graph frame (repeat calls in the same frame do nothing):
    /// takes what was published since the last latch, and frees the staging of uploads the GPU has
    /// finished with.
    void Latch(Rendering::IDevice& device, uint64 rgFrame);
    /// True when latched uploads are staged and wait for an upload pass.
    bool HasStagedUploads() const { return m_Staging.IsValid(); }
    /// Render thread, once the upload pass that will call FlushUploads is declared: the tables
    /// latched with the staged uploads become the readable ones.
    void CommitLatch();
    /// Records the staged uploads: clears the cache texture to 0 on first use, copies each slot and
    /// leaves the texture in ShaderResource.
    void FlushUploads(Rendering::CommandList& cl);

    /// The cache texture, invalid until a latch after the one whose upload pass cleared it: a
    /// consumer bound in the frame that creates it could run before that pass clears it (a draw
    /// at rest has no edge to the upload pass), so it reads the texture of a later frame, whose
    /// submission follows the clear.
    Rendering::TextureHandle CacheTexture() const { return m_CacheReadable ? m_Cache : Rendering::TextureHandle{}; }
    /// The committed words of paged terrain `terrain` and their version; false when it is not paged.
    bool LatchedTable(uint32 terrain, std::span<const uint32>& words, uint64& version) const;
    /// Bumped when the device was rebuilt: the main side restarts its cache (every page re-uploads).
    uint64 DeviceEpoch() const { return m_DeviceEpoch.load(std::memory_order_acquire); }

    void OnDeviceRebuilt(Rendering::IDevice* device) override;

private:
    // Words are shared, not copied, between the published, latched and committed sets: a table is
    // copied once, by Publish, when its version changes.
    struct Table
    {
        std::shared_ptr<const std::vector<uint32>> Words;
        uint64 Version = 0;
    };
    struct RetiringBuffer
    {
        Rendering::BufferHandle Buffer;
        uint64 Frame = 0;
    };

    void EnsureCache();
    void StageUploads();
    void RetireStaging();

    // Published by the main thread, under m_Mutex.
    std::mutex m_Mutex;
    std::vector<HeightPageUpload> m_PublishedUploads;
    std::unordered_map<uint32, Table> m_PublishedTables;
    uint32 m_CacheDim = 0;

    // Render thread only.
    Rendering::IDevice* m_Device = nullptr;
    std::vector<HeightPageUpload> m_LatchedUploads;
    std::unordered_map<uint32, Table> m_LatchedTables;   // latched with m_LatchedUploads
    std::unordered_map<uint32, Table> m_CommittedTables; // what LatchedTable reads
    Rendering::BufferHandle m_Staging;                   // m_LatchedUploads, staged
    std::vector<RetiringBuffer> m_Retiring;
    Rendering::TextureHandle m_Cache;
    bool m_CacheCleared = false;  // its clear is recorded (FlushUploads)
    bool m_CacheReadable = false; // its clear was recorded before this latch
    uint32 m_LatchedCacheDim = 0;
    uint64 m_LatchedFrame = ~0ull;
    std::atomic<uint64> m_DeviceEpoch{0};
};

} // namespace GameEngine::TerrainECS
