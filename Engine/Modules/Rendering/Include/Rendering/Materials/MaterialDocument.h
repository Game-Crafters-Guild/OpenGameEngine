#pragma once

#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialBlend.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace GameEngine
{
// Minimal material value types for inspector + serialization.
// This is intentionally small; we can expand to matrices/arrays later.
using MaterialValue = std::variant<
    bool,
    int32_t,
    float,
    std::vector<float> // vec2/vec3/vec4 encoded as float array
    >;

// Cutoff the alpha test compares against when a Mask document authors no
// `alphaCutoff` property. The fragment stage discards on
// `opacity < clamp(alphaCutoff, 0, 1)` — strict `<`, and clamped, so an
// authored cutoff outside [0,1] never tests outside it either.
inline constexpr float kDefaultAlphaCutoff = 0.5f;

// Relief depth a height-mapped document that authors no `reliefDepth` property marches with: the
// relief's depth as a fraction of one repeat of its height map (0.02 on a 2 m repeat is 4 cm).
inline constexpr float kDefaultReliefDepth = 0.02f;

// How far a surface's emission follows the view's exposure, in [0, 1], when a document authors no
// `emissiveExposureWeight`. The exposed emission is emission x exposure^weight: 1 is physical (the
// emission in nits goes through exposure like every other radiance), 0 shows the emission at its
// authored brightness whatever the exposure, values between interpolate in log exposure.
inline constexpr float kDefaultEmissiveExposureWeight = 1.0f;

// Texture filter mode applied to all textures sampled by this material.
enum class MaterialTextureFilter : uint8_t
{
    Trilinear = 0, // linear min/mag + linear mip, 16x anisotropic where the device allows (default)
    Bilinear  = 1, // linear min/mag + nearest mip (no mip interpolation)
    Point     = 2  // nearest min/mag (pixel-art / debug)
};

const char* MaterialTextureFilterToString(MaterialTextureFilter f);
MaterialTextureFilter MaterialTextureFilterFromString(const std::string& s);

struct MaterialShaderGraphPublicProperty
{
    std::string GraphName;
    std::string Type;
    std::vector<std::string> MaterialPropertyKeys;
};

struct MaterialDocument
{
    // Factory for a sensible default PBR material document.
    static MaterialDocument CreateDefaultPBR(const std::string& name);

    // Factory for an unlit, double-sided, single-texture document (thumbnail
    // billboards, placeholder cards). The albedoMap slot is declared with a
    // null GUID so every consumer composes IDENTICAL source — callers assign
    // the real texture on their copy; the offline cook registers this exact
    // shape so a compiler-less runtime can draw it.
    static MaterialDocument CreateUnlitTextured(const std::string& name);

    // Default value for the `opacity` property. `opacity` and `baseColor.a` are
    // two spellings of one quantity: there is no `opacity` slot in the material
    // UBO, and SyncMaterialOpacityFromDocument folds the property into
    // baseColor.a at registration — but only when the key is present. So a
    // document that never authored `opacity` must default it to the alpha it
    // *did* author; a constant 1.0 silently overwrites that alpha. Returns 1.0
    // when baseColor is absent or carries no alpha channel.
    float DefaultOpacity() const;

    // Fill every StandardPBR property this document does not already author, so a
    // partially-authored material still exposes the full control set. `opacity` is
    // filled from DefaultOpacity() rather than the factory constant, so seeding can
    // never overwrite an authored baseColor alpha. Unlit / ShadowOnly documents skip
    // the PBR-only scalars they cannot use; a caller that must not seed a foreign
    // lighting model at all gates on lightingModel itself.
    void FillMissingStandardPBRDefaults();

    int schemaVersion = 3;

    std::string materialName;

    // Shader composition inputs
    std::string lightingModel;        // e.g. "StandardPBR", "Unlit", "Toon"
    std::string surfaceShaderGuid;    // GUID string for the surface shader asset (v3)
    std::string vertexModifierGuid;   // GUID string for the vertex modifier asset (v3)
    std::string surfaceGraphGuid;     // GUID string for a material .graph asset (v3)

    // Shader paths — serialized alongside GUIDs for self-contained .material files.
    // ShaderComposer reads these. Reconciled from GUIDs at compile time if missing.
    std::string surfaceShader;        // path to .glsl implementing EvaluateSurface()
    std::string vertexModifier;       // path to .glsl vertex modifier
    std::string surfaceGraph;         // path to .graph material graph (compiled at build)

    // When true the vertex modifier supplies ALL geometry procedurally (CBT/LEB
    // decode, fully GPU-driven) — there is no vertex buffer. Forces the derived
    // vertex layout to VertexAttributeFlags::None everywhere it leaks, emits the
    // CUSTOM_VERTEX_SHADER keyword, and routes ge_FetchInstanceData to an
    // identity stub. Distinct from an ordinary vertex modifier, which still
    // consumes mesh attributes.
    bool customVertexShader = false;

    // Render state (affects PipelineDesc, not SPIR-V compilation)
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool doubleSided = false;

    // Explicit blend-state authoring (T1). Absent = the engine's historical
    // hardwired Blend equation, so an existing Blend material that authors no
    // `blend` block is byte-identical to pre-T1. Only consulted when
    // alphaMode == Blend. See MaterialBlend.h for the field defaults and the
    // order-dependence caveat (additive/multiply are correct unsorted; straight-
    // alpha needs the sorted transparent pass).
    std::optional<MaterialBlendState> blend;

    // Depth-write override. Absent = derived from alphaMode (Blend writes no
    // depth, everything else writes). Present = authored force, wins over the
    // alpha-mode default in both directions. Omitted from serialization when
    // absent so existing materials stay byte-identical.
    std::optional<bool> zWrite;

    // Depth-test override. Absent = test on (every material PSO historically
    // enabled depth test). Present false = ground stamps / decals that must
    // draw a complete ellipse even when a mesh sits on the far arc.
    // Omitted from serialization when absent.
    std::optional<bool> zTest;

    // When true, this material ignores any vertex color stream the mesh provides:
    // HasColor is masked out of the effective vertex flags at variant selection, so
    // the composed shader never declares/reads the color varying and vertexColor
    // stays constant white (its baseColor AND opacity multiplies become no-ops).
    // General authoring control for imported assets whose meshes carry baked color
    // layers a shader never intends to consume (e.g. Synty FBX "spirit" gradients,
    // which no Synty shader graph samples). Off by default — absent = old behavior.
    bool ignoreVertexColor = false;
    // True when imported from a foreign material format (e.g. MaterialX/.mtlx) that maps onto our
    // fixed StandardPBR shader: the inspector locks the shader controls; the editable subset is the
    // mapped properties. Transient (derived from the source format), never serialized into a native
    // .material — cleared when the material is converted to a native one.
    bool shaderLocked = false;

    // Sampler filter applied to all textures bound through this material.
    MaterialTextureFilter textureFilter = MaterialTextureFilter::Trilinear;

    // Enabled user shader keywords (§2). Authored-static: one fixed set per
    // material (one extra variant, never 2^N). Stored sanitized+deduped, capped
    // at kMaxUserKeywords; emitted as GE_USER_<NAME>. Omitted from serialization
    // when empty, so existing materials stay byte-identical.
    std::vector<std::string> keywords;

    std::unordered_map<std::string, MaterialValue> properties;
    std::unordered_map<std::string, std::string> textures; // name -> GUID string (v3) or path (v2)

    // Source-relative path companion to the GUID in `textures` (same name key).
    // Lets a texture ref self-heal when its GUID is re-derived (the path-hash
    // identity flip): resolution chases the GUID first, then re-derives from this
    // path — mirroring how scene AssetReferences store [path, guid]. Sibling of
    // `textureTransforms` (another parallel-by-name companion map).
    std::unordered_map<std::string, std::string> texturePaths;

    // Per-texture UV transform as two affine rows:
    // {m00, m01, offsetX, unused, m10, m11, offsetY, unused}.
    // Identity = {1,0,0,0, 0,1,0,0}.
    std::unordered_map<std::string, std::array<float, 8>> textureTransforms;

    // Optional explicit binding overrides:
    // map from shader-reflection uniform/member name -> authoring property key.
    // Example: { "uBaseColor": "baseColor" }.
    std::unordered_map<std::string, std::string> bindings;

    // Public shader-graph properties that use aliased material keys (e.g. uRimColor
    // stored in flipbookColumns/Rows/Fps). Drives inspector labels and hides the
    // aliased keys from the built-in property list.
    std::vector<MaterialShaderGraphPublicProperty> shaderGraphPublicProperties;
};
} // namespace GameEngine
