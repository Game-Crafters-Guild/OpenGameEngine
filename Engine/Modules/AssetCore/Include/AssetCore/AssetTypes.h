#pragma once

#include "AssetCore/Types.h"

#include <string_view>

namespace GameEngine {

/**
 * @brief The asset types supported by the engine, in enum order.
 *
 * This list is the single definition of the set. The AssetType enum,
 * AssetTypeToString and AssetTypeFromString are all generated from it, so
 * adding an entry here is the only edit a new type needs to round-trip through
 * the asset database. A second, hand-written list of the same types silently
 * disagrees with this one the moment a type is added — derive from the list
 * instead.
 *
 * Entry order is the persisted enum value: append new types, never insert.
 */
#define GE_ASSET_TYPE_LIST(X) \
    X(Unknown) \
    X(Texture) \
    X(CubeLut) \
    X(Model) \
    X(Audio) \
    X(Script) \
    X(Scene) \
    X(Material) \
    X(Shader) \
    X(RenderPipeline) \
    X(Font) \
    X(Animation) \
    /* Named animation entries backed by GUID references (.animlib JSON) */ \
    X(AnimationLibrary) \
    /* State machine / blend tree controller (.animcontroller JSON) */ \
    X(AnimationController) \
    /* 2D frame animation set (.spriteframes JSON) */ \
    X(SpriteFrames) \
    /* Composite multi-track timeline (.timeline JSON, references Animation by GUID) */ \
    X(Timeline) \
    /* Lane-based clip arrangement (.clipset JSON, references Animation by GUID) */ \
    X(ClipSet) \
    /* UI layout markup (typically .uxml; some .xml classify as UILayout after sniffing) */ \
    X(UILayout) \
    /* UI stylesheet (typically .css/.uss) */ \
    X(UIStyle) \
    /* Node graph (game logic / material) */ \
    X(Graph) \
    /* Generic XML documents (not necessarily UI) */ \
    X(XML) \
    /* Media formats */ \
    X(Video) \
    /* Navigation formats */ \
    X(NavigationGrid) \
    X(NavigationMesh) \
    /* Saved world-space ocean depth grid (.oceandepth) */ \
    X(OceanDepthCache) \
    /* Asset-backed ocean wave spectrum override (.oceanspectrum) */ \
    X(OceanWaveSpectrum) \
    /* Periodic CPU FFT collision frames (.oceanfft) */ \
    X(OceanFFTCollision) \
    /* Typed ocean simulation settings (.oceansettings) */ \
    X(OceanSettings) \
    /* Live-linked renderer/surface preset (.oceanpreset) */ \
    X(OceanPreset) \
    /* Humanoid retargeting: JSON-backed assets. \
       Canonical humanoid skeleton definition (.profile.json) */ \
    X(SkeletonProfile) \
    /* Per-mesh retarget metadata (.humanoidrig.json) */ \
    X(HumanoidRig) \
    /* Source -> target retarget settings (.retargetmap.json) */ \
    X(RetargetMap) \
    /* Native C++ user-script source (.cpp/.h/.hpp). Watched + compiled into a \
       hot-reloadable user DLL by the NativeScripting module (spec phase-2). */ \
    X(NativeSource) \
    /* .flareatlas JSON: texture ref + name->UV table */ \
    X(FlareAtlas) \
    /* .lensflare JSON: globals + elements, references a FlareAtlas */ \
    X(LensFlareDefinition) \
    /* Terrain heightmap raw formats (.r16 = uint16 samples, .r32 = float32 \
       samples; square dimensions inferred from file size at decode). 16-bit \
       PNG heightmaps import as regular Texture assets and are referenced from \
       the Terrain component by GUID — textures own .png. */ \
    X(TerrainHeightmap) \
    /* A single terrain sculpt/paint zone's brush-authored payload (.tzone): \
       a zone-sized R32F height-offset field or R8 weight mask plus its \
       dimensions. GUID-referenced from a TerrainSculptZone / TerrainPaintZone \
       component; authored in place by the terrain brush (edit-pipeline sec 9.3). */ \
    X(TerrainZoneData) \
    /* A planet's sphere sculpt page store (.tsculpt): the sparse virtual pages \
       (dab + modifier layers) of a spherical terrain's authored sculpt, saved \
       as a sidecar next to the scene. GUID-referenced from the Terrain \
       component's sphereSculpt field; written by the editor's save flush. */ \
    X(TerrainSphereSculptData) \
    /* A terrain's ordered material list (.terrainmatlib JSON): the authored \
       records its surface shades with, each keyed by a stable slot ID that the \
       painted splat stores. GUID-referenced from the Terrain component, so one \
       library can back several terrains. */ \
    X(TerrainMaterialLibrary) \
    /* Pose graph (.animgraph JSON). Evaluated at runtime; node-graph authoring is later. */ \
    X(AnimationGraph) \
    /* A particle processor stack (.particlestack): JSON as authored, cooked binary in a \
       packaged game. GUID-referenced from ParticleEmitter3D. */ \
    X(ParticleStack)

/**
 * @brief Asset types supported by the engine
 *
 * Generated from GE_ASSET_TYPE_LIST — add types there, not here.
 */
enum class AssetType {
#define GE_ASSET_TYPE_ENUMERATOR(name) name,
    GE_ASSET_TYPE_LIST(GE_ASSET_TYPE_ENUMERATOR)
#undef GE_ASSET_TYPE_ENUMERATOR
};

/**
 * @brief Asset loading state
 */
enum class AssetState {
    Unloaded,
    Loading,
    Loaded,
    Failed
};

/**
 * @brief Asset loading priority for async operations
 */
enum class AssetLoadPriority {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3
};

/**
 * @brief Convert asset type to string
 * @param type The asset type to convert
 * @return String representation of the asset type
 */
String AssetTypeToString(AssetType type);

/**
 * @brief Convert a type name produced by AssetTypeToString back to its AssetType
 *
 * The exact inverse of AssetTypeToString: both are generated from
 * GE_ASSET_TYPE_LIST, so every type name the engine writes parses back.
 * Matching is case-sensitive.
 *
 * @param name The type name to convert
 * @return The corresponding asset type, or AssetType::Unknown if unrecognized
 */
AssetType AssetTypeFromString(const String& name);

/**
 * @brief Whether a value is one of the types AssetType actually declares
 *
 * Types reach the engine from disk, where a truncated write or a hand edit can
 * leave any integer in the type column; an unrecognized value is reset to
 * Unknown and re-inferred rather than trusted. Generated from
 * GE_ASSET_TYPE_LIST, because a hand-written second list silently omits the
 * types added after it was written, and every omission reads downstream as
 * "this asset has no type".
 *
 * @param type The value to check
 * @return true when the enum declares it
 */
bool IsRecognizedAssetType(AssetType type);

/**
 * @brief Whether a (type, typeId) pair carries an answer about what an asset is
 *
 * AssetType::Unknown — and the string it serializes to — mean "this registrar
 * could not classify the asset", not "the asset has no type". A write path that
 * lets one overwrite a classification another registrar supplied loses the
 * answer permanently, so every such path gates on this.
 *
 * @param type The classified type, or AssetType::Unknown when none was resolved
 * @param typeId The programmable type id, which may name a type the enum lacks
 * @return true when the pair names a type, false when it means "no answer"
 */
bool IsClassificationKnown(AssetType type, std::string_view typeId);

/**
 * @brief Get asset type from file extension
 * @param extension The file extension (including the dot)
 * @return The corresponding asset type, or AssetType::Unknown if not recognized
 */
AssetType GetAssetTypeFromExtension(const String& extension);

/**
 * @brief Get the longest registered file extension matching the path suffix.
 *
 * Some asset types use compound extensions (e.g. `.humanoidrig.json`,
 * `.profile.json`, `.retargetmap.json`, `.anim.json`). std::filesystem::path::
 * extension() only returns the final segment (`.json`), which collides with
 * other JSON-backed assets. This helper checks two-segment suffixes first
 * (e.g. `.humanoidrig.json`) and falls back to the single-segment extension
 * when no compound match exists. Lookup is case-insensitive against the
 * static extension map.
 *
 * @param path The asset path (absolute or relative; lookup uses the filename only)
 * @return The longest matching registered extension (lowercase, with leading dot),
 *         or the single-segment lowercase extension if no compound match exists.
 *         Empty string when the path has no extension.
 */
String GetCompoundExtensionFromPath(const String& path);

/**
 * @brief Convert asset state to string
 * @param state The asset state to convert
 * @return String representation of the asset state
 */
String AssetStateToString(AssetState state);

/**
 * @brief Convert asset load priority to string
 * @param priority The priority to convert
 * @return String representation of the priority
 */
String AssetLoadPriorityToString(AssetLoadPriority priority);

/**
 * @brief Check if an asset type represents a text-based format
 * @param type The asset type to check
 * @return true if the asset type is text-based (XML, CSS, scripts, etc.)
 */
bool IsTextBasedAssetType(AssetType type);

/**
 * @brief Check if an asset type represents a binary format
 * @param type The asset type to check
 * @return true if the asset type is binary (textures, models, audio, etc.)
 */
bool IsBinaryAssetType(AssetType type);

/**
 * @brief Get the default file extensions for an asset type
 * @param type The asset type
 * @return Vector of file extensions (including the dot)
 */
Vector<String> GetDefaultExtensionsForAssetType(AssetType type);

/**
 * @brief Check if a file extension is valid for an asset type
 * @param type The asset type
 * @param extension The file extension to check
 * @return true if the extension is valid for the asset type
 */
bool IsValidExtensionForAssetType(AssetType type, const String& extension);

} // namespace GameEngine
