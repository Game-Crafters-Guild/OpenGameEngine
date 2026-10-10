#include "Engine/Rendering/RetargetGPUDataStore.h"

#include "Animation/AnimationClip.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/SkeletonData.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"
#include "Memory/AllocationCategory.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <cstring>

namespace GameEngine { namespace Engine { namespace Renderer {

namespace
{

// GUID -> 64-bit hash key for the per-asset upload tables. GUID is 128 bits;
// folding to 64 with a multiplicative mix is collision-safe for our cardinality
// (a few hundred clips and rig-pairs at most).
uint64_t PackGuid(const GUID& g)
{
    const auto& bytes = g.GetData();
    uint64_t lo = 0, hi = 0;
    std::memcpy(&lo, bytes.data(), 8);
    std::memcpy(&hi, bytes.data() + 8, 8);
    return lo ^ (hi * 0x9E3779B97F4A7C15ull);
}

// Reinterpret a float as its IEEE-754 uint32 bits.
inline uint32_t FloatBits(float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(float));
    return bits;
}

// Append a vec4 (4 floats) to a uint32 buffer. Returns the OFFSET in vec4
// units of the slot just written (matches the shader's std430 vec4[] view).
inline uint32_t PushVec4(std::vector<uint32_t>& buf, float x, float y, float z, float w)
{
    const uint32_t vec4Off = static_cast<uint32_t>(buf.size() / 4);
    buf.push_back(FloatBits(x));
    buf.push_back(FloatBits(y));
    buf.push_back(FloatBits(z));
    buf.push_back(FloatBits(w));
    return vec4Off;
}

inline uint32_t PushQuat(std::vector<uint32_t>& buf, const Mathematics::Quaternion& q)
{
    const auto& g = q.GetGLM();
    return PushVec4(buf, g.x, g.y, g.z, g.w);
}

inline uint32_t PushVec3AsVec4(std::vector<uint32_t>& buf, const Mathematics::Vector3& v)
{
    return PushVec4(buf, v.x, v.y, v.z, 1.0f);
}

// Pack N uint32s into a vec4-aligned buffer with 4 lanes per vec4. Returns
// the vec4 offset of the first 4-pack. Trailing partial pack is zero-padded.
// Shader access: `floatBitsToUint(rig.data[off + (i >> 2u)][i & 3u])`.
template <typename Iter>
inline uint32_t PushU32Packed(std::vector<uint32_t>& buf, Iter begin, Iter end)
{
    const uint32_t firstVec4 = static_cast<uint32_t>(buf.size() / 4);
    for (auto it = begin; it != end; ++it) buf.push_back(static_cast<uint32_t>(*it));
    while (buf.size() % 4 != 0) buf.push_back(0);
    return firstVec4;
}

// Slerp shortcut for pre-baking: pick the shorter great-circle path and
// renormalize. Matches Animation::ShortLerpQuat used by CPU sampling.
inline Mathematics::Quaternion ShortLerp(const Mathematics::Quaternion& a,
                                          const Mathematics::Quaternion& b,
                                          float t)
{
    const auto& ga = a.GetGLM();
    auto gb = b.GetGLM();
    if (glm::dot(ga, gb) < 0.0f) gb = -gb;
    const glm::quat lerped(
        ga.w + (gb.w - ga.w) * t,
        ga.x + (gb.x - ga.x) * t,
        ga.y + (gb.y - ga.y) * t,
        ga.z + (gb.z - ga.z) * t);
    return Mathematics::Quaternion(glm::normalize(lerped));
}

inline Mathematics::Vector3 LerpVec3(const Mathematics::Vector3& a,
                                      const Mathematics::Vector3& b,
                                      float t)
{
    return Mathematics::Vector3(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t);
}

// Sample a rotation channel at an arbitrary time. Mirrors the CPU clip
// sampler's behavior: clamp at endpoints, linear-search lower-bound, slerp.
Mathematics::Quaternion SampleRotation(const Animation::AnimChannel& ch, float time)
{
    if (ch.keys.empty()) return Mathematics::Quaternion::Identity();
    if (ch.keys.size() == 1)
    {
        const auto& k = ch.keys.front();
        return Mathematics::Quaternion(glm::quat(k.rotation[3], k.rotation[0], k.rotation[1], k.rotation[2]));
    }
    if (time <= ch.keys.front().time)
    {
        const auto& k = ch.keys.front();
        return Mathematics::Quaternion(glm::quat(k.rotation[3], k.rotation[0], k.rotation[1], k.rotation[2]));
    }
    if (time >= ch.keys.back().time)
    {
        const auto& k = ch.keys.back();
        return Mathematics::Quaternion(glm::quat(k.rotation[3], k.rotation[0], k.rotation[1], k.rotation[2]));
    }
    auto it = std::upper_bound(ch.keys.begin(), ch.keys.end(), time,
        [](float t, const Animation::AnimKeyframe& k) { return t < k.time; });
    const auto& hi = *it;
    const auto& lo = *(it - 1);
    const float dt = std::max(1e-6f, hi.time - lo.time);
    const float a = std::clamp((time - lo.time) / dt, 0.0f, 1.0f);
    const Mathematics::Quaternion qLo(glm::quat(lo.rotation[3], lo.rotation[0], lo.rotation[1], lo.rotation[2]));
    const Mathematics::Quaternion qHi(glm::quat(hi.rotation[3], hi.rotation[0], hi.rotation[1], hi.rotation[2]));
    return ShortLerp(qLo, qHi, a);
}

Mathematics::Vector3 SampleTranslation(const Animation::AnimChannel& ch, float time)
{
    if (ch.keys.empty()) return Mathematics::Vector3(0,0,0);
    if (ch.keys.size() == 1 || time <= ch.keys.front().time)
    {
        const auto& k = ch.keys.front();
        return Mathematics::Vector3(k.translation[0], k.translation[1], k.translation[2]);
    }
    if (time >= ch.keys.back().time)
    {
        const auto& k = ch.keys.back();
        return Mathematics::Vector3(k.translation[0], k.translation[1], k.translation[2]);
    }
    auto it = std::upper_bound(ch.keys.begin(), ch.keys.end(), time,
        [](float t, const Animation::AnimKeyframe& k) { return t < k.time; });
    const auto& hi = *it;
    const auto& lo = *(it - 1);
    const float dt = std::max(1e-6f, hi.time - lo.time);
    const float a = std::clamp((time - lo.time) / dt, 0.0f, 1.0f);
    return LerpVec3(
        Mathematics::Vector3(lo.translation[0], lo.translation[1], lo.translation[2]),
        Mathematics::Vector3(hi.translation[0], hi.translation[1], hi.translation[2]),
        a);
}

} // namespace

bool RetargetGPUDataStore::Initialize(GameEngine::Rendering::IDevice* device)
{
    if (!device) return false;
    m_Device = device;

    // Buffers are created lazily on first upload via CreateOrGrowBuffer. Zero
    // initial capacity keeps the store cheap when no retargeted characters
    // are spawned.
    m_ClipBuf = {};
    m_RigBuf = {};
    m_CharsBuf = {};
    m_ClipBufCapacity = 0;
    m_RigBufCapacity = 0;
    m_CharsBufCapacity = 0;

    UpdateCategoryBytes();
    return true;
}

void RetargetGPUDataStore::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;

    // Drop the dead SSBO handles WITHOUT DestroyBuffer (CreateOrGrowBuffer's grow-path
    // double-free) and zero the capacities so they recreate. The CPU mirrors + the
    // clip/rig/char id->offset caches survive, so re-flushing produces identical GPU
    // content and cached ids keep resolving.
    m_ClipBuf  = {};
    m_RigBuf   = {};
    m_CharsBuf = {};
    m_ClipBufCapacity  = 0;
    m_RigBufCapacity   = 0;
    m_CharsBufCapacity = 0;
    m_ClipDirty  = !m_ClipData.empty();
    m_RigDirty   = !m_RigData.empty();
    m_CharsDirty = !m_Chars.empty();
    FlushIfDirty();
}

void RetargetGPUDataStore::Shutdown()
{
    // Detach asset-event subscriptions before tearing buffers / tables down so
    // a late-arriving event from another thread doesn't reach into a half-
    // shutdown state.
    m_ClipUnloadWatcher.Reset();
    m_MapUnloadWatcher.Reset();

    if (m_Device)
    {
        if (m_ClipBuf.IsValid())  m_Device->DestroyBuffer(m_ClipBuf);
        if (m_RigBuf.IsValid())   m_Device->DestroyBuffer(m_RigBuf);
        if (m_CharsBuf.IsValid()) m_Device->DestroyBuffer(m_CharsBuf);
    }
    m_ClipBuf = {};
    m_RigBuf = {};
    m_CharsBuf = {};
    m_ClipData.clear();
    m_RigData.clear();
    m_Chars.clear();
    m_ClipGuidToHeaderIdx.clear();
    m_RigGuidToHeaderIdx.clear();
    m_ClipBufCapacity = 0;
    m_RigBufCapacity = 0;
    m_CharsBufCapacity = 0;
    m_ClipDirty = m_RigDirty = m_CharsDirty = false;
    m_ActiveCharCount = 0;
    m_DispatchScheduledThisFrame = false;
    m_Device = nullptr;
    UpdateCategoryBytes();
}

void RetargetGPUDataStore::BeginFrame(uint32_t /*frameIndex*/)
{
    m_DispatchScheduledThisFrame = false;
    m_HandledRuntimes.clear();

    // Drain queued asset-unload releases (AttachAssetEvents callbacks).
    // Swap-and-process so the release calls run outside the queue lock — the
    // dispatcher callback only takes m_PendingReleaseMutex, never the table
    // mutex (the data-store tables aren't locked; they're single-threaded
    // owned by this frame-start site + the system update wave).
    std::vector<GUID> clipReleases;
    std::vector<GUID> mapReleases;
    {
        std::lock_guard<std::mutex> lk(m_PendingReleaseMutex);
        clipReleases.swap(m_PendingClipReleases);
        mapReleases.swap(m_PendingMapReleases);
    }
    for (const auto& g : clipReleases) ReleaseClip(g);
    for (const auto& g : mapReleases)  ReleaseRigPair(g);

    // Clear per-frame character params so each ticked HumanoidRetargetSystem
    // instance (main scene + each thumbnail world) starts from an empty list
    // and contributes additively via AppendCharacterParams. Without this
    // clear, the previous frame's characters would leak forward; without
    // additive semantics on the append, the second instance would wipe the
    // first (the staggering bug observed when both scene and thumbnail
    // animate the same skeleton).
    m_Chars.clear();
    m_ActiveCharCount = 0;
    m_CharsDirty = true;
}

uint32_t RetargetGPUDataStore::EnsureClipUploaded(const GUID& guid, const Animation::AnimationClip& clip)
{
    const uint64_t key = PackGuid(guid);
    if (auto it = m_ClipGuidToHeaderIdx.find(key); it != m_ClipGuidToHeaderIdx.end())
        return it->second;

    const auto& channels = clip.GetChannels();
    if (channels.empty()) return kRetargetInvalidIndex;

    // Group channels by source-skeleton bone index; stash the rotation +
    // translation channels each bone has. The clip's `boneIndex` is the
    // source-skeleton index the channel targets.
    struct BoneChannels {
        const Animation::AnimChannel* rot = nullptr;
        const Animation::AnimChannel* trans = nullptr;
    };
    std::unordered_map<uint32_t, BoneChannels> byBone;
    for (const auto& ch : channels)
    {
        auto& slot = byBone[ch.boneIndex];
        if (ch.path == Animation::AnimPath::Rotation && !slot.rot) slot.rot = &ch;
        else if (ch.path == Animation::AnimPath::Translation && !slot.trans) slot.trans = &ch;
        // Scale channels are ignored for retarget — humanoid animation never
        // uses them at runtime; the rest scale carries through unchanged.
    }
    if (byBone.empty()) return kRetargetInvalidIndex;

    const float duration = std::max(1.0f / kRetargetClipSampleRate, clip.GetDuration());
    const uint32_t sampleCount = std::max<uint32_t>(2u,
        static_cast<uint32_t>(std::ceil(duration * kRetargetClipSampleRate)) + 1u);

    // Layout: [GPURetargetClipHeader (8 uints)] [GPURetargetClipBoneRec[N] (4 uints each)]
    //         [vec4 sample stream: rot+trans interleaved per sample, all bones contiguous]
    //
    // The shader treats the ENTIRE clip buffer as a vec4[] flat array;
    // offsets stored in headers/records are vec4 indices.

    // Reserve header.
    constexpr uint32_t kHeaderVec4Size = sizeof(GPURetargetClipHeader) / 16; // 32B = 2 vec4
    constexpr uint32_t kBoneRecVec4Size = sizeof(GPURetargetClipBoneRec) / 16; // 16B = 1 vec4
    static_assert(sizeof(GPURetargetClipHeader) == 32 && sizeof(GPURetargetClipBoneRec) == 16);

    const uint32_t headerVec4Off = static_cast<uint32_t>(m_ClipData.size() / 4);
    const uint32_t boneCount = static_cast<uint32_t>(byBone.size());

    // SAMPLE-MAJOR layout: all rotations for sample 0 across bones,
    // then all translations for sample 0 (hip-only — see channelMask), then
    // sample 1, ..., for cache-friendly parallel sampling. The shader's
    // Phase 1b loop has each thread handle a different bone at the same
    // clipTime; sample-major puts those reads on adjacent vec4 slots so
    // a single warp's reads coalesce into one cache line.
    //
    // Layout per sample stride:
    //   [bone0.rot, bone1.rot, ..., boneN-1.rot]                   (N vec4)
    //   [hipBone.trans] only if any bone has translation channel    (0 or 1 vec4)
    // Total per sample = N + transBoneCount vec4. Since only the hip
    // typically has translation, transBoneCount is 0 or 1 in practice.
    //
    // bone-rec carries (boneIndex, slotIndexWithinSampleStride) where the
    // slot index is the bone's position in the per-sample rotation block,
    // and channelMask carries whether that bone also writes a translation
    // slot in this clip's translation block.

    // Sort bones for deterministic ordering.
    std::vector<uint32_t> boneIndices;
    boneIndices.reserve(boneCount);
    for (const auto& kv : byBone) boneIndices.push_back(kv.first);
    std::sort(boneIndices.begin(), boneIndices.end());

    // Identify translation bones (subset of boneIndices). Per the v3 plan
    // (§5 retarget_full.comp Phase 3) only the hip's translation is
    // consumed, but we keep the layout flexible: any bone whose channel
    // pair has a translation channel gets a slot.
    std::vector<uint32_t> transBoneIndices; // boneIdx values that have trans channel
    transBoneIndices.reserve(boneCount);
    for (uint32_t bi : boneIndices)
        if (byBone[bi].trans) transBoneIndices.push_back(bi);
    const uint32_t transCount = static_cast<uint32_t>(transBoneIndices.size());

    const uint32_t sampleStrideVec4 = boneCount + transCount;
    const uint32_t firstSampleVec4Block = headerVec4Off + kHeaderVec4Size + boneCount * kBoneRecVec4Size;

    m_ClipData.reserve(m_ClipData.size() + (kHeaderVec4Size + boneCount * kBoneRecVec4Size + sampleStrideVec4 * sampleCount) * 4);

    // Header. Backfilled after — for now reserve the slot.
    GPURetargetClipHeader hdr{};
    hdr.firstSampleVec4 = firstSampleVec4Block;
    hdr.sampleCount     = sampleCount;
    hdr.boneCount       = boneCount;
    hdr.duration        = duration;
    hdr.firstBoneVec4   = headerVec4Off + kHeaderVec4Size;
    hdr.transCount      = transCount;
    for (uint32_t i = 0; i < kHeaderVec4Size * 4; ++i) m_ClipData.push_back(0);

    // Bone records. boneSlot identifies the bone's position in the
    // per-sample rotation block; transSlot points into the translation
    // block (kRetargetInvalidIndex if no translation channel for this bone).
    std::unordered_map<uint32_t, uint32_t> transSlotByBone;
    for (uint32_t i = 0; i < transCount; ++i)
        transSlotByBone[transBoneIndices[i]] = i;
    for (uint32_t bi = 0; bi < boneCount; ++bi)
    {
        const uint32_t boneIdx = boneIndices[bi];
        GPURetargetClipBoneRec rec{};
        rec.boneIndex   = boneIdx;
        rec.boneSlot    = bi;
        auto it = transSlotByBone.find(boneIdx);
        rec.transSlot   = (it != transSlotByBone.end()) ? it->second : kRetargetInvalidIndex;
        for (uint32_t i = 0; i < kBoneRecVec4Size * 4; ++i)
            m_ClipData.push_back(reinterpret_cast<const uint32_t*>(&rec)[i]);
    }

    // Sample stream — sample-major. For each uniform-time sample s:
    //   write boneCount rotation vec4s in boneIndices[] order,
    //   followed by transCount translation vec4s in transBoneIndices[] order.
    for (uint32_t s = 0; s < sampleCount; ++s)
    {
        const float t = static_cast<float>(s) / kRetargetClipSampleRate;
        for (uint32_t bi = 0; bi < boneCount; ++bi)
        {
            const auto& chans = byBone[boneIndices[bi]];
            const Mathematics::Quaternion rot =
                chans.rot ? SampleRotation(*chans.rot, t) : Mathematics::Quaternion::Identity();
            PushQuat(m_ClipData, rot);
        }
        for (uint32_t ti = 0; ti < transCount; ++ti)
        {
            const auto& chans = byBone[transBoneIndices[ti]];
            const Mathematics::Vector3 trans =
                chans.trans ? SampleTranslation(*chans.trans, t) : Mathematics::Vector3(0,0,0);
            PushVec3AsVec4(m_ClipData, trans);
        }
    }

    // Backfill the header into the reserved slot.
    std::memcpy(m_ClipData.data() + headerVec4Off * 4, &hdr, sizeof(hdr));

    // Index returned to the caller is the vec4 offset of the header in
    // clipBuf — the shader fetches the header struct via two vec4 loads at
    // [clipHeaderIdx + 0] and [clipHeaderIdx + 1].
    m_ClipGuidToHeaderIdx[key] = headerVec4Off;
    m_ClipDirty = true;
    UpdateCategoryBytes();
    return headerVec4Off;
}

uint32_t RetargetGPUDataStore::EnsureRigPairUploaded(const GUID& mapGuid,
                                                     const Animation::RetargetNode& built,
                                                     const Animation::SkeletonData& /*srcSkel*/,
                                                     const Animation::SkeletonData& tgtSkel,
                                                     uint32_t skinJointCount)
{
    const uint64_t key = PackGuid(mapGuid);
    if (auto it = m_RigGuidToHeaderIdx.find(key); it != m_RigGuidToHeaderIdx.end())
        return it->second;

    Animation::RetargetNode::GPUExport ex;
    built.ExportForGPU(ex);
    if (ex.SourceBoneCount == 0 || ex.TargetBoneCount == 0) return kRetargetInvalidIndex;

    // Header reservation. GPURigPairHeader is 80 bytes = 5 vec4. We push it
    // last (after data is written and we know all the offsets), so reserve
    // the slot first by inserting placeholder zeros.
    constexpr uint32_t kHeaderVec4Size = sizeof(GPURigPairHeader) / 16;
    static_assert(sizeof(GPURigPairHeader) == 80);
    const uint32_t headerVec4Off = static_cast<uint32_t>(m_RigData.size() / 4);
    for (uint32_t i = 0; i < kHeaderVec4Size * 4; ++i) m_RigData.push_back(0);

    GPURigPairHeader hdr{};
    hdr.srcBoneCount   = ex.SourceBoneCount;
    hdr.tgtBoneCount   = ex.TargetBoneCount;
    hdr.skinJointCount = skinJointCount;

    // Helper: convert int32 (with -1 = no parent) to packed uint (with
    // kRetargetInvalidIndex sentinel).
    auto sentinelize = [](int32 v) -> uint32_t {
        return (v < 0) ? kRetargetInvalidIndex : static_cast<uint32_t>(v);
    };

    // Source skeleton.
    {
        std::vector<uint32_t> packedParents;
        packedParents.reserve(ex.SourceBoneCount);
        for (int32 p : ex.SourceParent) packedParents.push_back(sentinelize(p));
        hdr.srcParentIndices = PushU32Packed(m_RigData, packedParents.begin(), packedParents.end());
        hdr.srcDepthSortedTopo = PushU32Packed(m_RigData, ex.SourceTopo.begin(), ex.SourceTopo.end());
        const uint32_t restRotOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& q : ex.SourceRestRot) PushQuat(m_RigData, q);
        hdr.srcRestRot = restRotOff;
    }

    // Target skeleton.
    {
        std::vector<uint32_t> packedParents;
        packedParents.reserve(ex.TargetBoneCount);
        for (int32 p : ex.TargetParent) packedParents.push_back(sentinelize(p));
        hdr.tgtParentIndices = PushU32Packed(m_RigData, packedParents.begin(), packedParents.end());
        hdr.tgtDepthSortedTopo = PushU32Packed(m_RigData, ex.TargetTopo.begin(), ex.TargetTopo.end());

        const uint32_t bindLocalOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& q : ex.TargetBindLocal) PushQuat(m_RigData, q);
        hdr.tgtBindLocal = bindLocalOff;

        const uint32_t restPosOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& v : ex.TargetRestPos) PushVec3AsVec4(m_RigData, v);
        hdr.tgtRestPos = restPosOff;
    }

    // Per-target-bone canonical routing.
    hdr.tgtSrcIdx = PushU32Packed(m_RigData, ex.TargetSourceIdx.begin(), ex.TargetSourceIdx.end());
    {
        const uint32_t bindCorrOff = static_cast<uint32_t>(m_RigData.size() / 4);
        for (const auto& q : ex.TargetBindCorrection) PushQuat(m_RigData, q);
        hdr.tgtBindCorrection = bindCorrOff;
    }

    // Translation.
    hdr.srcHipBoneIndex = (ex.SourceHipBoneIndex == ~0u) ? kRetargetInvalidIndex : ex.SourceHipBoneIndex;
    hdr.tgtHipBoneIndex = (ex.TargetHipBoneIndex == ~0u) ? kRetargetInvalidIndex : ex.TargetHipBoneIndex;
    hdr.hipScale        = ex.HipScale;
    hdr.translationTgtBones     = ex.TranslationTargetBones.empty()
        ? 0u
        : PushU32Packed(m_RigData, ex.TranslationTargetBones.begin(), ex.TranslationTargetBones.end());
    hdr.translationTgtBoneCount = static_cast<uint32_t>(ex.TranslationTargetBones.size());

    // Skinning data sourced directly from SkeletonData (no recomputation).

    // Inverse-bind matrices: SkeletonData carries them precomputed per bone.
    // Falls back to identity ONLY in debug paths (synthetic skeletons used in
    // unit tests); production rigs always populate this.
    {
        const uint32_t invBindOff = static_cast<uint32_t>(m_RigData.size() / 4);
        const size_t expectedFloats = static_cast<size_t>(ex.TargetBoneCount) * 16;
        if (tgtSkel.InverseBind.size() >= expectedFloats)
        {
            for (uint32_t b = 0; b < ex.TargetBoneCount; ++b)
            {
                const float* p = tgtSkel.InverseBind.data() + b * 16;
                for (int i = 0; i < 16; i += 4)
                    PushVec4(m_RigData, p[i], p[i + 1], p[i + 2], p[i + 3]);
            }
        }
        else
        {
            Logger::Log::Warning("[RetargetGPUDataStore] tgtSkel.InverseBind missing for rig pair "
                                  "(expected {} floats, got {}). Producing identity skinning matrices — "
                                  "renderer output will be wrong.",
                                  expectedFloats, tgtSkel.InverseBind.size());
            for (uint32_t b = 0; b < ex.TargetBoneCount; ++b)
            {
                PushVec4(m_RigData, 1, 0, 0, 0);
                PushVec4(m_RigData, 0, 1, 0, 0);
                PushVec4(m_RigData, 0, 0, 1, 0);
                PushVec4(m_RigData, 0, 0, 0, 1);
            }
        }
        hdr.tgtInverseBind = invBindOff;
    }

    // Joint-node table: skin-joint -> tgt-skeleton-bone.
    //
    // FBX-imported skeletons sometimes leave JointNodes empty + SkinJointCount=0;
    // their CPU BuildPoseToSkinMatrices falls back to "joint j == bone j"
    // identity mapping. Mirror that here when caller passes skinJointCount
    // == BoneCount but JointNodes is empty.
    if (tgtSkel.JointNodes.empty() && skinJointCount == tgtSkel.BoneCount)
    {
        std::vector<uint32_t> identityMap(skinJointCount);
        for (uint32_t j = 0; j < skinJointCount; ++j) identityMap[j] = j;
        hdr.tgtJointNodes = PushU32Packed(m_RigData, identityMap.begin(), identityMap.end());
    }
    else if (tgtSkel.JointNodes.size() == skinJointCount)
    {
        hdr.tgtJointNodes = PushU32Packed(m_RigData, tgtSkel.JointNodes.begin(), tgtSkel.JointNodes.end());
    }
    else
    {
        Logger::Log::Error("[RetargetGPUDataStore] JointNodes size mismatch: tgtSkel has {} entries, "
                            "caller passed skinJointCount={} (BoneCount={}). GPU upload aborted.",
                            tgtSkel.JointNodes.size(), skinJointCount, tgtSkel.BoneCount);
        m_RigData.resize(headerVec4Off * 4);
        return kRetargetInvalidIndex;
    }

    // Mesh root inverse: invert SkeletonData's MeshRootWorld at upload time.
    // For mesh nodes at non-identity world transforms (common for grouped
    // imports), this is the difference between correct skinning and visible
    // offset bugs.
    {
        const uint32_t mriOff = static_cast<uint32_t>(m_RigData.size() / 4);
        glm::mat4 mrw{};
        std::memcpy(&mrw, tgtSkel.MeshRootWorld, sizeof(glm::mat4));
        const glm::mat4 mri = glm::inverse(mrw);
        const float* p = reinterpret_cast<const float*>(&mri);
        for (int i = 0; i < 16; i += 4)
            PushVec4(m_RigData, p[i], p[i + 1], p[i + 2], p[i + 3]);
        hdr.tgtMeshRootInverse = mriOff;
    }

    // Backfill the header into the reserved slot.
    const uint32_t hdrUintOff = headerVec4Off * 4;
    std::memcpy(m_RigData.data() + hdrUintOff, &hdr, sizeof(hdr));

    m_RigGuidToHeaderIdx[key] = headerVec4Off;
    m_RigDirty = true;
    UpdateCategoryBytes();
    return headerVec4Off;
}

uint32_t RetargetGPUDataStore::ResolveClipIdx(const GUID& guid) const
{
    auto it = m_ClipGuidToHeaderIdx.find(PackGuid(guid));
    return (it != m_ClipGuidToHeaderIdx.end()) ? it->second : kRetargetInvalidIndex;
}

uint32_t RetargetGPUDataStore::ResolveRigPairIdx(const GUID& mapGuid) const
{
    auto it = m_RigGuidToHeaderIdx.find(PackGuid(mapGuid));
    return (it != m_RigGuidToHeaderIdx.end()) ? it->second : kRetargetInvalidIndex;
}

void RetargetGPUDataStore::ReleaseClip(const GUID& guid)
{
    m_ClipGuidToHeaderIdx.erase(PackGuid(guid));
    // The clip's bytes stay in m_ClipData until reuploaded — a future free-
    // list could reclaim them. Phase 1 accepts the small leak; clip uploads
    // happen at asset-load time, not per frame.
}

void RetargetGPUDataStore::ReleaseRigPair(const GUID& mapGuid)
{
    m_RigGuidToHeaderIdx.erase(PackGuid(mapGuid));
}

void RetargetGPUDataStore::AttachAssetEvents(AssetEventDispatcher& dispatcher)
{
    // Idempotent — replaces any previous subscription. The Reset() inside the
    // assignment-from-rvalue path of AssetReloadInvalidator handles cleanup of
    // the prior callback handle, so calling this twice (e.g. after a hot-
    // reload of AssetManager itself) doesn't accumulate dispatcher slots.
    //
    // AssetEventDispatcher fires synchronously on whoever called DispatchEvent
    // (typically the asset thread). The system thread mutates the same
    // GUID->headerIdx tables via EnsureClipUploaded / EnsureRigPairUploaded.
    // Direct mutation from the callback would race; instead, the callback only
    // queues the GUID and BeginFrame drains the queue from the single-threaded
    // frame-start site. Mirrors the deferred-queue pattern at
    // HumanoidRetargetSystem.cpp:323-337.
    m_ClipUnloadWatcher = AssetReloadInvalidator(
        dispatcher, AssetType::Animation,
        [this](const GUID& guid) {
            std::lock_guard<std::mutex> lk(m_PendingReleaseMutex);
            m_PendingClipReleases.push_back(guid);
        });

    m_MapUnloadWatcher = AssetReloadInvalidator(
        dispatcher, AssetType::RetargetMap,
        [this](const GUID& guid) {
            std::lock_guard<std::mutex> lk(m_PendingReleaseMutex);
            m_PendingMapReleases.push_back(guid);
        });
}

void RetargetGPUDataStore::AppendCharacterParams(std::span<const GPURetargetCharacterParams> params)
{
    m_Chars.insert(m_Chars.end(), params.begin(), params.end());
    m_ActiveCharCount = static_cast<uint32_t>(m_Chars.size());
    m_CharsDirty = true;
    UpdateCategoryBytes();
}

GameEngine::Rendering::BufferHandle RetargetGPUDataStore::CreateOrGrowBuffer(
    GameEngine::Rendering::BufferHandle existing,
    size_t& currentCapacity,
    size_t requiredSize,
    const char* debugName)
{
    if (!m_Device) return existing;
    if (requiredSize == 0) return existing;
    if (existing.IsValid() && currentCapacity >= requiredSize) return existing;

    // 1.5x geometric growth, vec4-aligned (16-byte). Dampens reallocation
    // thrash on incremental uploads while keeping memory waste bounded.
    size_t newCapacity = std::max(requiredSize, currentCapacity + currentCapacity / 2);
    newCapacity = (newCapacity + 15) & ~size_t{15};

    if (existing.IsValid()) m_Device->DestroyBuffer(existing);

    GameEngine::Rendering::BufferDesc desc{};
    desc.size = newCapacity;
    desc.usage = static_cast<uint32_t>(
        GameEngine::Rendering::BufferUsage::Storage |
        GameEngine::Rendering::BufferUsage::TransferDst);
    // Upload + PersistentlyMapped: host-visible memory the device reads
    // directly. UpdateBuffer becomes a simple memcpy into the persistent
    // mapping, no staging copy + transfer queue submission needed (which
    // would require explicit barriers before the compute dispatch reads it).
    // Mirrors GPUAnimationDataStore — same convention for the same reason.
    desc.memoryUsage = GameEngine::Rendering::BufferMemoryUsage::Upload;
    desc.flags = GameEngine::Rendering::BufferCreateFlags::PersistentlyMapped;
    desc.persistent = true;
    desc.debugName = debugName;

    auto handle = m_Device->CreateBuffer(desc);
    if (!handle.IsValid())
    {
        Logger::Log::Warning("[RetargetGPUDataStore] Failed to (re)create buffer '{}'.", debugName);
        currentCapacity = 0;
        return {};
    }
    currentCapacity = newCapacity;
    return handle;
}

void RetargetGPUDataStore::FlushIfDirty()
{
    if (!m_Device) return;

    if (m_ClipDirty)
    {
        const size_t bytes = m_ClipData.size() * sizeof(uint32_t);
        m_ClipBuf = CreateOrGrowBuffer(m_ClipBuf, m_ClipBufCapacity, bytes, "RetargetClipBuf");
        if (m_ClipBuf.IsValid() && bytes > 0)
            m_Device->UpdateBuffer(m_ClipBuf, 0, bytes, m_ClipData.data());
        m_ClipDirty = false;
    }

    if (m_RigDirty)
    {
        const size_t bytes = m_RigData.size() * sizeof(uint32_t);
        m_RigBuf = CreateOrGrowBuffer(m_RigBuf, m_RigBufCapacity, bytes, "RetargetRigBuf");
        if (m_RigBuf.IsValid() && bytes > 0)
            m_Device->UpdateBuffer(m_RigBuf, 0, bytes, m_RigData.data());
        m_RigDirty = false;
    }

    if (m_CharsDirty)
    {
        const size_t bytes = m_Chars.size() * sizeof(GPURetargetCharacterParams);
        m_CharsBuf = CreateOrGrowBuffer(m_CharsBuf, m_CharsBufCapacity, bytes, "RetargetCharsBuf");
        if (m_CharsBuf.IsValid() && bytes > 0)
            m_Device->UpdateBuffer(m_CharsBuf, 0, bytes, m_Chars.data());
        m_CharsDirty = false;
    }

    UpdateCategoryBytes();
}

void RetargetGPUDataStore::UpdateCategoryBytes()
{
    using ::GameEngine::Memory::AllocationCategory;
    using ::GameEngine::Memory::TrackAllocation;
    using ::GameEngine::Memory::TrackDeallocation;

    const uint64_t cpuBytes =
        m_ClipData.size() * sizeof(uint32_t)
      + m_RigData.size() * sizeof(uint32_t)
      + m_Chars.size() * sizeof(GPURetargetCharacterParams);
    const uint64_t devBytes =
        static_cast<uint64_t>(m_ClipBufCapacity) +
        static_cast<uint64_t>(m_RigBufCapacity) +
        static_cast<uint64_t>(m_CharsBufCapacity);
    const uint64_t newBytes = std::max(cpuBytes, devBytes);

    if (newBytes > m_ResidentBytes)
        TrackAllocation(AllocationCategory::Animation, newBytes - m_ResidentBytes);
    else if (newBytes < m_ResidentBytes)
        TrackDeallocation(AllocationCategory::Animation, m_ResidentBytes - newBytes);
    m_ResidentBytes = newBytes;
}

}}} // namespace GameEngine::Engine::Renderer
