#pragma once

// Phase C2a authored-LOD import: the first two artist-authored LOD sources that
// feed the C1 data model (Mesh::ExtraLODVertices / ExtraLODs). Both operate on
// assembled submesh names, so they are format-agnostic.
//
//   1. `_LOD<N>` name-suffix consumption — a base submesh X with siblings
//      X_LOD1..X_LOD<k> absorbs the siblings as its authored LOD chain; the
//      siblings stop being standalone submeshes (runs at parse, all formats).
//   2. Explicit per-asset slots — assets.lod.slot1..3 each name another model
//      asset whose LOD0 geometry becomes this model's LOD 1..3 (runs at PostLoad).
//
// Authored chains carry no meshopt error metric, so their switch thresholds are
// reproduced from the default descending table via the sloppy-flag path (amendment
// #13: sloppy flags, NOT zero-error which maps to the coverage ceiling).

#include "Assets/RuntimeAssetMetadata.h"
#include "Types/Types.h"

#include "AssetCore/GUID.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine {

struct Mesh;
class AssetManager;
class AssetRegistry;

// --- Slice 1: `_LOD<N>` name-suffix consumption -------------------------------

// Consume `_LOD<N>` submesh families in `meshes`, in place. A base submesh X with
// contiguous siblings X_LOD1..X_LOD<k> (case-insensitive suffix) absorbs the
// siblings as its authored LOD chain: ExtraLODVertices[k-1] = the sibling's
// vertices (LOD-local), ExtraLODs[k-1] = the sibling's indices, and the sibling
// submesh is removed from `meshes`. A suffixless member (or X_LOD0) is LOD0; an
// X_LOD0 base is renamed to X so the submesh-by-name resolver binds the bare name.
// A gap ends a chain (levels past it stay standalone + warn); a family whose base
// or any consumed sibling carries optional parallel vertex streams
// (uv1/skin/morph/extra-uv) is refused wholesale (left standalone + warn).
// RGBA is supported when every level of the matched material chain has a full
// finite Color0 stream, or every level has none. ExtraLODColor0 follows each
// absorbed vertex block. Sets the default
// descending threshold metadata on every consumed base. Returns the number of
// sibling submeshes consumed.
uint32 ConsumeLodSuffixFamilies(Vector<Mesh>& meshes);

// --- Slice 2: explicit per-asset LOD slots ------------------------------------

// Per-slot kv keys. slot0 is the asset itself (LOD0, implicit); slot1..3 name
// another model asset whose LOD0 geometry becomes this model's LOD 1..3. Indexed
// by (slot - 1).
inline constexpr const char* kLodSlotKeys[3] = {
    kLodSlot1MetaKey, kLodSlot2MetaKey, kLodSlot3MetaKey};

inline constexpr uint32 kMaxLodSlots = 3u;

// Encode a slot kv value as an AssetRef ([path="..." guid="..."]) — the same form
// MeshRenderer.meshAsset persists, so no compound delimiter string. Decode returns
// the referenced GUID (Null when the value is empty / unparseable) and, when
// `outPath` is non-null, the path field.
std::string EncodeLodSlotValue(std::string_view path, const GUID& guid);
GUID        DecodeLodSlotValue(std::string_view value, std::string* outPath = nullptr);

// Read/write slot `slot` (1..3) on the asset's kv block. A null GUID clears the
// key. No-op on an empty asset path; the read returns Null on any miss.
GUID LoadLodSlotRef(const AssetRegistry& registry,
                    const std::filesystem::path& assetPath, uint32 slot);
bool SaveLodSlotRef(AssetRegistry& registry, const std::filesystem::path& assetPath,
                    uint32 slot, const GUID& guid, std::string_view path);

// Append a slot model's LOD0 geometry as `level` (1..3) of `target`, aligned per
// submesh by name: exact case-folded match first, then a trailing-`_LOD<N>` fold
// (the same two-pass rule the runtime resolver uses). `inFileAuthored[i]` (parallel
// to `target`, empty = all-false) flags submeshes that already carry an in-file
// `_LOD` chain: those are skipped at EVERY level so slots never extend an in-file
// chain into mixed provenance (the caller issues the single per-submesh warn — a
// live HasAuthoredLODs() check cannot tell an in-file chain from one a prior slot
// level just built). A submesh is otherwise skipped when it has no name match,
// when its slot-built chain is not contiguous to `level-1`, or when it or its match
// carries unsupported vertex streams or inconsistent RGBA (warned). Returns levels appended.
uint32 AppendSlotLevelByName(Vector<Mesh>& target, const Vector<Mesh>& slotMeshes,
                             uint32 level, const Vector<uint8>& inFileAuthored,
                             std::string_view context);

// Fold one referenced slot's identity into a running source-content hash: the
// slot GUID always, plus its source-content hash when the asset was loadable (0
// otherwise). Combining these across the resolved slots yields a value that
// changes when a slot reference OR a slot asset's bytes change OR a slot is
// removed — the identity that must gate the cooked-LOD cache (C1 carry-forward:
// authored geometry sourced from sibling files must live in the cache key).
uint64 FoldLodSlotSource(uint64 running, const GUID& slotGuid, uint64 slotContentHash);

struct LodSlotResolution {
    uint64 SourceHash = 0;      // folded identity of the resolved slots (0 = none set)
    uint32 LevelsAppended = 0;
};

// Resolve assets.lod.slot1..3 for a loaded model: load each referenced model,
// append its LOD0 geometry per-submesh (AppendSlotLevelByName), and fold the
// resolved slots' identity into SourceHash. Slots are contiguous from 1 — an unset
// or unloadable slot ends the chain (later slots warn). Worker-safe: reuses an
// already-loaded slot via AssetManager::GetAsset and direct-loads the rest on the
// calling thread (no async job dispatch). `selfGuid` slots are skipped (self-ref).
LodSlotResolution ResolveExplicitLodSlots(Vector<Mesh>& meshes, AssetManager* assetManager,
                                          const std::filesystem::path& assetPath,
                                          const GUID& selfGuid);

// --- Slice 3 (C2b): glTF MSFT_lod --------------------------------------------
//
// MSFT_lod is the highest-priority authored source. cgltf v1.15 does not decode
// it, so LoadGLTF hands the parsed extension here as a format-agnostic group and
// assembly resolves against the already-built engine submeshes. MSFT_lod wins the
// source priority (MSFT_lod > ufbx groups > _LOD suffix > slots): AssembleMsftLodChains
// runs BEFORE ConsumeLodSuffixFamilies, which then guards against re-consuming a
// chain MSFT already built (amendment #10 dual-tag acceptance).

// A parsed MSFT_lod group: the LOD0 node index (which carries the extension), the
// ordered lower-detail node indices from the extension's {"ids":[...]}, and the
// optional switch coverages from the LOD0 node's extras {"MSFT_screencoverage":[...]}.
// Node indices are matched against Mesh::SourceNodeIndex at assembly.
struct MsftLodGroup {
    int32 Lod0Node = -1;
    Vector<int32> LowerNodes;  // LOD1..N node indices, in order
    Vector<float> Coverage;    // MSFT_screencoverage, empty when absent
};

// Parse the MSFT_lod extension blob `{"ids":[n,...]}` into node indices. Returns
// false (leaving outIds untouched) on malformed JSON, a missing/non-array "ids",
// a non-integer element, or an empty array. Values are not range-checked here;
// AssembleMsftLodChains resolves each against Mesh::SourceNodeIndex and warns on a
// node that owns no submeshes (out-of-range / empty).
bool ParseMsftLodIds(std::string_view extensionJson, Vector<int32>& outIds);

// Parse an extras blob for `{"MSFT_screencoverage":[c,...]}`. Returns false when
// the key is absent, malformed, or holds a non-number; the caller then falls back
// to the default descending table.
bool ParseMsftScreenCoverage(std::string_view extrasJson, Vector<float>& outCoverage);

// Assemble MSFT_lod chains into `meshes`, in place. For each group the LOD0 node's
// submeshes (SourceNodeIndex == Lod0Node) absorb the lower-detail nodes' submeshes,
// aligned per submesh by MaterialIndex, as authored LOD blocks (ExtraLODVertices /
// ExtraLODs); a material with no aligned lower match keeps that submesh LOD0-only
// (warn) and a chain stays contiguous from LOD1. EVERY lower-detail submesh is
// removed so a LOD node never renders standalone. Switch coverages, when present
// and length-consistent (== lodCount or lodCount-1; amendment #13: coverage[0]
// governs LOD0->LOD1), seed ExtraLODCoverage; absent/wrong-length falls back to the
// default table via the sloppy-flag path. A member whose name also carries a _LOD
// suffix warns once (dual-tag; MSFT wins). Refuses a chain carrying optional vertex
// streams other than consistent per-material RGBA. Returns submeshes consumed (removed).
uint32 AssembleMsftLodChains(Vector<Mesh>& meshes, const Vector<MsftLodGroup>& groups,
                             std::string_view context);

// --- Slice 4 (C2c): FBX LOD groups -------------------------------------------
//
// ufbx surfaces Maya/Max LOD groups first-class (scene->lod_groups): a group node
// owns child models in LOD order and per-level switch distances. LoadFBX resolves
// each group's per-level mesh node and its switch distances here, and assembly
// consumes the lower levels into LOD0's submeshes (material-aligned, never emitted
// standalone) exactly as MSFT_lod does. The one difference from MSFT is the switch
// metric: LOD groups carry DISTANCES, not screen coverages, so this slice converts
// them (screen-% used directly; world distance via the LOD0 bound radius and a
// reference FOV — approximate). FBX groups sit below MSFT_lod but above the _LOD
// suffix pass; running in LoadFBX (before ConsumeLodSuffixFamilies) enforces that.

// Reference vertical FOV for the world-distance -> coverage approximation. Matches
// the engine's default 60-degree camera (ModelAsset ImportedCameraData::FovY,
// SkyRenderNode kDefaultHalfFovDeg). Runtime coverage uses the ACTUAL camera FOV,
// so world-distance authored thresholds are approximate by construction (design R3).
inline constexpr float kLodRefVerticalFovDegrees = 60.0f;

// Convert one FBX LOD-group switch distance to an authored switch coverage, in the
// same units ge_SelectLOD compares (coverage = worldRadius * projScaleY / dist):
//   relative (screen-%): `switchDistance` in [0,100] -> switchDistance / 100.
//   world distance:      worldRadius (engine units) * refProjScaleY /
//                        (switchDistance * unitScale), refProjScaleY from the
//                        reference FOV above.
// Returns a negative sentinel for a non-positive / out-of-range distance; the
// caller then drops the group's coverage and falls back to the default table.
float FbxLodSwitchCoverage(float switchDistance, bool relative, float worldRadius,
                           float unitScale);

// A resolved FBX LOD group: the LOD0 mesh node scene index, the ordered lower-detail
// mesh node indices, and the per-lower-level switch distance (lod_levels[k+1].distance,
// governing leaving LOD k). Node indices are matched against Mesh::SourceNodeIndex at
// assembly, mirroring MsftLodGroup. LoadFBX fills these from scene->lod_groups.
// One LOD group child as the FBX stores it: the level's entry from lod_levels
// (parallel to ufbx_node.children) and that child node's name.
struct FbxLodGroupChild {
    float SwitchDistance = 0.0f;
    String Name;
};

// Resolve a LOD group's level order from the group's own data. FBX stores the
// levels as node children and lod_levels runs parallel to that child order, but
// nothing requires an exporter to write them finest-first, so child order is not
// level order. Preference:
//   1. lod_levels distances, when every child has a finite one and they are all
//      distinct. `distance` is the minimum distance at which a level shows, so it
//      rises with coarseness in world units and falls in screen percentage
//      (relativeDistances) — the same order the coverage conversion produces.
//   2. a trailing `_LOD<N>` on the child node's name, when every child carries
//      one and the levels are distinct.
// `Levels` holds the child indices in level order (index 0 is LOD0), or is empty
// when the group offers neither; the caller then warns and keeps the stored order.
// `FromSwitchDistances` says which rule decided. It matters because ufbx fills
// lod_levels positionally and synthesises level 0's entry (0 in world units, 100
// in screen percentage), so when the names decided, the distances were unusable
// by definition and re-indexing them into switch coverages would feed the chain a
// value the artist never wrote.
struct FbxLodGroupOrder {
    Vector<uint32> Levels;
    bool FromSwitchDistances = false;
    // Non-empty only when the group carried usable distances AND numbered names
    // that contradicted them. The names win — a `_LOD<N>` name is what a person
    // wrote, while the positional thresholds are what the exporter emitted around
    // whatever child order it chose — but the caller reports both orders so the
    // disagreement is visible in the file rather than silently resolved.
    Vector<uint32> OverriddenDistanceOrder;
};

FbxLodGroupOrder ResolveFbxLodGroupOrder(const Vector<FbxLodGroupChild>& children,
                                         bool relativeDistances);

struct FbxLodGroup {
    int32 Lod0Node = -1;
    Vector<int32> LowerNodes;      // LOD1..N mesh node indices, in order
    Vector<float> SwitchDistances; // parallel to LowerNodes; SwitchDistances[k] leaves LOD k
    bool RelativeDistances = false;
    float UnitScale = 1.0f;        // FBX->engine unit scale (world-distance conversion)
};

// Assemble FBX LOD-group chains into `meshes`, in place. For each group the LOD0
// node's submeshes (SourceNodeIndex == Lod0Node) absorb the lower nodes' submeshes,
// aligned per submesh by MaterialIndex, as authored LOD blocks; a material with no
// aligned lower match keeps that submesh LOD0-only (warn) and the chain stays
// contiguous from LOD1. EVERY lower-detail submesh is removed so a LOD node never
// renders standalone. Switch distances convert to ExtraLODCoverage per the LOD0 bound
// radius (FbxLodSwitchCoverage); a non-positive distance drops that group's coverage
// to the default table. Refuses (LOD0-only, lowers still removed) a chain carrying
// unsupported vertex streams, inconsistent RGBA or mixed / non-triangle topology. A member whose name
// also carries a _LOD suffix warns once (dual-tag; the group wins). Returns submeshes
// consumed (removed).
uint32 AssembleFbxLodGroupChains(Vector<Mesh>& meshes, const Vector<FbxLodGroup>& groups,
                                 std::string_view context);

} // namespace GameEngine
