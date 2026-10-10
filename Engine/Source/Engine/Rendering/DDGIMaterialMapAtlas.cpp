#include "Engine/Rendering/DDGIMaterialMapAtlas.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Types/StringId.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace GameEngine::Engine::Renderer
{

namespace
{

// The engine's canonical base-colour texture slot name (Material.cpp's
// well-known slot ladder). Also covers triplanar surfaces, which alias the
// same ordinal — a triplanar top map in the atlas is a rough stand-in for the
// blend, which is still far closer to the surface's real colour than a flat
// grey factor.
constexpr StringId kAlbedoSlotName = HashStringId("albedoMap");

// The canonical emissive slot name. Material::GetTexture keys on the authored
// NAME, so the triplanar side-normal map that shares this slot's ordinal is
// not reachable through it — an emissive layer is always emissive colour.
constexpr StringId kEmissiveSlotName = HashStringId("emissiveMap");

// The canonical metallic-roughness slot name (glTF packing: G roughness, B
// metallic). Uploaded linear (IsLinearTextureSlot, Assets/TextureCook.h), so
// the blit stores its channels untouched.
constexpr StringId kMetallicSlotName = HashStringId("metallicRoughnessMap");

// Sentinel for Material::GetGpuSceneMaterialIndex on a material with no SSBO
// row. Mirrors Material::kInvalidSSBOIndex, which is private (same duplication,
// same reason, as DDGISceneService's copy).
constexpr uint32_t kUnassignedMaterialSlot = 0xFFFFFFFFu;

// Guards against a corrupt slot index turning into a multi-gigabyte table.
constexpr uint32_t kMaxMaterialSlots = 65536u;

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime       = 1099511628211ull;

void HashBytes(uint64_t& hash, const void* data, size_t bytes)
{
    const auto* cursor = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= cursor[i];
        hash *= kFnvPrime;
    }
}

const DDGIMaterialMapAtlas::MaterialRecord kDefaultRecord{};

// Dev/QA A/B switches: GE_DDGI_ALBEDO_MAPS=0 / GE_DDGI_EMISSIVE_MAPS=0 /
// GE_DDGI_METALLIC_MAPS=0 keep every material on the corresponding flat
// factor, i.e. reproduce the pre-textured bounce for a side-by-side against
// the same binary. Independent because each carries its own signal (colour
// bleed, emitted light, how much of the surface bounces at all) and isolating
// one at a time is the point. Mirrors this codebase's
// established GE_DDGI_FORCE_SOFTWARE / GE_VK_* env-var convention rather than
// adding an end-user settings page for a developer switch. Read once per
// process.
bool EnvFlagEnabled(const char* name)
{
    const char* v = std::getenv(name);
    return !(v && v[0] == '0');
}

bool AlbedoMapsEnabled()
{
    static const bool enabled = EnvFlagEnabled("GE_DDGI_ALBEDO_MAPS");
    return enabled;
}

bool EmissiveMapsEnabled()
{
    static const bool enabled = EnvFlagEnabled("GE_DDGI_EMISSIVE_MAPS");
    return enabled;
}

bool MetallicMapsEnabled()
{
    static const bool enabled = EnvFlagEnabled("GE_DDGI_METALLIC_MAPS");
    return enabled;
}

}  // namespace

DDGIMaterialMapAtlas::DDGIMaterialMapAtlas(Rendering::IDevice* device, MaterialSystem* materials)
    : m_Device(device), m_Materials(materials)
{
}

DDGIMaterialMapAtlas::~DDGIMaterialMapAtlas()
{
    if (!m_Device)
        return;
    ReleaseAll();
    for (const RetiredResources& retired : m_Retired)
    {
        if (retired.Texture.IsValid())
            m_Device->DestroyTexture(retired.Texture);
        if (retired.Buffer.IsValid())
            m_Device->DestroyBuffer(retired.Buffer);
    }
    m_Retired.clear();
}

const DDGIMaterialMapAtlas::MaterialRecord& DDGIMaterialMapAtlas::GetRecordForMaterialSlot(
    uint32_t gpuSceneMaterialIndex) const
{
    if (gpuSceneMaterialIndex >= m_RecordsByMaterialSlot.size())
        return kDefaultRecord;
    return m_RecordsByMaterialSlot[gpuSceneMaterialIndex];
}

void DDGIMaterialMapAtlas::Refresh()
{
    if (!m_Device || !m_Materials)
        return;

    ++m_FrameClock;
    CollectRetired();

    // Sweep: every material holding a GPU-scene SSBO row contributes one
    // record; distinct source textures are deduplicated into layers in
    // first-seen order, which is what the layout hash below is taken over.
    // Dedup spans every map kind, so one texture used as base colour here and
    // as emissive there occupies a single layer.
    std::vector<Rendering::TextureHandle> layerSources;
    std::vector<MaterialRecord> records;
    std::unordered_map<uint64_t, uint32_t> layerByTextureId;
    bool exceededLayerCeiling = false;

    // An unbound texture keeps kNoLayer, which is the reference's own fallback
    // for an unreadable source: the flat factor stands in, and a later Refresh
    // picks the texture up once TextureService binds a real handle to the slot.
    //
    // Layer ASSIGNMENT is all this decides. Whether the layer has pixels in it
    // is tracked separately (m_LayerFill), because the two can lag each other:
    // an assigned layer whose blit never got recorded would otherwise stay
    // black forever, the layout hash having no reason to move again.
    auto assignLayer = [&](const Rendering::TextureHandle& source) -> float
    {
        if (!source.IsValid())
            return kNoLayer;
        const auto existing = layerByTextureId.find(source.id);
        if (existing != layerByTextureId.end())
            return static_cast<float>(existing->second);
        if (layerSources.size() >= kMaxLayers)
        {
            exceededLayerCeiling = true;
            return kNoLayer;
        }
        const auto layer = static_cast<uint32_t>(layerSources.size());
        layerSources.push_back(source);
        layerByTextureId.emplace(source.id, layer);
        return static_cast<float>(layer);
    };

    m_Materials->Registry().ForEach(
        [&](const GUID&, const Material& material)
        {
            const uint32_t slot = material.GetGpuSceneMaterialIndex();
            if (slot == kUnassignedMaterialSlot || slot >= kMaxMaterialSlots)
                return;

            MaterialRecord record{};
            material.GetTextureTransform(TextureSlot::kAlbedo, record.ScaleX, record.ScaleY,
                                         record.OffsetX, record.OffsetY);
            record.AlbedoLayer = assignLayer(
                AlbedoMapsEnabled() ? material.GetTexture(kAlbedoSlotName) : Rendering::TextureHandle{});
            record.EmissiveLayer =
                material.IsTextureAwaited(kEmissiveSlotName)
                    ? kAwaitedLayer
                    : assignLayer(EmissiveMapsEnabled() ? material.GetTexture(kEmissiveSlotName)
                                                        : Rendering::TextureHandle{});
            record.MetallicLayer = assignLayer(
                MetallicMapsEnabled() ? material.GetTexture(kMetallicSlotName) : Rendering::TextureHandle{});

            if (records.size() <= slot)
                records.resize(slot + 1);
            records[slot] = record;
        });

    uint64_t hash = kFnvOffsetBasis;
    for (const Rendering::TextureHandle& source : layerSources)
        HashBytes(hash, &source.id, sizeof(source.id));
    for (const MaterialRecord& record : records)
        HashBytes(hash, &record, sizeof(record));

    // A settled layout still falls through to the re-arm below: the layer set
    // being unchanged says nothing about whether every layer's contents landed.
    if (hash == m_LayoutHash)
    {
        RearmPendingBlits();
        return;
    }

    if (exceededLayerCeiling)
    {
        Logger::Log::Warning(
            "DDGIMaterialMapAtlas: more than {} distinct base-colour/emissive/metallic-roughness "
            "textures in the scene; the excess materials bounce their flat factors",
            kMaxLayers);
    }

    const bool layersChanged = layerSources != m_LayerSources;
    m_LayerSources = std::move(layerSources);
    m_RecordsByMaterialSlot = std::move(records);
    m_LayoutHash = hash;
    ++m_LayoutEpoch;

    // `!m_Texture.IsValid()` covers the first Refresh in a scene with no maps
    // at all: the layer set is unchanged (empty both sides) but the spare-layer
    // image still has to exist for the trace kernels to have something to bind.
    if (layersChanged || !m_Texture.IsValid())
    {
        // The array texture's layer count is fixed at creation, so any change
        // to the layer set is a full recreate — and every layer then needs
        // re-filling, since a recreated image holds nothing.
        RecreateTexture(static_cast<uint32_t>(m_LayerSources.size()));
        m_LayerFill.assign(m_LayerSources.size(), LayerFill{});
    }

    UploadTable();
    RearmPendingBlits();

    if (const char* v = std::getenv("GE_DDGI_ATLAS_TRACE"); v && v[0] != '\0' && v[0] != '0')
    {
        std::string ids;
        for (size_t i = 0; i < m_LayerSources.size(); ++i)
            ids += (i ? ", " : "") + std::to_string(i) + "->tex" + std::to_string(m_LayerSources[i].id);
        Logger::Log::Info("[DDGIAtlasTrace] layer sources: [{}]", ids);
    }
    Logger::Log::Info("DDGIMaterialMapAtlas: {} map layers ({}x{} RGBA8) across {} material slots{}{}",
                      m_LayerSources.size(), kLayerSize, kLayerSize, m_RecordsByMaterialSlot.size(),
                      AlbedoMapsEnabled() ? "" : " — base-colour maps disabled by GE_DDGI_ALBEDO_MAPS=0",
                      EmissiveMapsEnabled() ? "" : " — emissive maps disabled by GE_DDGI_EMISSIVE_MAPS=0");
}

// Re-derives the pending set from per-layer contents state rather than arming
// it once at recreate time.
//
// The failure this exists to close: a layer is assigned, one blit is queued,
// the pass declares but never records (pipeline variant still compiling,
// reflection unavailable, pass culled) and the entry is dropped. Nothing
// afterwards changes the layout hash — the handle ids are the same — so the
// layer samples as black for the lifetime of the scene. That is the cold-editor
// "textured emissive reads flat" symptom: the identical scene in a long-running
// editor has had its layer set churn often enough for a later recreate to
// re-arm the blit, which is why it only reproduces from a fresh start.
//
// Re-arming is self-terminating: each layer stops being queued the frame after
// its dispatch is recorded, so the steady state is an empty pending list.
void DDGIMaterialMapAtlas::RearmPendingBlits()
{
    m_PendingBlits.clear();
    if (!m_Texture.IsValid())
        return;

    m_LayerFill.resize(m_LayerSources.size());
    for (uint32_t layer = 0; layer < m_LayerSources.size(); ++layer)
    {
        LayerFill& fill = m_LayerFill[layer];
        if (fill.Filled)
            continue;
        if (fill.Recorded && fill.Recorded->load(std::memory_order_acquire))
        {
            fill.Filled = true;
            fill.Recorded.reset();
            continue;
        }
        // A source is only ever assigned a layer while valid, but it can be
        // evicted between Refreshes; blitting from a handle the device no
        // longer knows is a descriptor write against a destroyed image.
        if (!m_LayerSources[layer].IsValid())
            continue;
        if (!fill.Recorded)
            fill.Recorded = std::make_shared<std::atomic<bool>>(false);
        m_PendingBlits.push_back(PendingBlit{m_LayerSources[layer], layer, fill.Recorded});
    }
}

void DDGIMaterialMapAtlas::RecreateTexture(uint32_t layerCount)
{
    if (m_Texture.IsValid())
    {
        m_Retired.push_back(RetiredResources{m_Texture, {}, m_FrameClock});
        m_Texture = {};
    }
    // At least one layer, always. The trace kernels statically reference the
    // atlas sampler even in a scene with no maps (a `layer < 0` early-out is a
    // runtime branch, not a compile-time one), and leaving a statically-used
    // combined-image-sampler descriptor unwritten is exactly the silent-binding
    // failure class this feature has already been bitten by. The spare layer
    // costs 256 KiB and is never sampled: no material record points at it.
    Rendering::TextureDesc desc{};
    desc.width       = kLayerSize;
    desc.height      = kLayerSize;
    desc.depth       = 1;
    desc.mipLevels   = 1;
    desc.arrayLayers = std::max(layerCount, 1u);
    // Both trace kernels sample this as an array and the blit stores through
    // image2DArray, so it must stay array-typed even in a scene with a single
    // map layer — otherwise the backend picks a plain 2D texture and every
    // array-typed store into it is discarded.
    desc.flags       = Rendering::TextureCreateFlags::ForceArrayView;
    desc.sampleCount = 1;
    // RGBA8_UNORM, not the rgba16f every probe atlas uses: this holds colour in
    // [0,1] — base colour, and emissive texel colour whose HDR scale rides in
    // the material's authored luminance, not in the texture — so 8-bit linear
    // steps of ~0.4% are invisible in a diffuse bounce, at half the memory.
    // UNORM (not _SRGB) because the blit already resolved the source's colour
    // space.
    desc.format      = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
    desc.usage       = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                             Rendering::TextureUsage::ShaderResource);
    desc.persistent  = true;
    // A layer with no source is never blitted, so without this the image would
    // still be in Undefined layout the first time a trace pass binds it.
    desc.initialState = Rendering::ResourceState::ShaderResource;
    desc.debugName   = "DDGI.MaterialMapAtlas";
    Logger::Log::Info("[DDGIAtlasTrace] creating atlas: {} layers, {}x{}, usage={:#x}",
                      desc.arrayLayers, desc.width, desc.height, desc.usage);
    m_Texture = m_Device->CreateTexture(desc);
    if (!m_Texture.IsValid())
        Logger::Log::Error("DDGIMaterialMapAtlas: failed to create the {}-layer map atlas",
                           desc.arrayLayers);

    if (!m_Sampler.IsValid())
    {
        // Repeat, matching the reference — a material's uv scale above 1 must
        // tile, not clamp to the edge texel.
        m_Sampler = m_Device->CreateSampler(
            Rendering::SamplerDesc::MaterialLinearRepeat("DDGI.MaterialMapAtlasSampler"));
    }
}

void DDGIMaterialMapAtlas::UploadTable()
{
    // Two vec4s per material row (GE_DDGIMaterialMapRecord): the hardware lane
    // has no packed uber-material table to carry these in, so it reads them
    // from here with the same index it reads uBaseColor with.
    struct TableRow
    {
        float Layers[4];       // x = albedo, y = emissive, z = metallic-roughness
        float UvTransform[4];  // xy = scale, zw = offset
    };

    if (m_TableBuffer.IsValid())
    {
        m_Retired.push_back(RetiredResources{{}, m_TableBuffer, m_FrameClock});
        m_TableBuffer = {};
        m_TableBytes  = 0;
    }
    // Always at least one row, for the same reason the atlas always has at
    // least one layer: the hardware kernel statically references this buffer,
    // and an unwritten storage-buffer descriptor is a silent failure, not a
    // dimmer frame. A world with no GPU-scene materials gets one kNoLayer row.
    std::vector<TableRow> rows(std::max<size_t>(m_RecordsByMaterialSlot.size(), 1),
                               TableRow{{kNoLayer, kNoLayer, kNoLayer, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}});
    for (size_t i = 0; i < m_RecordsByMaterialSlot.size(); ++i)
    {
        const MaterialRecord& record = m_RecordsByMaterialSlot[i];
        rows[i] = TableRow{{record.AlbedoLayer, record.EmissiveLayer, record.MetallicLayer, 0.0f},
                           {record.ScaleX, record.ScaleY, record.OffsetX, record.OffsetY}};
    }

    Rendering::BufferDesc desc{};
    desc.size        = rows.size() * sizeof(TableRow);
    desc.usage       = static_cast<uint32_t>(Rendering::BufferUsage::Storage) |
                       static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    desc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    desc.persistent  = true;
    desc.debugName   = "DDGI.MaterialMapTable";
    m_TableBuffer    = m_Device->CreateBuffer(desc);
    if (!m_TableBuffer.IsValid())
    {
        Logger::Log::Error("DDGIMaterialMapAtlas: failed to create the material map lookup table");
        return;
    }
    m_Device->UpdateBuffer(m_TableBuffer, 0, desc.size, rows.data());
    m_TableBytes = desc.size;
}

void DDGIMaterialMapAtlas::CollectRetired()
{
    constexpr uint64_t kRetireMargin = Rendering::IDevice::kMaxSupportedFramesInFlight + 1;
    std::erase_if(m_Retired,
                  [&](const RetiredResources& retired)
                  {
                      if (m_FrameClock - retired.FrameStamp <= kRetireMargin)
                          return false;
                      if (retired.Texture.IsValid())
                          m_Device->DestroyTexture(retired.Texture);
                      if (retired.Buffer.IsValid())
                          m_Device->DestroyBuffer(retired.Buffer);
                      return true;
                  });
}

void DDGIMaterialMapAtlas::ReleaseAll()
{
    if (m_Texture.IsValid())
        m_Retired.push_back(RetiredResources{m_Texture, {}, m_FrameClock});
    if (m_TableBuffer.IsValid())
        m_Retired.push_back(RetiredResources{{}, m_TableBuffer, m_FrameClock});
    if (m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
    m_Texture     = {};
    m_TableBuffer = {};
    m_Sampler     = {};
    m_TableBytes  = 0;
    m_LayerSources.clear();
    m_LayerFill.clear();
    m_RecordsByMaterialSlot.clear();
    m_PendingBlits.clear();
    m_LayoutHash = 0;
}

}  // namespace GameEngine::Engine::Renderer
