#pragma once

// SkinPaletteAtlas: shared GPU buffer (SSBO) holding bone palettes for all
// skinned entity instances. Each entity's palette occupies a contiguous
// range of "bone slots". The vertex shader fetches bones using an
// instance-specific bone-slot offset into this atlas.
//
// Storage layout: each bone slot is 3 vec4 rows (mat3x4 — top 3 rows of
// a column-major mat4 with the implied (0,0,0,1) bottom row dropped).
// 12 floats / 48 bytes per bone, mathematically lossless for affine
// transforms. See Engine/Include/Engine/Rendering/BonePaletteLayout.h
// and Engine/Modules/Rendering/Shaders/Includes/bone_palette.glsl for
// the canonical layout definition shared between CPU and GPU.
//
// This enables instanced drawing of skinned meshes: all instances share
// the same draw call, each using a different palette offset from
// InstanceData.
//
// Responsibilities:
//   - Manage per-frame atlas buffer via PerFrameWritePool (BonePalette usage class)
//   - Accept palette uploads (mat4 array + bone count) and return a bone-slot offset
//   - Provide the atlas buffer handle for shader binding
//
// Ownership:
//   - Owned by RenderServices.
//   - Per-frame data is transient (reset each frame via PerFrameWritePool).
//
// Thread safety:
//   - Upload/Allocate from a single thread per frame (render thread).

#include "Engine/Rendering/BonePaletteLayout.h"
#include "Engine/Rendering/PerFrameWritePool.h"
#include "Rendering/Core/Handle.h"
#include "Types/Types.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
// Result of allocating space for a skeleton's palette in the atlas.
struct PaletteAllocation
{
    uint32_t offsetInFloats = 0; // Offset (in float units) into the atlas SSBO
    uint32_t boneCount = 0;      // Number of bones in this allocation
    bool valid = false;

    // Offset in bone-slot units (for shader indexing into the rows[]
    // array via the bone_palette.glsl helpers). One bone slot = 3 vec4
    // rows = 12 floats. Historically this was named OffsetInMat4s when
    // each bone occupied a full mat4 (16 floats); the unit semantics
    // (one slot per bone) are preserved across the mat3x4 layout change
    // so callers and per-instance push-constant payloads need no
    // numerical adjustment.
    uint32_t OffsetInBones() const { return offsetInFloats / BonePaletteLayout::kFloatsPerBone; }
};

class SkinPaletteAtlas
{
  public:
    SkinPaletteAtlas() = default;

    // Begin a new frame. Resets all allocations (palettes are re-uploaded each frame).
    // Uploads a block of identity mat4s at offset 0 so that any entity whose
    // skinPaletteOffset defaults to 0 (no active animation) gets bind-pose
    // rendering instead of garbage bone data.
    void BeginFrame(PerFrameWritePool& pool)
    {
        m_Pool = &pool;

        // Reserve atlas slot 0 with identity matrices. 256 covers the largest
        // bone-count bucket used by SkinningUploadSystem.
        UploadIdentityPalette(kIdentityBoneCount);

        // Reset diagnostic counters *after* the identity upload so that stats
        // reflect only the caller-visible allocations, not the internal
        // identity block.
        m_CurrentFrameAllocCount = 0;
        m_CurrentFrameTotalBones = 0;
    }

    static constexpr uint32_t kIdentityBoneCount = 256;

    // Bone-slot offset of the identity block BeginFrame reserves above.
    // A producer whose Upload/Reserve is REFUSED publishes this offset rather
    // than leaving the runtime's current-frame offset naming last frame's
    // slot: the refusing frame renders bind pose, instead of skinning the
    // entity with whichever runtime now owns that slot.
    static constexpr uint32_t kIdentityPaletteOffsetBones = 0;

    // Upload a bone palette (array of boneCount mat4 values, column-major,
    // 16 floats each) into the atlas. Internally packs each mat4 down to
    // 3 vec4 rows (BonePaletteLayout::kFloatsPerBone == 12) so the GPU
    // storage and the read helper see the same layout. Returns the
    // allocation info (offset) for shader binding.
    //
    // `paletteData` must point to boneCount * 16 floats.
    PaletteAllocation Upload(const float* paletteData, uint32_t boneCount)
    {
        PaletteAllocation result{};
        if (!m_Pool || !paletteData || boneCount == 0)
            return result;

        const size_t bytesNeeded = static_cast<size_t>(boneCount)
                                 * BonePaletteLayout::kFloatsPerBone * sizeof(float);
        auto alloc = m_Pool->Allocate(FrameWriteUsage::BonePalette, bytesNeeded, 16);
        if (!alloc.IsValid())
            return result;

        // Pack each input mat4 (16 floats) into 3 vec4 rows (12 floats).
        // The bottom row is implicitly (0,0,0,1) and is not stored.
        float* dst = static_cast<float*>(alloc.ptr);
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            BonePaletteLayout::PackMat4ToRows(
                paletteData + i * 16u,
                dst + i * BonePaletteLayout::kFloatsPerBone);
        }

        result.offsetInFloats = static_cast<uint32_t>(alloc.offset / sizeof(float));
        result.boneCount = boneCount;
        result.valid = true;

        m_CurrentFrameAllocCount++;
        m_CurrentFrameTotalBones += boneCount;

        return result;
    }

    // Reserve atlas space for a bone palette without writing data.
    // Used by the GPU compute skinning path: the compute shader writes
    // directly to the reserved region via ge_StoreBonePalette. Returns
    // the offset in bone-slot units, or UINT32_MAX on failure.
    uint32_t Reserve(uint32_t boneCount)
    {
        if (!m_Pool || boneCount == 0)
            return UINT32_MAX;

        const size_t bytesNeeded = static_cast<size_t>(boneCount)
                                 * BonePaletteLayout::kFloatsPerBone * sizeof(float);
        auto alloc = m_Pool->Allocate(FrameWriteUsage::BonePalette, bytesNeeded, 16);
        if (!alloc.IsValid())
            return UINT32_MAX;

        m_CurrentFrameAllocCount++;
        m_CurrentFrameTotalBones += boneCount;

        return static_cast<uint32_t>(alloc.offset / sizeof(float))
             / BonePaletteLayout::kFloatsPerBone;
    }

    // Get the atlas buffer handle for the current frame (for shader binding).
    Rendering::BufferHandle GetBuffer() const
    {
        return m_Pool ? m_Pool->GetBuffer(FrameWriteUsage::BonePalette) : Rendering::BufferHandle{};
    }

    // Byte size of the buffer GetBuffer returns — descriptor writes need an
    // explicit range and the pool's capacity is that range.
    uint64_t GetBufferBytes() const
    {
        return m_Pool ? m_Pool->GetCapacity(FrameWriteUsage::BonePalette) : 0;
    }

    // LAST frame's atlas, read by the TAA skinned motion-vector pass to
    // evaluate the previous skinned pose. Pair every use with
    // HasPreviousFrame(): on the first frame after startup or a device
    // rebuild there is no previous pose, and callers must fall back to the
    // current palette on both endpoints (zero pose motion) rather than
    // sampling undefined bytes.
    Rendering::BufferHandle GetPreviousBuffer() const
    {
        return m_Pool ? m_Pool->GetPreviousBuffer(FrameWriteUsage::BonePalette)
                      : Rendering::BufferHandle{};
    }

    bool HasPreviousFrame() const
    {
        return m_Pool && m_Pool->HasPreviousFrameBuffer(FrameWriteUsage::BonePalette);
    }

    size_t GetPreviousCapacityBytes() const
    {
        return m_Pool ? m_Pool->GetPreviousCapacity(FrameWriteUsage::BonePalette) : 0;
    }

    // Full extent of the current frame's atlas ring, for descriptor binds that
    // index it by bone offset rather than sub-allocating a range.
    size_t GetCapacityBytes() const
    {
        return m_Pool ? m_Pool->GetCapacity(FrameWriteUsage::BonePalette) : 0;
    }

    // Diagnostics.
    uint32_t GetCurrentFrameAllocCount() const { return m_CurrentFrameAllocCount; }
    uint32_t GetCurrentFrameTotalBones() const { return m_CurrentFrameTotalBones; }

  private:
    void UploadIdentityPalette(uint32_t boneCount)
    {
        if (!m_Pool || boneCount == 0)
            return;
        // Build column-major identity mat4 source data so we hit the same
        // PackMat4ToRows path as live uploads — keeps the identity
        // exactly equivalent to "play a clip whose pose is identity".
        const size_t floatCount = static_cast<size_t>(boneCount) * 16;
        m_IdentityBuf.assign(floatCount, 0.0f);
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            const size_t base = static_cast<size_t>(i) * 16;
            m_IdentityBuf[base + 0]  = 1.0f;
            m_IdentityBuf[base + 5]  = 1.0f;
            m_IdentityBuf[base + 10] = 1.0f;
            m_IdentityBuf[base + 15] = 1.0f;
        }
        Upload(m_IdentityBuf.data(), boneCount);
    }

    PerFrameWritePool* m_Pool = nullptr;
    uint32_t m_CurrentFrameAllocCount = 0;
    uint32_t m_CurrentFrameTotalBones = 0;
    std::vector<float> m_IdentityBuf; // reused across frames to avoid allocation
};

} // namespace Engine::Renderer
} // namespace GameEngine
