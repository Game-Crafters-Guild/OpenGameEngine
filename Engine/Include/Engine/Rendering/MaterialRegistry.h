#pragma once

// MaterialRegistry: GUID-keyed owner of all runtime Material instances.
//
// Responsibilities:
//   - Create Material instances from MaterialDocuments (compilation + reflection)
//   - GUID-keyed deduplication (same GUID always returns the same Material)
//   - Manage GPU lifetime (pipeline variants). Material params have no
//     per-material GPU buffer: they live in the shared MaterialParams SSBO
//     that RenderServices::PackMaterialSSBO writes, indexed per-instance.
//   - Support hot-reload (recompile when source changes)
//   - Provide O(1) lookup by GUID for extraction, Editor, and user code
//
// Ownership:
//   - MaterialRegistry is owned by RenderServices.
//   - Material instances are owned by the registry. Callers get Material* pointers
//     that remain valid until the material is unregistered or the registry shuts down.
//   - Do NOT delete Material pointers obtained from the registry.
//
// Thread safety:
//   - The registry is single-threaded — all of Register / Find / Unregister
//     run on the main/render thread (or whatever single thread the caller
//     serializes against). There is no internal mutex. If you need to call
//     Find from a worker, serialize externally.

#include "AssetCore/GUID.h"
#include "Engine/Rendering/Material.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
struct ShaderPropertyTable;
}

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::ShaderVariantKey;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace Rendering
{
class IDevice;
} // namespace Rendering

namespace Engine::Renderer
{
class MaterialRegistry
{
  public:
    MaterialRegistry() = default;
    ~MaterialRegistry() = default;

    // Non-copyable.
    MaterialRegistry(const MaterialRegistry&) = delete;
    MaterialRegistry& operator=(const MaterialRegistry&) = delete;

    void Initialize(Rendering::IDevice* device);
    void Shutdown();

    // Seed values for every newly-registered Material's texture-index array,
    // so all 8 slots resolve to a valid descriptor before any texture-resolve
    // runs. Must be called once after TextureService has provisioned the
    // engine-wide default textures and BEFORE any Register call.
    //
    // Bindless mode passes the defaults' bindless indices. Classic mode passes
    // zeroes: nothing reads the index lane there, and the per-material bind
    // group supplies the default TEXTURES for unbound slots instead.
    //
    // If the caller never seeds, Register asserts (debug) or accepts the
    // zero-initialised state (release) — meaning the original "slots 4-7 alias
    // bindless[0]" bug class re-emerges on the bindless path. Catch this in CI
    // by running the editor in a Debug configuration.
    void SeedTextureSlotDefaults(uint32_t whiteIdx, uint32_t flatNormalIdx, uint32_t blackIdx);

    // --- Registration ---

    // Register a Material from a MaterialDocument. If the GUID is already
    // registered, returns the existing Material (no recompilation).
    //
    // Alpha mode, double-sided, and lighting model are read from the document.
    // The variant key is derived internally from the document fields.
    //
    // Returns a non-owning pointer to the created Material, or nullptr on failure.
    Material* Register(const GUID& guid,
                       const MaterialDocument& doc,
                       std::string* outError = nullptr);

    // Install the surface's declared-property table on a registered material —
    // or, for a table without surface declarations (a surface still read by lane
    // name), the hand-typed lane table — and replay the material's document
    // values onto the new layout. Called once the surface file is known (after
    // Register, which cannot see the material's directory) and after a shader
    // edit recompiles the material. A rejected table leaves the layout alone:
    // the compose failure keeps the last good shader, and its lanes, running.
    void ApplyPropertyTable(Material& mat, std::shared_ptr<const Rendering::ShaderPropertyTable> table);

    // --- Lookup ---

    // Find a previously registered Material by GUID. Returns nullptr if not found.
    Material* Find(const GUID& guid);
    const Material* Find(const GUID& guid) const;

    // --- Lifecycle ---

    // Unregister a Material by GUID and remove the entry. If a pre-unregister
    // callback is set, it is called with the Material pointer before the
    // Material is freed.
    void Unregister(const GUID& guid);

    // Optional callback invoked just before a Material is freed (Unregister or
    // Shutdown). Used by RenderServices to clear per-material cache entries on
    // ShaderCompilationCache before the pointer becomes invalid.
    using PreUnregisterCallback = std::function<void(const Material*)>;
    void SetPreUnregisterCallback(PreUnregisterCallback callback)
    {
        m_PreUnregisterCallback = std::move(callback);
    }

    // --- Queries ---

    size_t GetMaterialCount() const { return m_Materials.size(); }

    // Iterate all registered materials (for Editor inspection, debug, etc.).
    template <typename Fn>
    void ForEach(Fn&& fn) const
    {
        for (const auto& [guid, mat] : m_Materials)
        {
            fn(guid, *mat);
        }
    }

    // Mutable iteration for engine-side fix-ups (e.g. re-defaulting a slot whose bindless
    // texture was freed). Render-thread only.
    template <typename Fn>
    void ForEachMutable(Fn&& fn)
    {
        for (auto& [guid, mat] : m_Materials)
        {
            fn(guid, *mat);
        }
    }

  private:
    // Apply (or re-apply) the values from a MaterialDocument onto an existing
    // Material instance. Updates render state, variant key, properties, and
    // texture transforms. Shared by fresh registration and the hot-reload
    // refresh path so a re-Register correctly propagates edits.
    void ApplyDocumentToMaterial(Material& mat, const MaterialDocument& doc);

    // The hand-typed name->offset table for surfaces that read lanes by name.
    static void InstallLegacyPropertyLayouts(Material& mat);

    Rendering::IDevice* m_Device = nullptr;
    PreUnregisterCallback m_PreUnregisterCallback;

    // Seed values, set via SeedTextureSlotDefaults. Used by Register to seed
    // Material::m_BindlessTextureIndices on construction. m_TextureSlotDefaultsSeeded
    // gates the assertion in Register that catches "registered before defaults".
    uint32_t m_DefaultWhiteIdx      = 0;
    uint32_t m_DefaultFlatNormalIdx = 0;
    uint32_t m_DefaultBlackIdx      = 0;
    bool     m_TextureSlotDefaultsSeeded = false;

    // GUID -> owned Material instance.
    std::unordered_map<GUID, std::unique_ptr<Material>> m_Materials;
};

// Push every document property into the Material's param cache: through the
// declared-property table when the surface declares its properties (typed,
// width-clamped), otherwise through the hand-wired name->offset table as
// floats. Shared by registration and MaterialSystem's per-registration re-apply.
void ApplyDocumentPropertiesToMaterial(Material& mat, const MaterialDocument& doc);

// After pushing document properties into a Material's UBO cache, copies the scalar
// `opacity` property into `baseColor` alpha; fragment surfaces use uBaseColor.a
// (there is no separate `opacity` UBO slot in the M0 layout).
void SyncMaterialOpacityFromDocument(Material& mat, const MaterialDocument& doc);

// ShadowOnly is a transparent shadow catcher; opaque or masked pipelines would
// write the tint even when the shader outputs alpha 0.
MaterialAlphaMode GetEffectiveMaterialAlphaMode(const MaterialDocument& doc);

// Build the base shader variant key from a material document: vertexFlags +
// lightingModel + the alpha-mode keyword + ClearCoat. Single source for the keywords a document
// decides on its own, so a new one is added in exactly one place.
//
// The vertex-modifier form is NOT among them: it is decided by the modifier file, not the
// document, so it needs path resolution this function has no inputs for.
// MaterialSystem::DeriveRegistrationVariantKey adds it via
// Rendering::ApplyVertexModifierKeyword.
Rendering::ShaderVariantKey DeriveBaseVariantKey(const MaterialDocument& doc);

// The alpha mode a material registered from `doc` draws with: the effective
// authored mode, except that a transmissive document renders Opaque.
MaterialAlphaMode DeriveRenderedAlphaMode(const MaterialDocument& doc);

// The compile inputs a material registered from `doc` compiles with — the spec
// ApplyDocumentToMaterial installs. A transmissive document renders Opaque, so it
// compiles without ALPHA_TEST even when authored as Mask.
MaterialCompileSpec DeriveCompileSpec(const MaterialDocument& doc);

} // namespace Engine::Renderer
} // namespace GameEngine
