// Material runtime implementation: named property access via StringId,
// CPU cache management, and GPU upload.

#include "Engine/Rendering/Material.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstring>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

// --- Content-change tracking ---

namespace
{
// The single process-wide material content epoch. Lives here (one copy inside
// Engine.dll, reached only through the exported accessors) rather than as a
// header-inline static, which every module would instantiate separately.
std::atomic<uint64_t> s_GlobalContentEpoch{0};
} // namespace

uint64_t Material::GetGlobalContentEpoch()
{
    return s_GlobalContentEpoch.load(std::memory_order_relaxed);
}

void Material::MarkDirty()
{
    ++m_ContentRevision;
    s_GlobalContentEpoch.fetch_add(1, std::memory_order_relaxed);
}

// --- Texture slot name mapping ---

namespace
{
// The fixed well-known name -> slot ladder every surface gets without
// `// @texture` declarations. Single source for both resolution
// (TextureSlotFromName) and the valid-key candidates that
// ValidateDocumentTextureKey lists when a document names an unknown key.
//
// Triplanar entries are the triplanar_pbr front-end binding names. The 8
// shared texture slots hold the 3-axis albedo + normal set, interleaved
// (albedo/normal per axis) so the triset-collapse fast path — one albedo + one
// normal when top==side==bottom — lands in slots 0-1 and leaves 2-5 unbound.
// The 3 normal names are linear-tagged in IsLinearTextureSlot
// (RenderServicesDetail.cpp); the 3 albedo names fall through to the sRGB default.
struct WellKnownSlotName
{
    std::string_view Name;
    StringId Id;
    TextureSlot Slot;
};
constexpr WellKnownSlotName kWellKnownSlotNames[] = {
    {"albedoMap", "albedoMap"_sid, TextureSlot::kAlbedo},
    {"normalMap", "normalMap"_sid, TextureSlot::kNormal},
    {"metallicRoughnessMap", "metallicRoughnessMap"_sid, TextureSlot::kMetalRough},
    {"emissiveMap", "emissiveMap"_sid, TextureSlot::kEmissive},
    {"aoMap", "aoMap"_sid, TextureSlot::kAO},
    {"coatNormalMap", "coatNormalMap"_sid, TextureSlot::kCoatNormal},
    {"roughnessMap", "roughnessMap"_sid, TextureSlot::kRoughness},
    {"metallicMap", "metallicMap"_sid, TextureSlot::kMetallic},
    // Triplanar front-end: albedo top/side/bottom -> slots 0/2/4, normal top/side/bottom -> 1/3/5.
    {"triplanarAlbedoTop", "triplanarAlbedoTop"_sid, TextureSlot::kAlbedo},
    {"triplanarNormalTop", "triplanarNormalTop"_sid, TextureSlot::kNormal},
    {"triplanarAlbedoSide", "triplanarAlbedoSide"_sid, TextureSlot::kMetalRough},
    {"triplanarNormalSide", "triplanarNormalSide"_sid, TextureSlot::kEmissive},
    {"triplanarAlbedoBottom", "triplanarAlbedoBottom"_sid, TextureSlot::kAO},
    {"triplanarNormalBottom", "triplanarNormalBottom"_sid, TextureSlot::kCoatNormal},
};

// GetTextureSlotNames lists the first kTextureSlotArraySize entries as the canonical name of each
// ordinal, so they must stay in ordinal order.
constexpr bool CanonicalLadderIsInOrdinalOrder()
{
    for (size_t ordinal = 0; ordinal < kTextureSlotArraySize; ++ordinal)
    {
        if (static_cast<size_t>(kWellKnownSlotNames[ordinal].Slot) != ordinal)
            return false;
    }
    return true;
}
static_assert(CanonicalLadderIsInOrdinalOrder(), "kWellKnownSlotNames must open with one name per ordinal, in order");
} // namespace

TextureSlot TextureSlotFromName(StringId name)
{
    for (const WellKnownSlotName& entry : kWellKnownSlotNames)
    {
        if (entry.Id == name)
            return entry.Slot;
    }
    return TextureSlot::kCount;
}

// --- Named setters/getters ---

void Material::SetFloat(StringId name, float value)
{
    if (const PropertyLayout* pl = FindProperty(name))
    {
        if (pl->size >= sizeof(float))
        {
            // Same-value early-out: document re-registration (per-frame for
            // particle emitters) re-applies every property; idempotent writes
            // must not dirty UBO slots or bust the PackMaterialSSBO skip.
            if (std::memcmp(m_CpuCache.data() + pl->offset, &value, sizeof(float)) == 0)
                return;
            std::memcpy(m_CpuCache.data() + pl->offset, &value, sizeof(float));
            MarkDirty();
        }
    }
}

float Material::GetFloat(StringId name, float fallback) const
{
    if (const PropertyLayout* pl = FindProperty(name))
    {
        if (pl->size >= sizeof(float))
        {
            float v;
            std::memcpy(&v, m_CpuCache.data() + pl->offset, sizeof(float));
            return v;
        }
    }
    return fallback;
}

void Material::SetVector(StringId name, const float* values, uint32_t componentCount)
{
    if (!values || componentCount == 0)
        return;
    if (const PropertyLayout* pl = FindProperty(name))
    {
        const uint32_t writeBytes = componentCount * sizeof(float);
        if (writeBytes <= pl->size)
        {
            if (std::memcmp(m_CpuCache.data() + pl->offset, values, writeBytes) == 0)
                return;
            std::memcpy(m_CpuCache.data() + pl->offset, values, writeBytes);
            MarkDirty();
        }
    }
}

bool Material::GetVector(StringId name, float* outValues, uint32_t componentCount) const
{
    if (!outValues || componentCount == 0)
        return false;
    if (const PropertyLayout* pl = FindProperty(name))
    {
        const uint32_t readBytes = componentCount * sizeof(float);
        if (readBytes <= pl->size)
        {
            std::memcpy(outValues, m_CpuCache.data() + pl->offset, readBytes);
            return true;
        }
    }
    return false;
}

void Material::SetColor(StringId name, const float* rgba)
{
    if (rgba)
        SetVector(name, rgba, 4);
}

void Material::SetTexture(StringId name, Rendering::TextureHandle tex)
{
    if (const uint32_t ordinal = ResolveTextureSlotOrdinal(name); ordinal < kTextureSlotArraySize)
        m_AwaitedTextureSlots &= ~(1u << ordinal);
    // Same-value early-out: per-frame rebinds (video textures re-asserting
    // their override, document re-application) must not dirty UBO slots or
    // bust the PackMaterialSSBO skip.
    auto [it, inserted] = m_Textures.try_emplace(name, tex);
    if (!inserted)
    {
        if (it->second == tex)
            return;
        it->second = tex;
    }
    MarkDirty();
}

void Material::MarkTextureAwaited(StringId name)
{
    if (const uint32_t ordinal = ResolveTextureSlotOrdinal(name); ordinal < kTextureSlotArraySize)
    {
        m_AwaitedTextureSlots |= 1u << ordinal;
        m_AwaitedTextureDefaults[ordinal] = AwaitedSlotDefault(TextureSlotFromName(name));
    }
}

bool Material::IsTextureAwaited(uint32_t slotOrdinal) const
{
    return slotOrdinal < kTextureSlotArraySize && (m_AwaitedTextureSlots & (1u << slotOrdinal)) != 0;
}

bool Material::IsTextureAwaited(StringId name) const
{
    return IsTextureAwaited(ResolveTextureSlotOrdinal(name));
}

SlotDefaultTexture Material::GetAwaitedTextureDefault(uint32_t slotOrdinal) const
{
    return slotOrdinal < kTextureSlotArraySize ? m_AwaitedTextureDefaults[slotOrdinal] : SlotDefaultTexture::White;
}

Rendering::TextureHandle Material::GetTexture(StringId name) const
{
    auto it = m_Textures.find(name);
    return (it != m_Textures.end()) ? it->second : Rendering::TextureHandle{};
}

Rendering::TextureHandle Material::GetTextureForSlot(uint32_t slotOrdinal) const
{
    if (slotOrdinal >= kTextureSlotArraySize)
        return {};
    // Linear over the binding map: a material carries at most a handful of
    // textures, and the map is name-keyed because that is what authoring and
    // hot-reload address slots by.
    for (const auto& [name, tex] : m_Textures)
    {
        if (tex.IsValid() && ResolveTextureSlotOrdinal(name) == slotOrdinal)
            return tex;
    }
    return {};
}

uint32_t Material::ResolveTextureSlotOrdinal(StringId name) const
{
    // A surface that declares its texture set via `// @texture <name>` routes
    // user (and well-known) names through the resolved map the shader compiled
    // against. A name it does not declare is unbound, never the ladder's ordinal:
    // on the standard surface ordinal 6 is heightMap, and the ladder would route a
    // stale roughnessMap key onto it. Legacy surfaces have an empty map and fall
    // through to TextureSlotFromName.
    if (auto it = m_UserTextureSlotMap.find(name); it != m_UserTextureSlotMap.end())
        return it->second;
    if (!m_UserTextureSlotMap.empty())
        return kTextureSlotArraySize;
    const TextureSlot slot = TextureSlotFromName(name);
    return (slot == TextureSlot::kCount) ? kTextureSlotArraySize : static_cast<uint32_t>(slot);
}

void Material::SetDeclaredProperties(std::shared_ptr<const Rendering::ShaderPropertyTable> table)
{
    m_DeclaredProperties = std::move(table);
    m_PropertyLayouts.clear();
    // Rebuild the block from scratch, then dirty only if the bytes moved: the
    // per-frame re-registration path replays this for unchanged documents.
    const size_t blockBytes = std::min<size_t>(kMaterialParamBlockBytes, m_CpuCache.size());
    std::array<uint8_t, kMaterialParamBlockBytes> before{};
    std::copy_n(m_CpuCache.begin(), blockBytes, before.begin());
    std::fill_n(m_CpuCache.begin(), blockBytes, uint8_t{0});
    if (m_DeclaredProperties)
    {
        for (const Rendering::ShaderProperty& p : m_DeclaredProperties->Properties)
        {
            if (!p.HasLane)
                continue;
            m_PropertyLayouts[HashStringId(p.Name)] = {p.ByteOffset, p.ByteSize};
            WriteDeclaredComponents(p, p.Default.data(), p.ComponentCount());
        }
        for (const auto& [name, value] : m_AuthoredProperties)
            SetDeclaredPropertyValue(name, value);
    }
    if (!std::equal(before.begin(), before.begin() + static_cast<std::ptrdiff_t>(blockBytes), m_CpuCache.begin()))
        MarkDirty();
}

bool Material::SetDeclaredPropertyValue(std::string_view name, const MaterialValue& value)
{
    if (!m_DeclaredProperties)
        return false;
    const Rendering::ShaderProperty* p = m_DeclaredProperties->Find(name);
    if (!p || !p->HasLane)
        return false;

    std::array<float, 4> components = p->Default;
    if (const float* f = std::get_if<float>(&value))
        components[0] = *f;
    else if (const int32_t* i = std::get_if<int32_t>(&value))
        components[0] = static_cast<float>(*i);
    else if (const bool* b = std::get_if<bool>(&value))
        components[0] = *b ? 1.0f : 0.0f;
    else if (const std::vector<float>* v = std::get_if<std::vector<float>>(&value))
    {
        const size_t n = std::min<size_t>(v->size(), p->ComponentCount());
        std::copy(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(n), components.begin());
    }
    WriteDeclaredComponents(*p, components.data(), p->ComponentCount());
    return true;
}

void Material::WriteDeclaredComponents(const Rendering::ShaderProperty& property, const float* components,
                                       uint32_t componentCount)
{
    if (property.ByteOffset + componentCount * sizeof(float) > m_CpuCache.size())
        return;
    uint8_t* dst = m_CpuCache.data() + property.ByteOffset;
    for (uint32_t i = 0; i < componentCount; ++i)
    {
        std::array<uint8_t, 4> bytes{};
        switch (property.Type)
        {
        case Rendering::ShaderPropertyType::Int:
        case Rendering::ShaderPropertyType::Enum:
        {
            const int32_t v = static_cast<int32_t>(components[i]);
            std::memcpy(bytes.data(), &v, sizeof(v));
            break;
        }
        case Rendering::ShaderPropertyType::Bool:
        {
            const float v = components[i] != 0.0f ? 1.0f : 0.0f;
            std::memcpy(bytes.data(), &v, sizeof(v));
            break;
        }
        default:
            std::memcpy(bytes.data(), &components[i], sizeof(float));
            break;
        }
        std::memcpy(dst + i * sizeof(float), bytes.data(), bytes.size());
    }
}

std::vector<std::string_view> Material::GetTextureSlotNames() const
{
    std::vector<std::string_view> names;
    if (!m_UserTextureSlotNames.empty())
    {
        names.assign(m_UserTextureSlotNames.begin(), m_UserTextureSlotNames.end());
        return names;
    }
    names.reserve(kTextureSlotArraySize);
    for (size_t ordinal = 0; ordinal < kTextureSlotArraySize; ++ordinal)
        names.push_back(kWellKnownSlotNames[ordinal].Name);
    return names;
}

void Material::SetTextureSlotMap(std::vector<std::pair<std::string, uint8_t>> slots)
{
    m_UserTextureSlotMap.clear();
    m_UserTextureSlotMap.reserve(slots.size());
    m_UserTextureSlotNames.clear();
    m_UserTextureSlotNames.reserve(slots.size());
    for (auto& [name, ordinal] : slots)
    {
        m_UserTextureSlotMap[HashStringId(name)] = ordinal;
        m_UserTextureSlotNames.push_back(std::move(name));
    }

    // The map arrives AFTER registration seeded the ladder, so the user ordinals
    // are still holding whatever their ordinal means to the engine. Re-seed them
    // — but never over a slot that already carries a texture: re-registration
    // installs the same map on a material whose bindings are already live.
    if (!m_HasBindlessDefaults)
        return;
    for (const auto& [name, ordinal] : m_UserTextureSlotMap)
    {
        if (!IsUserOwnedSlot(name, ordinal))
            continue;
        if (const auto it = m_Textures.find(name); it != m_Textures.end() && it->second.IsValid())
            continue;
        if (m_BindlessTextureIndices[ordinal] == m_DefaultWhiteIndex)
            continue;
        m_BindlessTextureIndices[ordinal] = m_DefaultWhiteIndex;
        MarkDirty();
    }
}

bool Material::IsUserOwnedSlot(StringId name, uint8_t ordinal) const
{
    if (ordinal >= kTextureSlotArraySize)
        return false;
    // A declared WELL-KNOWN name keeps its ladder default: the engine knows what
    // that slot means (flat normal, black metallic, ...). Only a name the engine
    // knows nothing about needs the identity default.
    return TextureSlotFromName(name) == TextureSlot::kCount;
}

bool Material::ValidateDocumentTextureKey(std::string_view key)
{
    const StringId keyId = HashStringId(key);
    if (ResolveTextureSlotOrdinal(keyId) != kTextureSlotArraySize)
    {
        // Re-arm: a key reported unknown and later declared by the surface
        // (`// @texture <key>`) must report again if the declaration goes away.
        if (!m_WarnedUnknownTextureKeys.empty())
            m_WarnedUnknownTextureKeys.erase(keyId);
        return true;
    }

    // Documents re-apply per frame (particle emitters), so an unknown key
    // reports once per material, not once per application.
    if (!m_WarnedUnknownTextureKeys.insert(keyId).second)
        return false;

    // The keys this material accepts: a declaring surface's own names, or every ladder name
    // (the canonical eight and the triplanar aliases that resolve onto them).
    std::string candidates;
    if (m_UserTextureSlotNames.empty())
    {
        for (const WellKnownSlotName& entry : kWellKnownSlotNames)
        {
            candidates += entry.Name;
            candidates += ", ";
        }
    }
    else
    {
        for (const std::string& name : m_UserTextureSlotNames)
        {
            candidates += name;
            candidates += ", ";
        }
    }
    candidates.resize(candidates.size() - 2);

    Logger::Log::Warning(
        "Material '{}' ({}): unknown texture key '{}' — this texture is skipped and the slot keeps "
        "its default. Fix the key in the .material document to one of: {}. (Or declare it in the "
        "surface shader with '// @texture {}'.)",
        m_Name,
        m_MaterialAssetPath.empty() ? m_Guid.ToString() : m_MaterialAssetPath.string(),
        key, candidates, key);
    return false;
}

void Material::PruneTextureKeyWarnings(const std::unordered_map<std::string, std::string>& documentTextures)
{
    if (m_WarnedUnknownTextureKeys.empty())
        return;
    std::unordered_set<StringId> documentKeys;
    documentKeys.reserve(documentTextures.size());
    for (const auto& [name, _] : documentTextures)
        documentKeys.insert(HashStringId(name));
    std::erase_if(m_WarnedUnknownTextureKeys,
                  [&](StringId key) { return documentKeys.find(key) == documentKeys.end(); });
}

void Material::SetBindlessTextureIndex(StringId name, uint32_t descriptorIndex)
{
    const uint32_t slot = ResolveTextureSlotOrdinal(name);
    // Unknown names are a no-op, mirroring the getter's 0: document keys are
    // validated where documents are applied (ValidateDocumentTextureKey), but
    // a name can legitimately stop resolving between track time and replay —
    // hot-reload sweeps and device-rebuild replays re-apply slot names
    // recorded before a surface's @texture set changed.
    if (slot == kTextureSlotArraySize)
        return;
    // Same-value early-out: TextureService re-applies unchanged indices on
    // document updates; a no-op write must not dirty UBO slots or bust the
    // PackMaterialSSBO skip stamp.
    if (m_BindlessTextureIndices[slot] == descriptorIndex)
        return;
    m_BindlessTextureIndices[slot] = descriptorIndex;
    MarkDirty();
}

uint32_t Material::GetBindlessTextureIndex(StringId name) const
{
    const uint32_t slot = ResolveTextureSlotOrdinal(name);
    if (slot == kTextureSlotArraySize)
        return 0u;
    return m_BindlessTextureIndices[slot];
}

void Material::SetBindlessTextureIndex(TextureSlot slot, uint32_t descriptorIndex)
{
    assert(static_cast<size_t>(slot) < kTextureSlotArraySize);
    if (m_BindlessTextureIndices[static_cast<size_t>(slot)] == descriptorIndex)
        return;
    m_BindlessTextureIndices[static_cast<size_t>(slot)] = descriptorIndex;
    MarkDirty();
}

uint32_t Material::GetBindlessTextureIndex(TextureSlot slot) const
{
    assert(static_cast<size_t>(slot) < kTextureSlotArraySize);
    return m_BindlessTextureIndices[static_cast<size_t>(slot)];
}

const uint32_t (&Material::GetBindlessTextureIndices() const)[kTextureSlotArraySize]
{
    return m_BindlessTextureIndices;
}

void Material::SetTextureTransform(TextureSlot slot, float scaleX, float scaleY, float offsetX, float offsetY)
{
    const uint32_t idx = static_cast<uint32_t>(slot);
    if (idx >= kTextureSlotArraySize) return;
    float* st = &m_TextureTransforms[idx * 8];
    st[0] = scaleX; st[1] = 0.0f;   st[2] = offsetX; st[3] = 0.0f;
    st[4] = 0.0f;   st[5] = scaleY; st[6] = offsetY; st[7] = 0.0f;
    MarkDirty();
}

void Material::SetTextureTransform(TextureSlot slot, const float (&matrixRows)[8])
{
    const uint32_t idx = static_cast<uint32_t>(slot);
    if (idx >= kTextureSlotArraySize) return;
    std::memcpy(&m_TextureTransforms[idx * 8], matrixRows, 8 * sizeof(float));
    MarkDirty();
}

void Material::SetTextureTransform(TextureSlot slot, const std::array<float, 8>& matrixRows)
{
    const uint32_t idx = static_cast<uint32_t>(slot);
    if (idx >= kTextureSlotArraySize) return;
    std::memcpy(&m_TextureTransforms[idx * 8], matrixRows.data(), 8 * sizeof(float));
    MarkDirty();
}

void Material::SetTextureTransform(StringId name, const std::array<float, 8>& matrixRows)
{
    const uint32_t slot = ResolveTextureSlotOrdinal(name);
    if (slot == kTextureSlotArraySize)
        return;
    SetTextureTransform(static_cast<TextureSlot>(slot), matrixRows);
}

void Material::GetTextureTransform(TextureSlot slot, float& scaleX, float& scaleY, float& offsetX, float& offsetY) const
{
    const uint32_t idx = static_cast<uint32_t>(slot);
    if (idx >= kTextureSlotArraySize) { scaleX = 1; scaleY = 1; offsetX = 0; offsetY = 0; return; }
    const float* st = &m_TextureTransforms[idx * 8];
    scaleX = st[0]; scaleY = st[5]; offsetX = st[2]; offsetY = st[6];
}

const float (&Material::GetTextureTransforms() const)[kTextureSlotArraySize * 8]
{
    return m_TextureTransforms;
}

void Material::InitBindlessDefaults(uint32_t whiteIdx, uint32_t flatNormalIdx, uint32_t blackIdx)
{
    // Every slot the adapter declares must be initialised to a valid bindless
    // descriptor. Bindless index 0 is the engine-wide "not set" sentinel
    // (BindlessResourceManager hands out indices starting at 1), and the
    // bindless texture array's descriptor[0] is never written. Sampling
    // ge_BindlessTextures[0] in the surface shader returns undefined data —
    // NVIDIA drivers tend to alias the most recently written descriptor,
    // producing the "stale texture leaks onto a plane that has none" symptom
    // for any material that uses an extended PBR surface (which unconditionally
    // samples AO / coat normal / separate roughness / separate metallic).
    for (uint32_t ordinal = 0; ordinal < kTextureSlotArraySize; ++ordinal)
    {
        switch (UnassignedSlotDefault(static_cast<TextureSlot>(ordinal)))
        {
        case SlotDefaultTexture::White:      m_BindlessTextureIndices[ordinal] = whiteIdx; break;
        case SlotDefaultTexture::FlatNormal: m_BindlessTextureIndices[ordinal] = flatNormalIdx; break;
        case SlotDefaultTexture::Black:      m_BindlessTextureIndices[ordinal] = blackIdx; break;
        }
    }
    m_DefaultWhiteIndex = whiteIdx;
    m_HasBindlessDefaults = true;

    // The ladder above is keyed by what a slot MEANS, and a user @texture name
    // has no engine meaning — so its ordinal's ladder default is arbitrary for
    // it. Re-seed those to white unconditionally: this call is a FULL re-bake
    // (registration, and again on device rebuild where every previously-baked
    // index is dead), and the caller replays real bindings afterwards. At
    // registration the map is still empty; SetTextureSlotMap seeds it when the
    // map arrives.
    for (const auto& [name, ordinal] : m_UserTextureSlotMap)
    {
        if (IsUserOwnedSlot(name, ordinal))
            m_BindlessTextureIndices[ordinal] = whiteIdx;
    }
    MarkDirty();
}

// --- Internal ---

void Material::InitCache(uint32_t sizeBytes)
{
    // Ensure 16-byte alignment for std140 UBO compatibility.
    const uint32_t aligned = (sizeBytes + 15u) & ~15u;
    m_CpuCache.resize(aligned, 0);
    MarkDirty();
}

const Material::PropertyLayout* Material::FindProperty(StringId name) const
{
    auto it = m_PropertyLayouts.find(name);
    return (it != m_PropertyLayouts.end()) ? &it->second : nullptr;
}

Rendering::GraphicsPipelineId Material::GetGraphicsPipelineIdForFlags(
    Rendering::VertexAttributeFlags flags, Rendering::IDevice* device) const
{
    if (!m_GraphicsPipelineId.IsValid() || !device)
        return {};

    {
        std::lock_guard lock(*m_PerFlagsIdMutex);
        if (auto it = m_PerFlagsId.find(flags); it != m_PerFlagsId.end())
            return it->second;
    }

    const Rendering::GraphicsPipelineDesc* baseDesc =
        device->LookupGraphicsPipeline(m_GraphicsPipelineId);
    if (!baseDesc)
        return {};

    Rendering::GraphicsPipelineDesc variant = *baseDesc;
    Rendering::BuildVertexLayoutFromFlags(flags, variant);
    Rendering::GraphicsPipelineId variantId =
        device->InternGraphicsPipeline(std::move(variant));

    {
        std::lock_guard lock(*m_PerFlagsIdMutex);
        m_PerFlagsId.emplace(flags, variantId);
    }
    return variantId;
}

} // namespace Engine::Renderer
} // namespace GameEngine
