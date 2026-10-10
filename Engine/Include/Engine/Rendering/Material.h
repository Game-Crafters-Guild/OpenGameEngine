#pragma once

// Material: the runtime material type used by the rendering pipeline.
//
// This is the type users interact with at runtime. It holds:
//   - Identity (GUID, name)
//   - Schema (lighting model, variant key)
//   - Property values (aligned CPU cache; the canonical source packed into the
//     shared MaterialParams SSBO by RenderServices::PackMaterialSSBO)
//   - Render state (alpha mode, double-sided)
//
// Three access patterns for properties, all writing to the same CPU cache:
//   1. As<T>()        -- direct typed access (zero-overhead, gameplay C++)
//   2. SetParams<T>() -- bulk typed set (initialization, type changes)
//   3. SetFloat() etc -- StringId-keyed named access (Editor, scripting)
//
// Any content change bumps a process-wide content epoch (see MarkDirty /
// GetGlobalContentEpoch); PackMaterialSSBO folds that epoch into its per-frame
// skip stamp and repacks when it changes. There is no per-material GPU buffer:
// material params live only in the shared SSBO, indexed per-instance.
//
// GPU state (pipeline id, shader meta) is private. Only MaterialRegistry has
// friend access for compilation and lifecycle.
//
// Ownership:
//   - Material instances are owned by MaterialRegistry.
//   - ECS components and user code hold Material* or MaterialHandle references.
//   - Do NOT delete Material instances directly; use MaterialRegistry.

#include "AssetCore/GUID.h"
#include "Engine/Rendering/MaterialCompileSpec.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialBlend.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "Rendering/Materials/ShaderMeta.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ShaderMeta;
using ::GameEngine::Rendering::GraphicsPipelineId;
using ::GameEngine::Rendering::IDevice;
namespace LightingModel = ::GameEngine::Rendering::LightingModel;
using ::GameEngine::Rendering::SamplerPreset;
using ::GameEngine::Rendering::ShaderVariantKey;
using ::GameEngine::Rendering::TextureHandle;
using ::GameEngine::Rendering::VertexAttributeFlags;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{

namespace Rendering
{
class IDevice;
struct ShaderMeta;
} // namespace Rendering

namespace Engine::Renderer
{
// Ordinal indices for well-known texture slots in the bindless texture array.
// The array is sized to kTextureSlotArraySize (8) to match the SSBO MaterialData
// layout; only slots 0..kCount-1 have named semantics.
enum class TextureSlot : uint8_t
{
    kAlbedo     = 0,
    kNormal     = 1,
    kMetalRough = 2,
    kEmissive   = 3,
    kAO         = 4,
    kCoatNormal = 5,  // Clear-coat normal map (perturbs the coat lobe only; base keeps kNormal)
    kRoughness  = 6,  // Separate roughness (for Polyhaven-style assets)
    kMetallic   = 7,  // Separate metallic (for Polyhaven-style assets)
    kCount      = 8
};

static constexpr uint32_t kTextureSlotArraySize = 8;

// The 1x1 texture a material slot samples when no texture of its own is resident.
enum class SlotDefaultTexture : uint8_t
{
    White,      // the multiplicative identity
    FlatNormal, // (0.5, 0.5, 1): no perturbation
    Black,      // zero
};

// What an UNASSIGNED slot samples: the value that leaves the surface's own maths to its
// parameters. Emission multiplies its map, so the emissive slot reads white like albedo;
// the separate metallic map reads black so the metallic parameter alone does not turn a
// surface metal. Every unbound-slot default (Material, TextureService, MaterialBindingCache,
// MaterialSystem) reads this one rule.
constexpr SlotDefaultTexture UnassignedSlotDefault(TextureSlot slot)
{
    switch (slot)
    {
    case TextureSlot::kNormal:
    case TextureSlot::kCoatNormal: return SlotDefaultTexture::FlatNormal;
    case TextureSlot::kMetallic:   return SlotDefaultTexture::Black;
    default:                       return SlotDefaultTexture::White;
    }
}

// What an ASSIGNED slot samples while its texture is not resident (loading, streaming, evicted
// for a reload, or failed): the unassigned default, except the emissive slot, which reads black
// so a material shows no emission before its map arrives and a map that fails stays dark.
constexpr SlotDefaultTexture AwaitedSlotDefault(TextureSlot slot)
{
    return slot == TextureSlot::kEmissive ? SlotDefaultTexture::Black : UnassignedSlotDefault(slot);
}

// Returns the slot ordinal for a well-known texture name, or kCount if unknown.
TextureSlot TextureSlotFromName(StringId name);

class MaterialRegistry;

class Material
{
  public:
    // --- Identity ---

    const GUID& GetGuid() const { return m_Guid; }
    const std::string& GetName() const { return m_Name; }

    // --- Schema ---

    StringId GetLightingModel() const { return m_LightingModel; }
    const Rendering::ShaderVariantKey& GetVariantKey() const { return m_VariantKey; }

    // --- Typed direct access ---
    //
    // Returns a T& into the aligned CPU property cache. The struct layout must
    // match the material's compiled UBO layout.
    //
    // Non-const: marks dirty (assumes the caller will write).
    // Const: read-only, no dirty marking.

    template <typename T>
    T& As()
    {
        static_assert(std::is_standard_layout_v<T>, "Material param struct must be standard layout");
        assert(sizeof(T) <= m_CpuCache.size() && "Param struct exceeds material UBO size");
        MarkDirty();
        return *reinterpret_cast<T*>(m_CpuCache.data());
    }

    template <typename T>
    const T& As() const
    {
        static_assert(std::is_standard_layout_v<T>, "Material param struct must be standard layout");
        assert(sizeof(T) <= m_CpuCache.size() && "Param struct exceeds material UBO size");
        return *reinterpret_cast<const T*>(m_CpuCache.data());
    }

    // --- Bulk typed set ---
    //
    // Copies the entire struct into the CPU cache. Marks dirty.

    template <typename T>
    void SetParams(const T& params)
    {
        static_assert(std::is_standard_layout_v<T>, "Material param struct must be standard layout");
        assert(sizeof(T) <= m_CpuCache.size() && "Param struct exceeds material UBO size");
        std::memcpy(m_CpuCache.data(), &params, sizeof(T));
        MarkDirty();
    }

    // --- Named setters/getters (StringId for O(1) hash lookup) ---
    //
    // Uses SPIR-V reflection metadata to find the byte offset and write
    // into the CPU cache at the correct position.

    void SetFloat(StringId name, float value);
    float GetFloat(StringId name, float fallback = 0.0f) const;

    // No SetInt/GetInt: every material param lane is a float vec4
    // (material_params.glsl), so integer document values are coerced to float
    // at the document-apply sites — raw int bits in a lane read as denormals.

    void SetVector(StringId name, const float* values, uint32_t componentCount);
    bool GetVector(StringId name, float* outValues, uint32_t componentCount) const;

    void SetColor(StringId name, const float* rgba);

    void SetTexture(StringId name, Rendering::TextureHandle tex);
    Rendering::TextureHandle GetTexture(StringId name) const;

    // Texture bound to a slot ordinal (TextureSlot, or a @texture-declared user
    // slot), or an invalid handle when the slot carries none. The compatibility
    // profile builds its per-material bind group by ordinal; the bindless
    // profile reaches the same slots through m_BindlessTextureIndices instead.
    Rendering::TextureHandle GetTextureForSlot(uint32_t slotOrdinal) const;

    // An ASSIGNED slot whose texture is not resident (its load is pending, it was
    // evicted for a reload, or it failed): it carries no texture and samples
    // AwaitedSlotDefault. Consumers that read the slot's texture rather than its
    // bindless index (the Classic bind group, the DDGI map atlas) read this to
    // tell it from an unassigned slot. Set by TextureService::BindAwaitedTexture;
    // any SetTexture on the slot clears it.
    void MarkTextureAwaited(StringId name);
    bool IsTextureAwaited(uint32_t slotOrdinal) const;
    // The same question asked by the name's slot on this material's surface: a
    // surface that declares its own @texture list places a well-known name such as
    // emissiveMap at whichever ordinal it declares, not the fixed TextureSlot one.
    bool IsTextureAwaited(StringId name) const;
    // What an awaited slot samples: AwaitedSlotDefault of the name it was assigned
    // under, the same choice the bindless index carries, so a user-named texture
    // on the emissive ordinal does not read black. Meaningful only while
    // IsTextureAwaited.
    SlotDefaultTexture GetAwaitedTextureDefault(uint32_t slotOrdinal) const;

    // Read-only access to every named texture this material owns. Used by the
    // hot-reload path to find slots no longer present in a re-parsed document
    // and revert them to the bindless defaults.
    const std::unordered_map<StringId, Rendering::TextureHandle>& GetTextureBindings() const
    {
        return m_Textures;
    }

    // Unknown names are ignored by the setter (and read back 0 from the
    // getter) — see ValidateDocumentTextureKey for where document keys are
    // checked and reported.
    void SetBindlessTextureIndex(StringId name, uint32_t descriptorIndex);
    uint32_t GetBindlessTextureIndex(StringId name) const;

    // True when `name` resolves to a texture slot on this material: a name its
    // surface declares via `// @texture`, or, on a surface that declares none, a
    // well-known ladder name.
    bool HasTextureSlot(StringId name) const
    {
        return ResolveTextureSlotOrdinal(name) != kTextureSlotArraySize;
    }

    // Validates a texture key from a .material document against this
    // material's slot table (the surface-declared @texture names, or the
    // well-known ladder on a surface that declares none). Returns true when the
    // key resolves. An unknown key
    // logs one warning per material+key — naming the material, the key, and
    // every valid candidate — and returns false; document appliers skip such
    // entries so hand-edited content degrades to defaults instead of aborting.
    bool ValidateDocumentTextureKey(std::string_view key);

    // Drops warned-unknown-key entries whose keys are absent from the document
    // being applied: a key repaired by renaming it away is never visited by
    // ValidateDocumentTextureKey again, so without this a same-session relapse
    // to the same wrong key would be silent. Keys still present stay deduped.
    // Full-document appliers only — a partial apply would prune entries its
    // document simply doesn't mention. No-op while nothing has been reported,
    // so the per-frame re-registration path pays one empty() check.
    void PruneTextureKeyWarnings(const std::unordered_map<std::string, std::string>& documentTextures);

    // Install the program's declared-property table (ShaderPropertyTable): the
    // name->offset map becomes the packed lanes, the param block is zeroed, every
    // declared default is written, then the document values this material was
    // last applied with are re-applied BY NAME — so a re-layout after a shader
    // edit keeps the authored values on their new lanes. nullptr reverts to the
    // registry's hand-typed table on the next document apply.
    void SetDeclaredProperties(std::shared_ptr<const Rendering::ShaderPropertyTable> table);
    const Rendering::ShaderPropertyTable* GetDeclaredProperties() const { return m_DeclaredProperties.get(); }

    // Store one document value by its declared type: int/enum land as int bits,
    // bool as 0/1, vectors clamped to the declared width (missing components keep
    // the declared default). False when no declared property carries the name.
    bool SetDeclaredPropertyValue(std::string_view name, const MaterialValue& value);

    // Install the per-surface name->slot set (ShaderComposer::ResolveTextureSlots'
    // DeclaredSlots, in declaration order). The StringId routing
    // (Set/GetBindlessTextureIndex, SetTextureTransform) resolves only these names
    // on a surface that declares any; an empty set is a legacy surface, routed by
    // the fixed TextureSlotFromName ladder. Names are kept as authored so
    // ValidateDocumentTextureKey can list them as candidates.
    void SetTextureSlotMap(std::vector<std::pair<std::string, uint8_t>> slots);

    // One name per texture slot this material's surface has: its declared
    // @texture names, or the eight canonical ladder names (albedoMap ..
    // metallicMap) on a surface that declares none. The views stay valid until
    // the next SetTextureSlotMap.
    std::vector<std::string_view> GetTextureSlotNames() const;

    void SetBindlessTextureIndex(TextureSlot slot, uint32_t descriptorIndex);
    uint32_t GetBindlessTextureIndex(TextureSlot slot) const;

    // Returns the full bindless texture index array (kTextureSlotArraySize elements).
    // Used for bulk memcpy into the MaterialParams SSBO.
    const uint32_t (&GetBindlessTextureIndices() const)[kTextureSlotArraySize];

    // Per-texture UV transform. Legacy scale/offset setter stores:
    // {scaleX, 0, offsetX, 0, 0, scaleY, offsetY, 0} per slot.
    void SetTextureTransform(TextureSlot slot, float scaleX, float scaleY, float offsetX, float offsetY);
    void SetTextureTransform(TextureSlot slot, const float (&matrixRows)[8]);
    void SetTextureTransform(TextureSlot slot, const std::array<float, 8>& matrixRows);
    // A document's per-slot transform, routed by name like SetBindlessTextureIndex(StringId):
    // a surface's own texture name (heightMap on the standard surface) carries its tiling on
    // the ordinal it binds to. A name that resolves to no slot is a no-op.
    void SetTextureTransform(StringId name, const std::array<float, 8>& matrixRows);
    void GetTextureTransform(TextureSlot slot, float& scaleX, float& scaleY, float& offsetX, float& offsetY) const;
    const float (&GetTextureTransforms() const)[kTextureSlotArraySize * 8];

    // Bake default bindless indices into ALL 8 texture slots. Called once at
    // material registration time so the shader's `ge_BindlessTextures[idx]`
    // sample never hits the bindless "not set" sentinel (index 0) for any slot
    // the adapter declares, regardless of which surface shader is in use.
    void InitBindlessDefaults(uint32_t whiteIdx, uint32_t flatNormalIdx, uint32_t blackIdx);

    // --- Render state ---

    MaterialAlphaMode GetAlphaMode() const { return m_AlphaMode; }
    bool IsDoubleSided() const { return m_DoubleSided; }

    // Explicit blend authoring (T1). Absent = the historical hardwired Blend
    // equation. Consumed by the pipeline builder via DeriveMaterialBlendState;
    // only meaningful when GetAlphaMode() == Blend.
    const std::optional<MaterialBlendState>& GetBlendState() const { return m_BlendState; }
    // Authored depth-write override; absent = derived from alpha mode.
    std::optional<bool> GetDepthWriteOverride() const { return m_ZWriteOverride; }
    // Authored depth-test override; absent = test on (historical default).
    std::optional<bool> GetDepthTestOverride() const { return m_ZTestOverride; }
    // When true, HasColor is masked from the mesh's effective vertex flags at
    // color-variant selection so this material never reads the vertex color stream.
    bool IgnoresVertexColor() const { return m_IgnoreVertexColor; }
    Rendering::SamplerPreset GetSamplerPreset() const { return m_SamplerPreset; }

    // Per-texture-slot sampler indices packed 8 × 4 bits into one uint32 (slot i =
    // bits [i*4, i*4+4)), indexing the engine's shared bindless sampler array.
    // Every slot carries the material's SamplerPreset unless a per-texture
    // override (SetSlotSamplerOverride, from the texture asset's filter
    // metadata) replaces that slot's nibble.
    uint32_t GetPackedSamplerIndices() const
    {
        uint32_t packed = 0;
        for (uint32_t slot = 0; slot < kTextureSlotArraySize; ++slot)
        {
            const uint8_t over = m_SlotSamplerOverride[slot];
            const uint32_t preset = over != 0 ? static_cast<uint32_t>(over - 1)
                                              : static_cast<uint32_t>(m_SamplerPreset);
            packed |= (preset & 0xFu) << (slot * 4);
        }
        return packed;
    }

    // Per-slot sampler override for the texture bound to `name`: `preset`
    // replaces the material's SamplerPreset for that slot, nullopt returns the
    // slot to the material's choice. A no-op for a name that resolves to no
    // slot. Same-value guarded: the packed indices live in the MaterialParams
    // SSBO, so a change must invalidate the pack skip stamp.
    void SetSlotSamplerOverride(StringId name, std::optional<Rendering::SamplerPreset> preset)
    {
        const uint32_t slot = ResolveTextureSlotOrdinal(name);
        if (slot >= kTextureSlotArraySize)
            return;
        const uint8_t encoded = preset ? static_cast<uint8_t>(static_cast<uint32_t>(*preset) + 1u) : 0u;
        if (m_SlotSamplerOverride[slot] == encoded)
            return;
        m_SlotSamplerOverride[slot] = encoded;
        MarkDirty();
    }

    // --- Compilation spec (lightweight inputs for lazy variant compilation) ---

    const MaterialCompileSpec& GetCompileSpec() const { return m_CompileSpec; }

    // Path to the on-disk .material asset (when registered from the asset database).
    const std::filesystem::path& GetMaterialAssetPath() const { return m_MaterialAssetPath; }

    // --- GPU state (read-only for draw pipeline consumers) ---

    // Interned pipeline identity, populated by `CompileMaterialPipeline`.
    // Invalid id == material not yet compiled.
    Rendering::GraphicsPipelineId GetGraphicsPipelineId() const { return m_GraphicsPipelineId; }
    const std::shared_ptr<Rendering::ShaderMeta>& GetShaderMeta() const { return m_ShaderMeta; }

    uint32_t GetGpuSceneMaterialIndex() const { return m_GpuSceneMaterialIndex; }

    // Pipeline-compilation generation: moves only when a shader recompile
    // begins or publishes (MaterialSystem). Does not move on a property,
    // texture or sampler edit — that is GetContentRevision.
    uint32_t GetVersion() const { return m_Version; }

    // Returns the interned `GraphicsPipelineId` for a draw with the given
    // effective vertex flags. Lazily derives variants from `m_GraphicsPipelineId`
    // (which holds the layout for the variant-key flags) by overwriting the
    // vertex bindings/attributes and re-interning. Cached per-flags so the
    // intern call only happens on first use of each combo.
    Rendering::GraphicsPipelineId GetGraphicsPipelineIdForFlags(
        Rendering::VertexAttributeFlags flags, Rendering::IDevice* device) const;

    // --- Content-change tracking ---

    // Per-material content revision: moves on every MarkDirty() of THIS
    // material (properties, textures, bindless indices, UV transforms, sampler
    // preset) and on nothing else — a recompile leaves it alone, and another
    // material's edit leaves it alone. Consumers that cache work derived from
    // this material's content compare it; the global epoch below cannot tell
    // them which material moved.
    uint32_t GetContentRevision() const { return m_ContentRevision; }

    // Process-wide epoch bumped by every MarkDirty() on any material
    // (properties, textures, bindless indices, UV transforms, sampler preset).
    // PackMaterialSSBO folds it into its per-frame-slot stamp, making the
    // idle-frame skip check O(1): any material content change anywhere
    // invalidates the packed slots.
    //
    // Out-of-line on purpose: every module that includes this header would
    // instantiate a header-inline static separately (the generated exports.def
    // strips data symbols), and a per-module epoch copy breaks the "any write
    // anywhere invalidates the pack" contract. Only the header-INLINE writers
    // are exposed to that — As<T>() and SetParams<T>(); the named setters are
    // out-of-line and always land in Engine.dll whoever calls them. Today the
    // only module outside Engine.dll that instantiates one is the MaterialTests
    // binary, which is where the per-module copy was caught.
    static uint64_t GetGlobalContentEpoch();

    // --- Property cache info ---

    // Size of the CPU property cache (matches the GPU UBO size).
    uint32_t GetCacheSize() const { return static_cast<uint32_t>(m_CpuCache.size()); }

    // Raw read-only access to the CPU cache (for debugging/serialization).
    const uint8_t* GetCacheData() const { return m_CpuCache.data(); }

    // Reflection metadata for a single property in the UBO layout.
    // Public so tests and tooling can inspect layouts.
    struct PropertyLayout
    {
        uint32_t offset = 0; // byte offset into UBO
        uint32_t size = 0;   // byte size of the property
    };

    // Test-only factory for creating Material instances without a full MaterialRegistry.
    // Do NOT use in production code; use MaterialRegistry for proper lifecycle management.
    struct TestFactory
    {
        static Material Create(const GUID& guid, const std::string& name,
                               uint32_t cacheSizeBytes,
                               StringId lightingModel = Rendering::LightingModel::kStandardPBR)
        {
            Material mat;
            mat.m_Guid = guid;
            mat.m_Name = name;
            mat.m_LightingModel = lightingModel;
            mat.InitCache(cacheSizeBytes);
            return mat;
        }

        static void AddPropertyLayout(Material& mat, StringId name, uint32_t offset, uint32_t size)
        {
            mat.m_PropertyLayouts[name] = PropertyLayout{offset, size};
        }

        static void SetCompileSpec(Material& mat, const MaterialCompileSpec& spec)
        {
            mat.m_CompileSpec = spec;
        }

        static void SetVariantKey(Material& mat, const Rendering::ShaderVariantKey& key)
        {
            mat.m_VariantKey = key;
        }

        static void SetGraphicsPipelineId(Material& mat, Rendering::GraphicsPipelineId id)
        {
            mat.m_GraphicsPipelineId = id;
        }

        static void SetGpuSceneMaterialIndex(Material& mat, uint32_t index)
        {
            mat.m_GpuSceneMaterialIndex = index;
        }

        static void SetAlphaMode(Material& mat, MaterialAlphaMode mode)
        {
            mat.m_AlphaMode = mode;
        }

        static void SetBlendState(Material& mat, const std::optional<MaterialBlendState>& blend)
        {
            mat.m_BlendState = blend;
        }

        static void SetShaderMeta(Material& mat, std::shared_ptr<Rendering::ShaderMeta> meta)
        {
            mat.m_ShaderMeta = std::move(meta);
        }

        // The path a registration derives from the asset database, which no
        // headless test has: MaterialSystem's asset lookup needs an initialized
        // EngineCore and its fallback needs a MaterialCompiler. Without this
        // seam every material in a MaterialSystem-level test carries an empty
        // asset path, so the shader-edit requeue probe (which folds the path
        // into the source identity) matches every row no matter what it probes
        // with.
        static void SetMaterialAssetPath(Material& mat, std::filesystem::path path)
        {
            mat.m_MaterialAssetPath = std::move(path);
        }
    };

  private:
    // Only MaterialRegistry can construct, compile, upload, and destroy Materials.
    friend class MaterialRegistry;
    friend class MaterialSystem; // A1.3: the material stack's compile/pack bodies moved off RenderServices
    friend class TextureService;

    Material() = default;

    // --- Internal: called by MaterialRegistry ---

    // Bump this material's content revision and the process-wide content
    // epoch so PackMaterialSSBO repacks the shared MaterialParams SSBO on the
    // next frame. Any property / texture / sampler change routes through here.
    // Out-of-line for the same one-copy-per-process reason as
    // GetGlobalContentEpoch.
    void MarkDirty();

    // Initialize the CPU cache to a given size (called during compilation).
    void InitCache(uint32_t sizeBytes);

    const PropertyLayout* FindProperty(StringId name) const;

    // Resolve a texture name to a bindless slot ordinal. A surface that declares
    // its texture set (`// @texture`) owns its ordinals: a name it declares
    // resolves through that map and any other name is unbound. A legacy surface
    // (no declarations) resolves through the fixed TextureSlotFromName ladder.
    // Returns kTextureSlotArraySize for an unbound name.
    uint32_t ResolveTextureSlotOrdinal(StringId name) const;

    // True when `ordinal` is claimed by a USER (non-well-known) @texture name.
    // Such a name packs into whatever ordinal is free, so it inherits that
    // ordinal's ladder default — black on the metallic slot, a flat
    // normal on the normal slots. None of those mean anything for a name the
    // engine knows nothing about; 1x1 white is the multiplicative identity, so
    // an unassigned user slot leaves the surface's own maths untouched.
    bool IsUserOwnedSlot(StringId name, uint8_t ordinal) const;

    // --- Data ---

    GUID m_Guid;
    std::string m_Name;
    StringId m_LightingModel = Rendering::LightingModel::kUnlit;
    Rendering::ShaderVariantKey m_VariantKey{};
    MaterialCompileSpec m_CompileSpec;
    std::filesystem::path m_MaterialAssetPath;

    // Aligned CPU cache — the canonical param source packed into the shared
    // MaterialParams SSBO by PackMaterialSSBO. All access patterns write here.
    std::vector<uint8_t> m_CpuCache; // 16-byte aligned via allocator or manual padding

    // Render state.
    MaterialAlphaMode m_AlphaMode = MaterialAlphaMode::Opaque;
    bool m_DoubleSided = false;

    // Explicit blend authoring + depth-write override; mirror MaterialDocument's
    // `blend` / `zWrite`, resolved at register/update time. Absent reproduces the
    // pre-T1 hardwired Blend state, keeping existing materials byte-identical.
    std::optional<MaterialBlendState> m_BlendState;
    std::optional<bool> m_ZWriteOverride;
    std::optional<bool> m_ZTestOverride;

    // Mirrors MaterialDocument::ignoreVertexColor; resolved at register/update time.
    // Read by the world pass when computing effective vertex flags per draw.
    bool m_IgnoreVertexColor = false;

    // Sampler preset used when binding all textures owned by this material.
    // Mirrors MaterialDocument::textureFilter; resolved at register/update time.
    Rendering::SamplerPreset m_SamplerPreset = Rendering::SamplerPreset::LinearRepeat;
    // 0 = inherit m_SamplerPreset, else SamplerPreset + 1 (SetSlotSamplerOverride).
    uint8_t m_SlotSamplerOverride[kTextureSlotArraySize]{};

    // Reflection metadata: StringId -> property layout.
    std::unordered_map<StringId, PropertyLayout> m_PropertyLayouts;

    // The declared-property table this material's layouts come from (null for a
    // surface still wired through the registry's hand-typed table) and the
    // document values last applied — the inputs SetDeclaredProperties replays.
    std::shared_ptr<const Rendering::ShaderPropertyTable> m_DeclaredProperties;
    std::unordered_map<std::string, MaterialValue> m_AuthoredProperties;

    void WriteDeclaredComponents(const Rendering::ShaderProperty& property, const float* components,
                                 uint32_t componentCount);

    // Texture bindings: StringId -> texture handle.
    std::unordered_map<StringId, Rendering::TextureHandle> m_Textures;
    uint32_t m_AwaitedTextureSlots = 0; // bit per slot ordinal: MarkTextureAwaited
    std::array<SlotDefaultTexture, kTextureSlotArraySize> m_AwaitedTextureDefaults{};

    // Resolved name -> bindless slot ordinal for surfaces that declare their
    // texture set via `// @texture <name>` tags. Populated once at registration
    // from ShaderComposer::ResolveTextureSlots (the same resolver the composer
    // emits GE_TEXSLOT_* macros from), so a project surface's user texture names
    // route to the ordinal the shader compiled against. Empty for legacy surfaces
    // (no @texture declarations) — those fall through to TextureSlotFromName with
    // zero behavior change.
    std::unordered_map<StringId, uint8_t> m_UserTextureSlotMap;

    // The declared @texture names as authored (declaration order). StringIds
    // are one-way hashes, so ValidateDocumentTextureKey needs the strings to
    // list this surface's slots as valid candidates in its warning.
    std::vector<std::string> m_UserTextureSlotNames;

    // Keys ValidateDocumentTextureKey has already reported for this material
    // (documents re-apply per frame, the report must not). A key that resolves
    // again — e.g. the surface gained its @texture declaration — is removed,
    // so a later relapse reports again.
    std::unordered_set<StringId> m_WarnedUnknownTextureKeys;

    // The parallax refusal last reported for this material ("" when none).
    // Registration re-derives it on every call and runtime materials re-register
    // every frame, so MaterialSystem reports a refusal when it changes, not per call.
    std::string m_ParallaxRefusal;

    // Bindless texture descriptor indices indexed by TextureSlot ordinal.
    // Populated at texture upload time; used by the bindless draw path to
    // pack texture indices into the MaterialParams SSBO.
    // Defaults are baked at registration time via InitBindlessDefaults().
    uint32_t m_BindlessTextureIndices[kTextureSlotArraySize] = {};

    // The default indices this material was seeded with, kept so the user-slot
    // defaults can be re-applied when the @texture map arrives after registration
    // (and again after a device rebuild re-bakes fresh defaults).
    uint32_t m_DefaultWhiteIndex = 0;
    bool m_HasBindlessDefaults = false;

    // Per-texture UV affine transform rows:
    // [m00, m01, offsetX, unused, m10, m11, offsetY, unused] per slot.
    // Packed contiguously for bulk memcpy into the MaterialParams SSBO.
    float m_TextureTransforms[kTextureSlotArraySize * 8] = {
        1,0,0,0, 0,1,0,0, 1,0,0,0, 0,1,0,0,
        1,0,0,0, 0,1,0,0, 1,0,0,0, 0,1,0,0,
        1,0,0,0, 0,1,0,0, 1,0,0,0, 0,1,0,0,
        1,0,0,0, 0,1,0,0, 1,0,0,0, 0,1,0,0
    };

    // GPU state (managed exclusively by MaterialRegistry).
    Rendering::GraphicsPipelineId m_GraphicsPipelineId{};

    // Shader metadata for descriptor binding-by-name (shared, not owned).
    std::shared_ptr<Rendering::ShaderMeta> m_ShaderMeta{};

    // Monotonic version counter bumped on shader recompile for cache invalidation.
    uint32_t m_Version = 0;
    // Monotonic content revision bumped by MarkDirty (see GetContentRevision).
    uint32_t m_ContentRevision = 0;

    // Per-flags interned-id cache. Lazily filled by
    // `GetGraphicsPipelineIdForFlags`. Invalidated when `m_Version`
    // changes (shader recompile) — see `MaterialRegistry`. Mutex is
    // heap-allocated so `Material` remains movable (std::mutex is not).
    struct VariantKeyHash
    {
        size_t operator()(Rendering::VertexAttributeFlags f) const noexcept
        {
            return std::hash<uint32_t>{}(static_cast<uint32_t>(f));
        }
    };
    mutable std::unique_ptr<std::mutex> m_PerFlagsIdMutex = std::make_unique<std::mutex>();
    mutable std::unordered_map<Rendering::VertexAttributeFlags,
                               Rendering::GraphicsPipelineId,
                               VariantKeyHash> m_PerFlagsId;

    // GPUScene material index (for scene-buffer-backed materials).
    static constexpr uint32_t kInvalidSSBOIndex = 0xFFFFFFFFu;
    uint32_t m_GpuSceneMaterialIndex = kInvalidSSBOIndex;
};

// The single source of truth for "this material belongs to the sorted
// transparent path" — order-dependent alpha blend. Additive/multiply blends are
// commutative and stay on the batched path; Mask/Opaque are opaque-class.
// Transmission is coerced to Opaque before m_AlphaMode is set (MaterialRegistry)
// so glass never matches. This exact predicate must be used everywhere the sort
// set is decided: the CPU collection filter (RenderServicesSortedTransparent),
// the opaque batch peel (recordEntityBatch), and the GPUInstance class bit
// stamped at extraction — a one-case disagreement double-draws or vanishes an
// instance (transparency-scale Risk 3).
inline bool IsOrderDependentBlendMaterial(const Material& material)
{
    return material.GetAlphaMode() == MaterialAlphaMode::Blend &&
           ::GameEngine::IsOrderDependentBlendState(material.GetBlendState());
}

} // namespace Engine::Renderer
} // namespace GameEngine
