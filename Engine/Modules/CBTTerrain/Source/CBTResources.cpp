#include "CBTTerrain/CBTResources.h"

#include <algorithm>

#include "CBTTerrain/CBTKernelSet.h"

#include "Rendering/Core/Device.h"
#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine::CBTTerrain
{

using namespace GameEngine::Rendering;

namespace
{
// Per-binding element byte size (std430). Multiplied by the count below to size
// each buffer. Non-per-bisector buffers use a fixed word count.
struct BufferSpec
{
    uint64_t bytes;
    uint32_t extraUsage; // beyond Storage | TransferSrc | TransferDst
    const char* name;
};

BufferSpec SpecFor(CBTBinding binding, uint32_t p)
{
    const uint64_t P = p;
    switch (binding)
    {
    case CBTBinding::HeapID:          return {P * sizeof(uint64_t), 0, "CBT.HeapID"};
    case CBTBinding::NeighborsA:      return {P * sizeof(CBTNeighbors), 0, "CBT.NeighborsA"};
    case CBTBinding::NeighborsB:      return {P * sizeof(CBTNeighbors), 0, "CBT.NeighborsB"};
    case CBTBinding::BisectorData:    return {P * sizeof(CBTBisectorData), 0, "CBT.BisectorData"};
    case CBTBinding::Bitfield:        return {static_cast<uint64_t>(CBTBitfieldWords(p)) * 4u, 0, "CBT.Bitfield"};
    case CBTBinding::SumTree:         return {static_cast<uint64_t>(CBTSumTreeWords(p)) * 4u, 0, "CBT.SumTree"};
    case CBTBinding::WorkQueue:       return {static_cast<uint64_t>(WQElementCount(p)) * 4u, 0, "CBT.WorkQueue"};
    case CBTBinding::IndirectDispatch: return {kIndirectDispatchWords * 4u,
                                               static_cast<uint32_t>(BufferUsage::Indirect), "CBT.IndirectDispatch"};
    case CBTBinding::IndirectDraw:    return {kIndirectDrawWords * 4u,
                                               static_cast<uint32_t>(BufferUsage::Indirect), "CBT.IndirectDraw"};
    case CBTBinding::IndicesAll:      return {P * sizeof(uint32_t), 0, "CBT.IndicesAll"};
    case CBTBinding::IndicesVisible:  return {P * sizeof(uint32_t), 0, "CBT.IndicesVisible"};
    case CBTBinding::IndicesModified: return {P * sizeof(uint32_t), 0, "CBT.IndicesModified"};
    case CBTBinding::CurrentVertex:   return {P * sizeof(CBTVertexData), 0, "CBT.CurrentVertex"};
    case CBTBinding::Validation:      return {kValidationWords * 4u, 0, "CBT.Validation"};
    default:                          return {0, 0, "CBT.Unknown"};
    }
}
} // namespace

CBTResources::~CBTResources()
{
    Shutdown();
}

BufferHandle CBTResources::CreateStorageBuffer(uint64_t size, uint32_t extraUsage, const char* name)
{
    BufferDesc desc{};
    desc.size = static_cast<size_t>(size);
    // Every persistent buffer supports GPU zero-fill (root init), CPU upload
    // (root data), and readback copies (debug counters), plus its extra usage.
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc |
                                       BufferUsage::TransferDst) |
                 extraUsage;
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.persistent = true;
    desc.debugName = name;
    return m_Device->CreateBuffer(desc);
}

bool CBTResources::Initialize(IDevice& device, const CBTKernelSet& kernelSet, uint32_t poolSize)
{
    Shutdown();
    m_Device = &device;
    m_PoolSize = poolSize;

    if (poolSize != kDefaultBisectorPoolSize)
    {
        Logger::Log::Error("CBTResources: C1 supports only the default {} pool (got {})",
                           kDefaultBisectorPoolSize, poolSize);
        return false;
    }

    m_PersistentBytes = 0;
    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
    {
        const CBTBinding binding = static_cast<CBTBinding>(i);
        const BufferSpec spec = SpecFor(binding, poolSize);
        BufferHandle h = CreateStorageBuffer(spec.bytes, spec.extraUsage, spec.name);
        if (!h.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create buffer {}", spec.name);
            Shutdown();
            return false;
        }
        m_Buffers[i] = h;
        m_PersistentBytes += spec.bytes;
    }

    // C3 draw resources. Identity index buffer (index[i] = i, 3*P u32 = 12 MiB at the
    // 1M pool — it scales with the pool, not with the live set) and the one-element
    // draw-count buffer (holds 1). TransferSrc keeps them readback-testable;
    // Index/Indirect are the graphics consumption usages.
    m_IdentityIndexBuffer = CreateStorageBuffer(
        static_cast<uint64_t>(IdentityIndexCount(poolSize)) * sizeof(uint32_t),
        static_cast<uint32_t>(BufferUsage::Index), "CBT.IdentityIndex");
    m_DrawCountBuffer = CreateStorageBuffer(
        sizeof(uint32_t), static_cast<uint32_t>(BufferUsage::Indirect), "CBT.DrawCount");
    if (!m_IdentityIndexBuffer.IsValid() || !m_DrawCountBuffer.IsValid())
    {
        Logger::Log::Error("CBTResources: failed to create draw resources");
        Shutdown();
        return false;
    }
    m_PersistentBytes += static_cast<uint64_t>(IdentityIndexCount(poolSize)) * sizeof(uint32_t);

    // C4 per-frame params UBO (host-visible ring, written once per frame) and the
    // terrain height source. The default height is a 1x1 zero R32_FLOAT so the
    // combined-image-sampler descriptor (binding 15) is always valid even before a
    // terrain binds a real heightmap; VertexEval multiplies by HeightScale (0 until
    // a terrain is bound), so its content is irrelevant. The linear-clamp sampler
    // matches the CDLOD path's GE_TS_CLAMP. Content upload + the ShaderResource
    // transition happen in CBTInstance::InitializeRoots (which owns command lists).
    {
        BufferDesc ubo{};
        ubo.size = static_cast<size_t>(kCBTFrameParamsRing) * sizeof(CBTFrameParams);
        ubo.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        ubo.memoryUsage = BufferMemoryUsage::Upload;
        ubo.persistent = true;
        ubo.debugName = "CBT.FrameParams";
        m_FrameParams = device.CreateBuffer(ubo);

        TextureDesc td{};
        td.width = 1;
        td.height = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferDst);
        td.persistent = true;
        td.debugName = "CBT.DefaultHeight";
        m_DefaultHeight = device.CreateTexture(td);
        for (auto& slot : m_BoundHeight)
            slot = m_DefaultHeight;

        m_HeightSampler = device.CreateSampler(SamplerDesc::MaterialLinearClamp("CBT.HeightSampler"));

        if (!m_FrameParams.IsValid() || !m_DefaultHeight.IsValid() || !m_HeightSampler.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create C4 params/height resources");
            Shutdown();
            return false;
        }
        m_PersistentBytes += ubo.size;

        // Sphere sculpt page POOL (binding 16) + page TABLE (binding 20). A planar terrain never reads
        // them, so the rings start as placeholders that only keep the two bindings valid;
        // InitializeRoots provisions the full rings when the tree is seeded for the sphere.
        SculptRings placeholders{};
        if (!CreateSculptRings(false, 0u, placeholders))
        {
            Shutdown();
            return false;
        }
        AdoptSculptRings(placeholders);

        // Planet-shading surface params (plan §planet-shading) — a tiny host-visible UNIFORM ring the
        // fragment reads on the graphics side (radius, relief amplitude/frequency/octaves,
        // sculpt-enabled). Per-slot stride padded to kCBTSurfaceParamsSlotStride so a per-slot
        // offset-bind meets minUniformBufferOffsetAlignment. Not in the compute descriptor set.
        BufferDesc surf{};
        surf.size = static_cast<size_t>(kCBTFrameParamsRing) * kCBTSurfaceParamsSlotStride;
        // Uniform, not Storage: the fragment reads it as a uniform block so it does not spend one
        // of WebGPU's 10 guaranteed per-stage storage slots (cbt_surface.glsl says why).
        surf.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        surf.memoryUsage = BufferMemoryUsage::Upload;
        surf.persistent = true;
        surf.debugName = "CBT.SurfaceParams";
        m_SurfaceParams = device.CreateBuffer(surf);
        if (!m_SurfaceParams.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create surface-params UBO");
            Shutdown();
            return false;
        }
        if (void* mapped = m_Device->MapBuffer(m_SurfaceParams))
        {
            std::memset(mapped, 0, surf.size);
            m_Device->UnmapBuffer(m_SurfaceParams);
        }
        m_PersistentBytes += surf.size;

        // Phase E resident-window atlas (bindings 17/18). The indirection SSBO is a
        // host-visible ring (kCBTFrameParamsRing slots of kAtlasMaxTiles rows), written
        // once per frame from the residency table — same discipline as the sculpt atlas.
        // Zeroed on create so an atlas-disabled default reads every row as kAtlasNoSlot.
        BufferDesc atlasRows{};
        atlasRows.size = static_cast<size_t>(kCBTFrameParamsRing) *
                         static_cast<size_t>(kAtlasMaxTiles) * kAtlasRowBytes;
        atlasRows.usage = static_cast<uint32_t>(BufferUsage::Storage);
        atlasRows.memoryUsage = BufferMemoryUsage::Upload;
        atlasRows.persistent = true;
        atlasRows.debugName = "CBT.AtlasRows";
        m_AtlasRows = device.CreateBuffer(atlasRows);
        if (!m_AtlasRows.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create atlas indirection SSBO");
            Shutdown();
            return false;
        }
        if (void* mapped = m_Device->MapBuffer(m_AtlasRows))
        {
            std::memset(mapped, 0xFF, atlasRows.size); // 0xFFFFFFFF Slot == kAtlasNoSlot
            m_Device->UnmapBuffer(m_AtlasRows);
        }
        m_PersistentBytes += atlasRows.size;

        // The default atlas height texture — a 1x1 zero R32_FLOAT so binding 18's ring is
        // always valid before any atlas-backed terrain binds. Shares the height sampler
        // (linear-clamp). heightScale gates its content to irrelevance when unbound.
        TextureDesc atd{};
        atd.width = 1;
        atd.height = 1;
        atd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        atd.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
                    static_cast<uint32_t>(TextureUsage::TransferDst);
        atd.persistent = true;
        atd.debugName = "CBT.DefaultAtlasHeight";
        m_DefaultAtlasHeight = device.CreateTexture(atd);
        atd.debugName = "CBT.DefaultAtlasCoarse";
        m_DefaultAtlasCoarse = device.CreateTexture(atd);
        if (!m_DefaultAtlasHeight.IsValid() || !m_DefaultAtlasCoarse.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create default atlas textures");
            Shutdown();
            return false;
        }
        for (auto& slot : m_BoundAtlasHeight)
            slot = m_DefaultAtlasHeight;
        for (auto& slot : m_BoundAtlasCoarse)
            slot = m_DefaultAtlasCoarse;
    }

    // Readback: indirect-draw records + validation counters, whichever is larger,
    // rounded to hold both.
    m_ReadbackSize = static_cast<uint64_t>(kIndirectDrawWords + kValidationWords) * 4u;
    m_Readback = device.CreateReadbackBuffer(static_cast<size_t>(m_ReadbackSize), "CBT.Readback");
    if (!m_Readback.IsValid())
    {
        Logger::Log::Error("CBTResources: failed to create readback buffer");
        Shutdown();
        return false;
    }

    // Binding 21: the height-range pyramid (content-aware split), wide arm only. Device-local;
    // Kernel_HeightRangeBuild writes it from the bound height texture before Classify reads it.
    if (!kernelSet.IsNarrowHeap())
    {
        const uint64_t bytes = static_cast<uint64_t>(kCBTHeightRangeBufferWords) * sizeof(uint32_t);
        m_HeightRange = CreateStorageBuffer(bytes, 0u, "CBT.HeightRange");
        if (!m_HeightRange.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create the height-range pyramid buffer");
            Shutdown();
            return false;
        }
        m_PersistentBytes += bytes;

        // Bindings 22/23: the paged height resolve (wide arm only). The page-table ring is
        // host-visible and written whole per frame slot when the terrain's table changes; zeroed
        // so a slot never written reads an empty field (level count 0 resolves to 0).
        m_PageTable = CreatePageTableRing(kCBTPageTableMinRingWords);
        m_PageTableSlotWords = kCBTPageTableMinRingWords;
        TextureDesc pcd{};
        pcd.width = 1;
        pcd.height = 1;
        pcd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        pcd.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
        pcd.persistent = true;
        pcd.debugName = "CBT.DefaultPageCache";
        m_DefaultPageCache = device.CreateTexture(pcd);
        if (!m_PageTable.IsValid() || !m_DefaultPageCache.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create the page table ring or the default page cache");
            Shutdown();
            return false;
        }
        m_PersistentBytes += PageTableRingBytes(m_PageTableSlotWords);
        for (auto& slot : m_BoundPageCache)
            slot = m_DefaultPageCache;
    }

    m_TextureRing = CBTTextureRingFor(kernelSet.IsNarrowHeap());
    if (kernelSet.UsesPerKernelLayouts())
    {
        // One set per kernel, each matching that kernel's filtered layout: the shared
        // layout exceeds the device's per-stage storage-buffer budget, and a set must
        // match its pipeline's layout exactly.
        m_KernelSets.resize(kCBTKernelCount);
        m_KernelSetBindings.resize(kCBTKernelCount);
    }
    for (uint32_t k = 0; k < static_cast<uint32_t>(m_KernelSets.size()); ++k)
    {
        const Rendering::DescriptorSetLayoutDesc* layout = kernelSet.KernelLayout(k);
        if (layout == nullptr)
        {
            Logger::Log::Error("CBTResources: kernel {} has no layout", k);
            Shutdown();
            return false;
        }
        DescriptorSetDesc dsDesc{};
        dsDesc.layout = *layout;
        dsDesc.debugName = "CBT.DescriptorSet";
        m_KernelSets[k] = device.CreateDescriptorSet(dsDesc);
        if (!m_KernelSets[k].IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create descriptor set for kernel {}", k);
            Shutdown();
            return false;
        }
        m_KernelSetBindings[k].reserve(layout->bindings.size());
        for (const auto& b : layout->bindings)
            m_KernelSetBindings[k].push_back(b.binding);
    }
    if (m_KernelSets.empty())
    {
        DescriptorSetDesc dsDesc{};
        dsDesc.layout = CBTKernelSet::MakeDescriptorSetLayout(kernelSet.IsNarrowHeap());
        dsDesc.debugName = "CBT.DescriptorSet";
        m_DescriptorSet = device.CreateDescriptorSet(dsDesc);
        if (!m_DescriptorSet.IsValid())
        {
            Logger::Log::Error("CBTResources: failed to create descriptor set");
            Shutdown();
            return false;
        }
    }
    WriteDescriptorSet();

    m_Ready = true;
    Logger::Log::Info("CBTResources: {} buffers, {} KiB persistent", kCBTBindingCount,
                      m_PersistentBytes / 1024u);
    return true;
}

bool CBTResources::CreateSculptRings(bool spherical, uint32_t pagePoolCount, SculptRings& out)
{
    // Spherical: `pagePoolCount` physical pages (radius-INDEPENDENT) and the fixed 6*cap^2 page table,
    // per ring slot. Placeholder: kSculptPlaceholderSlotWords per slot, a whole
    // minStorageBufferOffsetAlignment step so the per-slot offset bind stays valid.
    out = {};
    out.Spherical = spherical;
    out.PagePoolCount = spherical ? pagePoolCount : 0u;
    out.PoolSlotWords = spherical ? pagePoolCount * kSculptPageTexels : kSculptPlaceholderSlotWords;
    out.TableSlotWords = spherical ? kSculptPageTableEntries : kSculptPlaceholderSlotWords;

    BufferDesc sculpt{};
    sculpt.size = static_cast<size_t>(kCBTFrameParamsRing) * out.PoolSlotWords * sizeof(float);
    sculpt.usage = static_cast<uint32_t>(BufferUsage::Storage);
    sculpt.memoryUsage = BufferMemoryUsage::Upload;
    sculpt.persistent = true;
    sculpt.debugName = spherical ? "CBT.SphereSculptPool" : "CBT.SphereSculptPoolPlaceholder";
    out.Pool = m_Device->CreateBuffer(sculpt);

    BufferDesc sculptTable{};
    sculptTable.size = static_cast<size_t>(kCBTFrameParamsRing) * out.TableSlotWords * sizeof(uint32_t);
    sculptTable.usage = static_cast<uint32_t>(BufferUsage::Storage);
    sculptTable.memoryUsage = BufferMemoryUsage::Upload;
    sculptTable.persistent = true;
    sculptTable.debugName = spherical ? "CBT.SphereSculptPageTable" : "CBT.SphereSculptPageTablePlaceholder";
    out.Table = m_Device->CreateBuffer(sculptTable);
    if (!out.Pool.IsValid() || !out.Table.IsValid())
    {
        Logger::Log::Error("CBTResources: failed to create the sphere sculpt page pool / table SSBO ({}, {} KiB); "
                           "the previous rings stay bound",
                           spherical ? "spherical" : "placeholder", (sculpt.size + sculptTable.size) / 1024u);
        if (out.Pool.IsValid())
            m_Device->DestroyBuffer(out.Pool);
        if (out.Table.IsValid())
            m_Device->DestroyBuffer(out.Table);
        out = {};
        return false;
    }
    // Zeroed so the default (no edits) reads additive 0; the table defaults to kSculptNoPage
    // (all-ones), so 0xFF fills every entry.
    if (void* mapped = m_Device->MapBuffer(out.Pool))
    {
        std::memset(mapped, 0, sculpt.size);
        m_Device->UnmapBuffer(out.Pool);
    }
    if (void* mapped = m_Device->MapBuffer(out.Table))
    {
        std::memset(mapped, 0xFF, sculptTable.size);
        m_Device->UnmapBuffer(out.Table);
    }
    out.Bytes = sculpt.size + sculptTable.size;
    return true;
}

void CBTResources::AdoptSculptRings(const SculptRings& rings)
{
    m_SphereSculpt = rings.Pool;
    m_SphereSculptTable = rings.Table;
    m_SculptPagePoolCount = rings.PagePoolCount;
    m_SculptPoolSlotWords = rings.PoolSlotWords;
    m_SculptTableSlotWords = rings.TableSlotWords;
    m_SculptRingBytes = rings.Bytes;
    m_SculptRingsSpherical = rings.Spherical;
    m_PersistentBytes += rings.Bytes;
}

void CBTResources::DestroySculptRings()
{
    if (m_SphereSculpt.IsValid())
        m_Device->DestroyBuffer(m_SphereSculpt);
    m_SphereSculpt = {};
    if (m_SphereSculptTable.IsValid())
        m_Device->DestroyBuffer(m_SphereSculptTable);
    m_SphereSculptTable = {};
    m_PersistentBytes -= m_SculptRingBytes;
    m_SculptRingBytes = 0;
    m_SculptPagePoolCount = 0u;
    m_SculptPoolSlotWords = 0u;
    m_SculptTableSlotWords = 0u;
    m_SculptRingsSpherical = false;
}

bool CBTResources::ProvisionSculptRings(bool spherical, uint32_t pagePoolCount)
{
    if (!m_Ready)
        return false;
    if (spherical == m_SculptRingsSpherical)
        return true;
    // Create first: a refused allocation leaves the previous rings, and the sets that point at them,
    // untouched, so the GPU never reads a destroyed buffer.
    SculptRings next{};
    if (!CreateSculptRings(spherical, pagePoolCount, next))
        return false;
    DestroySculptRings();
    AdoptSculptRings(next);
    WriteDescriptorSet();
    Logger::Log::Info("CBTResources: sphere sculpt rings {} ({} KiB host-visible)",
                      spherical ? "provisioned" : "released to the placeholder", m_SculptRingBytes / 1024u);
    return true;
}

Rendering::DescriptorSetHandle CBTResources::GetDescriptorSet(uint32_t kernelIndex) const
{
    if (m_KernelSets.empty())
        return m_DescriptorSet;
    return kernelIndex < m_KernelSets.size() ? m_KernelSets[kernelIndex]
                                             : Rendering::DescriptorSetHandle{};
}

void CBTResources::WriteDescriptorSet()
{
    if (m_KernelSets.empty())
    {
        WriteOneSet(m_DescriptorSet, {});
        return;
    }
    for (size_t k = 0; k < m_KernelSets.size(); ++k)
        WriteOneSet(m_KernelSets[k], m_KernelSetBindings[k]);
}

// `only` empty means "this set carries every binding"; otherwise a binding absent from it belongs
// to a different kernel's layout and writing it would not match the set.
void CBTResources::WriteOneSet(Rendering::DescriptorSetHandle set,
                               const std::vector<uint32_t>& only)
{
    const auto has = [&](uint32_t b) {
        return only.empty() || std::find(only.begin(), only.end(), b) != only.end();
    };
    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
    {
        // size = 0 binds the whole buffer from offset 0.
        if (has(i))
            m_Device->UpdateStorageBufferBinding(set, i, m_Buffers[i], 0, 0);
    }
    // Binding 14: the whole params UBO ring (the shader indexes it by frame slot).
    if (has(kCBTFrameParamsBinding))
        m_Device->UpdateBufferBinding(set, kCBTFrameParamsBinding, m_FrameParams, 0,
                                  static_cast<size_t>(kCBTFrameParamsRing) * sizeof(CBTFrameParams));
    // Binding 15: every element starts on the flat default until SetHeightSource rebinds
    // a frame slot to a real heightmap. One element where the ring is collapsed.
    for (uint32_t slot = 0; slot < m_TextureRing && has(kCBTHeightTextureBinding); ++slot)
        m_Device->UpdateCombinedImageSamplerBinding(set, kCBTHeightTextureBinding,
                                                    m_BoundHeight[slot], m_HeightSampler, slot);
    // Binding 16: the whole sphere sculpt ring SSBO (the shader indexes it by frame slot).
    if (has(kCBTSphereSculptBinding))
        m_Device->UpdateStorageBufferBinding(set, kCBTSphereSculptBinding, m_SphereSculpt, 0, 0);
    // Binding 17: the whole atlas indirection ring SSBO (the shader indexes it by frame slot).
    if (has(kCBTAtlasRowsBinding))
        m_Device->UpdateStorageBufferBinding(set, kCBTAtlasRowsBinding, m_AtlasRows, 0, 0);
    // Binding 18: every element starts on the flat default until SetAtlasSource rebinds
    // a frame slot to a real atlas texture.
    for (uint32_t slot = 0; slot < m_TextureRing && has(kCBTAtlasHeightBinding); ++slot)
        m_Device->UpdateCombinedImageSamplerBinding(set, kCBTAtlasHeightBinding,
                                                    m_BoundAtlasHeight[slot], m_HeightSampler, slot);
    // Binding 19: the coarse fallback field, default until SetCoarseSource rebinds it.
    for (uint32_t slot = 0; slot < m_TextureRing && has(kCBTAtlasCoarseBinding); ++slot)
        m_Device->UpdateCombinedImageSamplerBinding(set, kCBTAtlasCoarseBinding,
                                                    m_BoundAtlasCoarse[slot], m_HeightSampler, slot);
    // Binding 20: the whole sphere sculpt page-table ring SSBO (the shader indexes it by frame slot).
    if (has(kCBTSphereSculptPageTableBinding))
        m_Device->UpdateStorageBufferBinding(set, kCBTSphereSculptPageTableBinding,
                                             m_SphereSculptTable, 0, 0);
    // Binding 21: the height-range pyramid (wide arm only; the narrow layouts do not carry it).
    if (has(kCBTHeightRangeBinding) && m_HeightRange.IsValid())
        m_Device->UpdateStorageBufferBinding(set, kCBTHeightRangeBinding, m_HeightRange, 0, 0);
    // Bindings 22/23: the paged height resolve (wide arm only, like 21).
    if (has(kCBTPageTableBinding) && m_PageTable.IsValid())
        m_Device->UpdateStorageBufferBinding(set, kCBTPageTableBinding, m_PageTable, 0, 0);
    for (uint32_t slot = 0; slot < m_TextureRing && has(kCBTPageCacheBinding) && m_DefaultPageCache.IsValid(); ++slot)
        m_Device->UpdateCombinedImageSamplerBinding(set, kCBTPageCacheBinding, m_BoundPageCache[slot],
                                                    m_HeightSampler, slot);
}

void CBTResources::UploadFrameParams(uint32_t frameCounter, const CBTFrameParams& params)
{
    if (!m_FrameParams.IsValid())
        return;
    const uint32_t slot = CBTFrameRingSlot(frameCounter);
    void* mapped = m_Device->MapBuffer(m_FrameParams);
    if (!mapped)
        return;
    auto* dst = reinterpret_cast<CBTFrameParams*>(static_cast<uint8_t*>(mapped) +
                                                  static_cast<size_t>(slot) * sizeof(CBTFrameParams));
    std::memcpy(dst, &params, sizeof(CBTFrameParams));
    // The page table's slot stride travels with the frame that reads it (cbt_layout.glsl
    // CBT_PageRingBase): ProvisionPageTableWords runs before this upload in the frame.
    dst->AnalyticParams[kCBTPageRingWordsLane] = static_cast<float>(m_PageTableSlotWords);
    m_Device->UnmapBuffer(m_FrameParams);
}

void CBTResources::UploadSphereSculptPool(uint32_t frameCounter, const float* pool, uint32_t texelCount)
{
    if (!m_SphereSculpt.IsValid() || pool == nullptr)
        return;
    if (!m_SculptRingsSpherical)
        return;
    const uint32_t poolTexels = m_SculptPoolSlotWords;
    const uint32_t count = texelCount < poolTexels ? texelCount : poolTexels;
    const uint32_t slot = CBTFrameRingSlot(frameCounter);
    void* mapped = m_Device->MapBuffer(m_SphereSculpt);
    if (!mapped)
        return;
    const size_t slotByteBase =
        static_cast<size_t>(slot) * static_cast<size_t>(poolTexels) * sizeof(float);
    std::memcpy(static_cast<uint8_t*>(mapped) + slotByteBase, pool,
                static_cast<size_t>(count) * sizeof(float));
    m_Device->UnmapBuffer(m_SphereSculpt);
}

void CBTResources::UploadSphereSculptPageTable(uint32_t frameCounter, const uint32_t* table,
                                               uint32_t entryCount)
{
    if (!m_SphereSculptTable.IsValid() || table == nullptr || !m_SculptRingsSpherical)
        return;
    const uint32_t count = entryCount < m_SculptTableSlotWords ? entryCount : m_SculptTableSlotWords;
    const uint32_t slot = CBTFrameRingSlot(frameCounter);
    void* mapped = m_Device->MapBuffer(m_SphereSculptTable);
    if (!mapped)
        return;
    const size_t slotByteBase =
        static_cast<size_t>(slot) * static_cast<size_t>(m_SculptTableSlotWords) * sizeof(uint32_t);
    std::memcpy(static_cast<uint8_t*>(mapped) + slotByteBase, table,
                static_cast<size_t>(count) * sizeof(uint32_t));
    m_Device->UnmapBuffer(m_SphereSculptTable);
}

void CBTResources::UploadSurfaceParams(uint32_t frameCounter, const CBTSurfaceParams& params)
{
    if (!m_SurfaceParams.IsValid())
        return;
    const uint32_t slot = CBTFrameRingSlot(frameCounter);
    void* mapped = m_Device->MapBuffer(m_SurfaceParams);
    if (!mapped)
        return;
    std::memcpy(static_cast<uint8_t*>(mapped) +
                    static_cast<size_t>(slot) * kCBTSurfaceParamsSlotStride,
                &params, sizeof(CBTSurfaceParams));
    m_Device->UnmapBuffer(m_SurfaceParams);
}

// Rewrite one ring element of a terrain-source binding in every set that binds it.
// On the web build each kernel binds its OWN set (bind groups must match the
// kernel's exact layout), so a runtime rebind must fan out — writing only
// m_DescriptorSet updates a set no web kernel binds, and VertexEval keeps
// sampling whatever the set held at creation (the flat default: terrain uploads
// land in the texture yet the terrain renders flat, with no validation error).
// The backend drops writes to bindings a set's layout does not declare, so the
// fan-out is safe for kernels without the binding.
void CBTResources::UpdateTerrainSourceBinding(uint32_t binding, Rendering::TextureHandle texture,
                                              uint32_t slot)
{
    if (m_KernelSets.empty())
    {
        m_Device->UpdateCombinedImageSamplerBinding(m_DescriptorSet, binding, texture,
                                                    m_HeightSampler, slot);
        return;
    }
    for (const auto& set : m_KernelSets)
    {
        if (set.IsValid())
            m_Device->UpdateCombinedImageSamplerBinding(set, binding, texture, m_HeightSampler, slot);
    }
}

void CBTResources::SetHeightSource(uint32_t frameCounter, Rendering::TextureHandle texture)
{
    if (!m_Ready)
        return;
    const uint32_t slot = CBTTextureRingSlot(frameCounter, m_TextureRing);
    const Rendering::TextureHandle target = texture.IsValid() ? texture : m_DefaultHeight;
    if (target == m_BoundHeight[slot])
        return; // unchanged — nothing to rewrite in this frame's element
    m_BoundHeight[slot] = target;
    UpdateTerrainSourceBinding(kCBTHeightTextureBinding, m_BoundHeight[slot], slot);
    ++m_TerrainSourceGeneration;
}

Rendering::TextureHandle CBTResources::GetBoundHeightSource(uint32_t frameCounter) const
{
    if (!m_Ready)
        return {};
    const Rendering::TextureHandle bound = m_BoundHeight[CBTTextureRingSlot(frameCounter, m_TextureRing)];
    return bound.IsValid() ? bound : m_DefaultHeight;
}

void CBTResources::SetAtlasSource(uint32_t frameCounter, Rendering::TextureHandle atlasTexture)
{
    if (!m_Ready)
        return;
    const uint32_t slot = CBTTextureRingSlot(frameCounter, m_TextureRing);
    const Rendering::TextureHandle target = atlasTexture.IsValid() ? atlasTexture : m_DefaultAtlasHeight;
    if (target == m_BoundAtlasHeight[slot])
        return; // unchanged — nothing to rewrite in this frame's element
    m_BoundAtlasHeight[slot] = target;
    UpdateTerrainSourceBinding(kCBTAtlasHeightBinding, m_BoundAtlasHeight[slot], slot);
    ++m_TerrainSourceGeneration;
}

void CBTResources::SetCoarseSource(uint32_t frameCounter, Rendering::TextureHandle coarseTexture)
{
    if (!m_Ready)
        return;
    const uint32_t slot = CBTTextureRingSlot(frameCounter, m_TextureRing);
    const Rendering::TextureHandle target = coarseTexture.IsValid() ? coarseTexture : m_DefaultAtlasCoarse;
    if (target == m_BoundAtlasCoarse[slot])
        return;
    m_BoundAtlasCoarse[slot] = target;
    UpdateTerrainSourceBinding(kCBTAtlasCoarseBinding, m_BoundAtlasCoarse[slot], slot);
    ++m_TerrainSourceGeneration;
}

void CBTResources::SetPageCacheSource(uint32_t frameCounter, Rendering::TextureHandle cacheTexture)
{
    if (!m_Ready || !m_DefaultPageCache.IsValid())
        return;
    const uint32_t slot = CBTTextureRingSlot(frameCounter, m_TextureRing);
    const Rendering::TextureHandle target = cacheTexture.IsValid() ? cacheTexture : m_DefaultPageCache;
    if (target == m_BoundPageCache[slot])
        return;
    m_BoundPageCache[slot] = target;
    UpdateTerrainSourceBinding(kCBTPageCacheBinding, m_BoundPageCache[slot], slot);
    ++m_TerrainSourceGeneration;
}

size_t CBTResources::PageTableRingBytes(uint32_t slotWords)
{
    return static_cast<size_t>(kCBTFrameParamsRing) * slotWords * sizeof(uint32_t);
}

Rendering::BufferHandle CBTResources::CreatePageTableRing(uint32_t slotWords)
{
    using namespace Rendering;
    BufferDesc desc{};
    desc.size = PageTableRingBytes(slotWords);
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.persistent = true;
    desc.debugName = "CBT.PageTable";
    const BufferHandle ring = m_Device->CreateBuffer(desc);
    if (!ring.IsValid())
        return ring;
    // Zeroed so a slot never written reads an empty field (level count 0 resolves to 0).
    if (void* mapped = m_Device->MapBuffer(ring))
    {
        std::memset(mapped, 0, desc.size);
        m_Device->UnmapBuffer(ring);
    }
    return ring;
}

bool CBTResources::ProvisionPageTableWords(uint32_t words)
{
    if (!m_PageTable.IsValid())
        return false;
    if (words <= m_PageTableSlotWords)
        return true;
    // Grow in whole steps so a terrain whose table grows a little does not reallocate each time.
    const uint32_t slotWords =
        (words + kCBTPageTableMinRingWords - 1u) / kCBTPageTableMinRingWords * kCBTPageTableMinRingWords;
    // Create first: a refused allocation leaves the previous ring, and the sets that point at it.
    const Rendering::BufferHandle next = CreatePageTableRing(slotWords);
    if (!next.IsValid())
    {
        Logger::Log::Error("CBTResources: failed to grow the page table ring to {} KiB; the terrain draws from its "
                           "height texture",
                           PageTableRingBytes(slotWords) / 1024u);
        return false;
    }
    // The descriptor sets are shared by every frame slot and rewritten in place below. Wait for
    // their previous graphics users before replacing the ring and its stride, without changing
    // the device-wide idle state during render-graph declaration.
    m_Device->WaitGpuSyncToken(m_Device->LastGraphicsSubmissionToken());
    m_Device->DestroyBuffer(m_PageTable);
    m_PersistentBytes -= PageTableRingBytes(m_PageTableSlotWords);
    m_PageTable = next;
    m_PageTableSlotWords = slotWords;
    m_PersistentBytes += PageTableRingBytes(slotWords);
    WriteDescriptorSet();
    Logger::Log::Info("CBTResources: page table ring grown to {} words a frame slot ({} KiB host-visible)", slotWords,
                      PageTableRingBytes(slotWords) / 1024u);
    return true;
}

void CBTResources::UploadPageTable(uint32_t frameCounter, std::span<const uint32_t> words)
{
    if (!m_PageTable.IsValid())
        return;
    const uint32_t slot = CBTFrameRingSlot(frameCounter);
    const size_t count = std::min<size_t>(words.size(), m_PageTableSlotWords);
    void* mapped = m_Device->MapBuffer(m_PageTable);
    if (!mapped)
        return;
    uint32_t* dst = static_cast<uint32_t*>(mapped) + static_cast<size_t>(slot) * m_PageTableSlotWords;
    if (count > 0u)
        std::memcpy(dst, words.data(), count * sizeof(uint32_t));
    else
        std::memset(dst, 0, kCBTPageHeaderWords * sizeof(uint32_t)); // level count 0: no field
    m_Device->UnmapBuffer(m_PageTable);
}

bool CBTResources::RebindTerrainSourcesToDefault()
{
    if (!m_Ready)
        return false;
    bool changed = false;
    auto rebindRing = [&](std::array<Rendering::TextureHandle, kCBTTextureRingMax>& ring,
                          Rendering::TextureHandle def, uint32_t binding)
    {
        for (uint32_t slot = 0; slot < m_TextureRing; ++slot)
        {
            if (ring[slot] == def)
                continue; // already the default — leave the descriptor untouched
            ring[slot] = def;
            UpdateTerrainSourceBinding(binding, def, slot);
            changed = true;
        }
    };
    rebindRing(m_BoundHeight, m_DefaultHeight, kCBTHeightTextureBinding);
    rebindRing(m_BoundAtlasHeight, m_DefaultAtlasHeight, kCBTAtlasHeightBinding);
    rebindRing(m_BoundAtlasCoarse, m_DefaultAtlasCoarse, kCBTAtlasCoarseBinding);
    if (m_DefaultPageCache.IsValid())
        rebindRing(m_BoundPageCache, m_DefaultPageCache, kCBTPageCacheBinding);
    if (changed)
        ++m_TerrainSourceGeneration;
    return changed;
}

uint32_t CBTResources::CountTerrainSourceSlots(Rendering::TextureHandle texture) const
{
    if (!texture.IsValid())
        return 0u;
    uint32_t count = 0u;
    for (const auto& h : m_BoundHeight)
        if (h == texture)
            ++count;
    for (const auto& h : m_BoundAtlasHeight)
        if (h == texture)
            ++count;
    for (const auto& h : m_BoundAtlasCoarse)
        if (h == texture)
            ++count;
    for (const auto& h : m_BoundPageCache)
        if (h == texture)
            ++count;
    return count;
}

void CBTResources::UploadAtlasRows(uint32_t frameCounter, const void* rows, uint32_t rowCount)
{
    if (!m_AtlasRows.IsValid())
        return;
    const uint32_t slot = CBTFrameRingSlot(frameCounter);
    const uint32_t clamped = rowCount < kAtlasMaxTiles ? rowCount : kAtlasMaxTiles;
    void* mapped = m_Device->MapBuffer(m_AtlasRows);
    if (!mapped)
        return;
    const size_t slotByteBase =
        static_cast<size_t>(slot) * static_cast<size_t>(kAtlasMaxTiles) * kAtlasRowBytes;
    uint8_t* dst = static_cast<uint8_t*>(mapped) + slotByteBase;
    if (rows != nullptr && clamped > 0u)
        std::memcpy(dst, rows, static_cast<size_t>(clamped) * kAtlasRowBytes);
    // Zero the tail past the live rows so a shrunk table never resolves a stale tile
    // (a leftover 0 Slot would read as slot 0 resident; a full 0xFF row is kAtlasNoSlot).
    const size_t tailRows = static_cast<size_t>(kAtlasMaxTiles) - clamped;
    if (tailRows > 0u)
        std::memset(dst + static_cast<size_t>(clamped) * kAtlasRowBytes, 0xFF,
                    tailRows * kAtlasRowBytes);
    m_Device->UnmapBuffer(m_AtlasRows);
}

void CBTResources::Shutdown()
{
    if (m_Device)
    {
        if (m_DescriptorSet.IsValid())
            m_Device->DestroyDescriptorSet(m_DescriptorSet);
        m_DescriptorSet = {};
        for (auto& b : m_Buffers)
        {
            if (b.IsValid())
                m_Device->DestroyBuffer(b);
            b = {};
        }
        if (m_IdentityIndexBuffer.IsValid())
            m_Device->DestroyBuffer(m_IdentityIndexBuffer);
        m_IdentityIndexBuffer = {};
        if (m_DrawCountBuffer.IsValid())
            m_Device->DestroyBuffer(m_DrawCountBuffer);
        m_DrawCountBuffer = {};
        if (m_FrameParams.IsValid())
            m_Device->DestroyBuffer(m_FrameParams);
        m_FrameParams = {};
        DestroySculptRings();
        if (m_SurfaceParams.IsValid())
            m_Device->DestroyBuffer(m_SurfaceParams);
        m_SurfaceParams = {};
        if (m_DefaultHeight.IsValid())
            m_Device->DestroyTexture(m_DefaultHeight);
        m_DefaultHeight = {};
        m_BoundHeight = {};
        if (m_AtlasRows.IsValid())
            m_Device->DestroyBuffer(m_AtlasRows);
        m_AtlasRows = {};
        if (m_DefaultAtlasHeight.IsValid())
            m_Device->DestroyTexture(m_DefaultAtlasHeight);
        m_DefaultAtlasHeight = {};
        m_BoundAtlasHeight = {};
        if (m_DefaultAtlasCoarse.IsValid())
            m_Device->DestroyTexture(m_DefaultAtlasCoarse);
        m_DefaultAtlasCoarse = {};
        m_BoundAtlasCoarse = {};
        if (m_HeightRange.IsValid())
            m_Device->DestroyBuffer(m_HeightRange);
        m_HeightRange = {};
        if (m_PageTable.IsValid())
            m_Device->DestroyBuffer(m_PageTable);
        m_PageTable = {};
        if (m_DefaultPageCache.IsValid())
            m_Device->DestroyTexture(m_DefaultPageCache);
        m_DefaultPageCache = {};
        m_BoundPageCache = {};
        if (m_HeightSampler.IsValid())
            m_Device->DestroySampler(m_HeightSampler);
        m_HeightSampler = {};
        if (m_Readback.IsValid())
            m_Device->DestroyBuffer(m_Readback);
        m_Readback = {};
    }
    m_PoolSize = 0;
    m_PersistentBytes = 0;
    m_ReadbackSize = 0;
    m_Ready = false;
    m_Device = nullptr;
}

void CBTResources::ReprovisionAfterDeviceRebuild()
{
    // Handle-forget only: every VkObject behind these was already freed by the
    // rebuild teardown, so calling Destroy* here would re-enter destruction on dead
    // handles. Nulling m_Device additionally makes the Shutdown() that Initialize()
    // runs first a no-op, so the re-bring-up cannot resurrect that path.
    m_DescriptorSet = {};
    for (auto& b : m_Buffers)
        b = {};
    m_IdentityIndexBuffer = {};
    m_DrawCountBuffer = {};
    m_FrameParams = {};
    m_SphereSculpt = {};
    m_SphereSculptTable = {};
    m_SculptPagePoolCount = 0u;
    m_SculptPoolSlotWords = 0u;
    m_SculptTableSlotWords = 0u;
    m_SculptRingBytes = 0;
    m_SculptRingsSpherical = false;
    m_SurfaceParams = {};
    m_DefaultHeight = {};
    m_BoundHeight = {};
    m_AtlasRows = {};
    m_DefaultAtlasHeight = {};
    m_BoundAtlasHeight = {};
    m_DefaultAtlasCoarse = {};
    m_BoundAtlasCoarse = {};
    m_HeightRange = {};
    m_PageTable = {};
    m_DefaultPageCache = {};
    m_BoundPageCache = {};
    m_HeightSampler = {};
    m_Readback = {};
    m_PoolSize = 0;
    m_PersistentBytes = 0;
    m_ReadbackSize = 0;
    m_Ready = false;
    m_Device = nullptr;
    // m_TerrainSourceGeneration is deliberately NOT reset: it is a monotonic binding
    // version that readers compare for change, so rewinding it could mask a rebind.
}

BufferHandle CBTResources::GetBuffer(CBTBinding binding) const
{
    const uint32_t idx = static_cast<uint32_t>(binding);
    if (idx >= kCBTBindingCount)
        return {};
    return m_Buffers[idx];
}

uint64_t CBTResources::GetBufferByteSize(CBTBinding binding) const
{
    const uint32_t idx = static_cast<uint32_t>(binding);
    if (idx >= kCBTBindingCount)
        return 0;
    return SpecFor(binding, m_PoolSize).bytes;
}

} // namespace GameEngine::CBTTerrain
