#pragma once

#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
#include "Rendering/Core/Handle.h"
#include "Types/Types.h"

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine { namespace Rendering { class IDevice; } }
namespace GameEngine { class AssetEventDispatcher; }
namespace GameEngine { namespace Animation {
    class AnimationClip;
    class HumanoidRig;
    class RetargetMap;
    class RetargetNode;
    struct SkeletonData;
} }

namespace GameEngine { namespace Engine { namespace Renderer {

// Hard cap on bones per skeleton. Sized to fit the LDS budget of the fused
// retarget_full.comp (~16 KB / workgroup at 256 bones × 4 vec4 arrays).
// MetaHuman ~150 bones; humanoid rigs all fit comfortably.
constexpr uint32_t kRetargetMaxBones = 256;

// Sentinel for "no parent" / "no canonical mapping" / "non-canonical bone"
// in the GPU-side flat uint32 arrays. Mirrors the shader's `0xFFFFFFFFu`
// branch in retarget_full.comp.
constexpr uint32_t kRetargetInvalidIndex = 0xFFFFFFFFu;

// Uniform clip sample rate used by the GPU retargeting pipeline. 30 Hz is
// the chosen rate — humanoid locomotion is faithful at this rate, and
// pre-baked uniform samples eliminate keyframe binary search on the GPU.
constexpr float kRetargetClipSampleRate = 30.0f;

// Per-clip header entry in clipBuf. Packed std430. 32 bytes.
//
// Sample-major layout: per uniform-time sample s, the stream contains
//   boneCount rotation vec4s (in bone-rec order)
//   followed by transCount translation vec4s (in bone-rec order, indexed
//   via GPURetargetClipBoneRec::transSlot)
// Stride per sample = boneCount + transCount vec4. The shader reads bone b's
// rotation at sample s as `clip[firstSampleVec4 + s * stride + boneSlot]`,
// translation (if transSlot != kRetargetInvalidIndex) at
// `clip[firstSampleVec4 + s * stride + boneCount + transSlot]`.
struct GPURetargetClipHeader {
    uint32_t firstSampleVec4;       // offset into clipBuf where the sample stream starts
    uint32_t sampleCount;           // duration_seconds * kRetargetClipSampleRate + 1
    uint32_t boneCount;             // bones with rotation channels (size of per-sample rot block)
    float    duration;              // seconds
    uint32_t firstBoneVec4;         // offset into clipBuf of the per-bone records (boneCount entries)
    uint32_t transCount;            // bones with translation channels (size of per-sample trans block)
    uint32_t _pad0;
    uint32_t _pad1;
};
static_assert(sizeof(GPURetargetClipHeader) == 32, "GPURetargetClipHeader must be 32 bytes");

// Per-bone channel record, one per animated bone in a clip. 16 bytes.
//   boneIndex   — index in the source skeleton this bone targets
//   boneSlot    — position of this bone's rotation in the per-sample rot block
//   transSlot   — position of this bone's translation in the per-sample trans
//                 block, or kRetargetInvalidIndex if the clip has no
//                 translation channel for this bone (the common case for
//                 every bone except the hip)
struct GPURetargetClipBoneRec {
    uint32_t boneIndex;
    uint32_t boneSlot;
    uint32_t transSlot;
    uint32_t _pad;
};
static_assert(sizeof(GPURetargetClipBoneRec) == 16, "GPURetargetClipBoneRec must be 16 bytes");

// Per-rig-pair header in rigBuf. Packed std430. 80 bytes. Offsets are in
// vec4 units into rigBuf's flat data section. The shader fetches via
// `rigData[offset + idx]` patterns.
struct GPURigPairHeader {
    uint32_t srcBoneCount;
    uint32_t tgtBoneCount;
    uint32_t skinJointCount;

    // Source skeleton metadata.
    uint32_t srcParentIndices;          // 1 packed uint32 per src bone (4 bones / vec4)
    uint32_t srcDepthSortedTopo;        // 1 packed uint32 per src bone (parent-before-child)
    uint32_t srcRestRot;                // vec4 per src bone (rest local rotation; clip-shorter-than-skel fallback)

    // Target skeleton metadata.
    uint32_t tgtParentIndices;
    uint32_t tgtDepthSortedTopo;
    uint32_t tgtBindLocal;              // vec4 per tgt bone (AUTHORED bind local rotation, NOT retarget pose)
    uint32_t tgtRestPos;                // vec4 (xyz, 1) per tgt bone (rest local translation)

    // Per-target-bone canonical routing — SoA, two parallel arrays.
    uint32_t tgtSrcIdx;                 // 1 packed uint32 per tgt bone (kRetargetInvalidIndex for non-canonical)
    uint32_t tgtBindCorrection;         // vec4 per tgt bone (BindCorrection quat; identity for non-canonical)

    // Translation retargeting.
    uint32_t srcHipBoneIndex;
    uint32_t tgtHipBoneIndex;
    float    hipScale;                  // tgt_hip_height / src_hip_height
    uint32_t translationTgtBones;       // packed uint32 list (4 / vec4)
    uint32_t translationTgtBoneCount;

    // Skinning (consumed by the fused shader's pose-to-palette phase).
    uint32_t tgtInverseBind;            // 4 vec4 per tgt bone (mat4 column-major)
    uint32_t tgtJointNodes;             // 1 packed uint32 per skin joint (joint -> tgt bone)
    uint32_t tgtMeshRootInverse;        // 4 vec4 (single mat4)
};
static_assert(sizeof(GPURigPairHeader) == 80, "GPURigPairHeader must be 80 bytes");

// Per-character per-frame parameter block. Written by the CPU side once per
// frame for every active retargeted character; uploaded as a single contiguous
// memcpy. 32 bytes.
struct GPURetargetCharacterParams {
    uint32_t rigPairHeaderIdx;          // index into rigBuf's header table
    uint32_t clipHeaderIdx;             // index into clipBuf's header table
    float    clipTime;                  // seconds since clip start
    uint32_t paletteOffset;             // mat4 offset into SkinPaletteAtlas
    // The clip an animator crossfade blends from (AnimatorRef::PrevClipIndex),
    // or kRetargetInvalidIndex when the character is not blending. The source
    // pose is slerp(prev, current, blendAlpha) per bone, as the CPU path's
    // BlendLocalPoses computes it.
    uint32_t prevClipHeaderIdx = kRetargetInvalidIndex;
    float    prevClipTime = 0.0f;       // seconds since the previous clip's start
    float    blendAlpha = 0.0f;         // weight of the current clip; the kernel clamps it to [0, 1]
    uint32_t _pad = 0;
};
static_assert(sizeof(GPURetargetCharacterParams) == 32, "GPURetargetCharacterParams must be 32 bytes");

// Owns the persistent GPU SSBOs that feed retarget_full.comp:
//   - clipBuf : [GPURetargetClipHeader[N_clips]] [GPURetargetClipBoneRec[N_bones]] [vec4 samples[]]
//                Immutable per AnimationClip asset; uploaded at clip post-load.
//   - rigBuf  : [GPURigPairHeader[N_rigs]] [vec4 data[]]
//                Immutable per (srcRig, tgtRig, retargetMap) triple; uploaded
//                at retarget map post-load.
//   - charsBuf: GPURetargetCharacterParams[active_chars]
//                Mutable per frame; one CPU memcpy populates the whole array.
//
// All three buffers are device-local SSBOs grown as needed. The atlas (where
// the fused shader writes its mat4s) is the global SkinPaletteAtlas owned by
// PerFrameWritePool; this store does not allocate it.
//
// Lifecycle:
//   Initialize(device)               at engine boot
//   EnsureClipUploaded(guid, clip)   at AnimationClip post-load
//   EnsureRigPairUploaded(...)       at RetargetMap post-load
//   ResolveClipIdx / RigPairIdx      per-frame lookup (O(1) hash)
//   AppendCharacterParams(span)      per-frame add of charsBuf entries
//   BeginFrame(frame)                clears charsBuf + dispatch-scheduled guard
//   GetClip/Rig/CharsBuffer()        bound by RetargetFullPass
//   Shutdown()                       teardown
class RetargetGPUDataStore {
public:
    bool Initialize(GameEngine::Rendering::IDevice* device);
    void Shutdown();

    // Q6 slice 4 (§8-completion): zero the dead clip/rig/character SSBO handles +
    // capacities after a device rebuild (WITHOUT DestroyBuffer — the grow-path
    // double-free hazard) so CreateOrGrowBuffer recreates them; the CPU mirrors +
    // id->offset caches survive, so a re-flush restores identical GPU content.
    void ReprovisionAfterDeviceRebuild();

    void BeginFrame(uint32_t frameIndex);

    // Per-frame multi-view dispatch dedup (Scene + Game + thumbnails). The
    // render-graph builder in each view calls into BuildPasses; this guard
    // ensures the compute work happens only once per frame.
    bool IsDispatchScheduledThisFrame() const { return m_DispatchScheduledThisFrame; }
    void MarkDispatchScheduledThisFrame() { m_DispatchScheduledThisFrame = true; }

    // -- Asset-load uploads ----------------------------------------------

    // Pre-bakes the clip's per-bone channels into uniform 30 Hz samples and
    // appends them to clipBuf. Idempotent: returns the existing index if the
    // GUID has already been uploaded.
    //
    // Returns the clip header index, or kRetargetInvalidIndex on failure
    // (null clip, no animated bones, etc.).
    uint32_t EnsureClipUploaded(const GUID& guid, const Animation::AnimationClip& clip);

    // Serializes the rig pair (canonical Q values, BindCorrection, parent
    // indices, depth-sorted topo, inverse-bind matrices, ...) into rigBuf.
    // The CPU build state for the pair lives on a temporary RetargetNode used
    // only to compute the canonical routing table — caller passes a fully-
    // configured node whose Build() has already returned true.
    //
    // Returns the rig pair header index, or kRetargetInvalidIndex on failure.
    uint32_t EnsureRigPairUploaded(const GUID& mapGuid,
                                   const Animation::RetargetNode& built,
                                   const Animation::SkeletonData& srcSkel,
                                   const Animation::SkeletonData& tgtSkel,
                                   uint32_t skinJointCount);

    // Idempotent lookups. Return kRetargetInvalidIndex when not yet uploaded.
    uint32_t ResolveClipIdx(const GUID& guid) const;
    uint32_t ResolveRigPairIdx(const GUID& mapGuid) const;

    // Drop a previously-uploaded rig pair / clip. Frees the slot but doesn't
    // shrink the buffer (next upload may reuse the bytes via a free list).
    // Called when an asset is unloaded; idempotent if already absent.
    void ReleaseClip(const GUID& guid);
    void ReleaseRigPair(const GUID& mapGuid);

    // Subscribe to asset unload/reload events so the upload tables drop their
    // GUID -> headerIdx entries when the underlying asset goes away. Without
    // this, an editor session that hot-reloads many .retargetmap / clip assets
    // accumulates dead entries indefinitely. The lifetime is owned by the
    // store; detach happens automatically on Shutdown / destruction via the
    // AssetReloadInvalidator dtors.
    //
    // Idempotent: re-attaching to the same dispatcher resets the previous
    // subscription. Pass nullptr or skip the call entirely in headless test
    // paths that don't construct an AssetManager.
    void AttachAssetEvents(AssetEventDispatcher& dispatcher);

    // -- Per-frame writes ------------------------------------------------

    // Append per-frame character params. Multiple HumanoidRetargetSystem
    // instances (one per ticked world: main scene + each thumbnail world)
    // each contribute their own characters; the data store aggregates them
    // additively so all worlds share a single GPU dispatch. m_Chars is
    // cleared in BeginFrame so each frame starts empty.
    //
    // Pre-Tier-1 there was only a single ticking system instance and the
    // call replaced the array; that broke when multi-world ticking was
    // introduced because the second SetCharacterParams call would wipe
    // the first. AppendCharacterParams is the multi-instance-safe API.
    void AppendCharacterParams(std::span<const GPURetargetCharacterParams> params);

    // -- Buffer accessors ------------------------------------------------

    GameEngine::Rendering::BufferHandle GetClipBuffer()  const { return m_ClipBuf; }
    GameEngine::Rendering::BufferHandle GetRigBuffer()   const { return m_RigBuf; }
    GameEngine::Rendering::BufferHandle GetCharsBuffer() const { return m_CharsBuf; }

    // Used-byte ranges per buffer. Required when binding storage buffers in
    // a descriptor set: this engine's UpdateStorageBufferBinding treats
    // range=0 as "empty bind" rather than VK_WHOLE_SIZE, so callers must
    // supply explicit sizes.
    size_t GetClipUsedBytes()  const { return m_ClipData.size() * sizeof(uint32_t); }
    size_t GetRigUsedBytes()   const { return m_RigData.size()  * sizeof(uint32_t); }
    size_t GetCharsUsedBytes() const { return m_Chars.size()    * sizeof(GPURetargetCharacterParams); }

    uint32_t GetActiveCharacterCount() const { return m_ActiveCharCount; }

    // Returns true when `rtId` is one of the runtimes whose atlas slot was
    // reserved + scheduled for GPU compute write this frame. Used by
    // SkinningUploadSystem to skip its CPU upload (CPU and GPU racing on
    // the same atlas slot would produce flicker / stale frames).
    //
    // Populated by HumanoidRetargetSystem alongside AppendCharacterParams
    // via RegisterHandledRuntime; cleared in BeginFrame.
    bool IsRetargetGPUHandled(uint32_t rtId) const {
        return m_HandledRuntimes.find(rtId) != m_HandledRuntimes.end();
    }
    void RegisterHandledRuntime(uint32_t rtId) { m_HandledRuntimes.insert(rtId); }

    // Flush dirty CPU mirrors to GPU. Called once per frame before dispatch
    // by the render-graph compile path. Safe to call multiple times.
    void FlushIfDirty();

    // Aggregate resident-RAM accounting (CPU mirrors + device-side capacity).
    uint64_t GetResidentBytes() const { return m_ResidentBytes; }

    // -- Test seams ------------------------------------------------------

    // CPU mirror accessors for parity-test introspection. Mirror what
    // FlushIfDirty pushes to the GPU.
    std::span<const uint32_t> GetClipDataForTest() const { return m_ClipData; }
    std::span<const uint32_t> GetRigDataForTest() const { return m_RigData; }

private:
    GameEngine::Rendering::BufferHandle CreateOrGrowBuffer(
        GameEngine::Rendering::BufferHandle existing,
        size_t& currentCapacity,
        size_t requiredSize,
        const char* debugName);

    void UpdateCategoryBytes();

    GameEngine::Rendering::IDevice* m_Device = nullptr;

    // Clip data (immutable per asset).
    GameEngine::Rendering::BufferHandle m_ClipBuf;
    std::vector<uint32_t> m_ClipData;       // flat: [GPURetargetClipHeader[]][GPURetargetClipBoneRec[]][vec4 samples[]]
    size_t m_ClipBufCapacity = 0;
    bool m_ClipDirty = false;
    std::unordered_map<uint64_t, uint32_t> m_ClipGuidToHeaderIdx;  // packed-GUID -> idx into clip-header section

    // Rig data (immutable per rig-pair).
    GameEngine::Rendering::BufferHandle m_RigBuf;
    std::vector<uint32_t> m_RigData;        // flat: [GPURigPairHeader[]][vec4 data[]]
    size_t m_RigBufCapacity = 0;
    bool m_RigDirty = false;
    std::unordered_map<uint64_t, uint32_t> m_RigGuidToHeaderIdx;   // packed map-GUID -> idx into rig-header section

    // Per-frame character params.
    GameEngine::Rendering::BufferHandle m_CharsBuf;
    std::vector<GPURetargetCharacterParams> m_Chars;
    size_t m_CharsBufCapacity = 0;
    bool m_CharsDirty = false;
    uint32_t m_ActiveCharCount = 0;

    // Multi-view dispatch dedup.
    bool m_DispatchScheduledThisFrame = false;

    // Per-frame: runtime IDs whose atlas slots are owned by the GPU retarget
    // dispatch. Populated by HumanoidRetargetSystem via RegisterHandledRuntime;
    // consumed by SkinningUploadSystem's gate. Cleared in BeginFrame.
    std::unordered_set<uint32_t> m_HandledRuntimes;

    uint64_t m_ResidentBytes = 0;

    // Asset-event subscriptions. Wired by AttachAssetEvents to drop upload-
    // table entries when the underlying clip / retarget map is unloaded or
    // destroyed. RAII teardown — Shutdown / dtor releases the subscription.
    AssetReloadInvalidator m_ClipUnloadWatcher;
    AssetReloadInvalidator m_MapUnloadWatcher;

    // Pending releases queued by the dispatcher callbacks. AssetEventDispatcher
    // fires synchronously on whoever calls DispatchEvent (typically the asset
    // thread); EnsureClipUploaded / EnsureRigPairUploaded mutate the same
    // GUID->headerIdx tables from the system thread. Mirrors the pattern in
    // HumanoidRetargetSystem's reload watcher: the callback only pushes a GUID
    // under m_PendingReleaseMutex, and BeginFrame drains the queue from the
    // single-threaded site that already mutates these tables.
    std::mutex m_PendingReleaseMutex;
    std::vector<GUID> m_PendingClipReleases;
    std::vector<GUID> m_PendingMapReleases;
};

}}} // namespace GameEngine::Engine::Renderer
