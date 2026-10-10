#pragma once

#include "AssetCore/GUID.h"

#include <cstdint>
#include <string>

namespace GameEngine
{

// Kinds of dependency edges in the asset dependency graph. Drives both
// hot-reload propagation (e.g. material change → reload referring scenes)
// and the Missing Assets retarget UX (which can list referrers grouped by
// their kind so the user understands what's broken).
//
// New kinds can be added at the end without breaking the on-disk schema as
// long as the SQLite column stays as an INTEGER and old kinds keep their
// numeric values. Don't reorder existing kinds.
enum class DepEdgeKind : uint16_t
{
    Other = 0,                 // Generic / not classified
    MeshMaterial = 1,          // Mesh -> material slot
    MaterialTexture = 2,       // Material -> texture
    MaterialShader = 3,        // Material -> shader / shader package
    SceneEntityComponent = 4,  // Scene entity component -> any asset
    PrefabComponent = 5,       // Prefab component -> any asset
    AnimationSkeleton = 6,     // Animation clip -> skeleton
    UILayoutStyle = 7,         // UI layout -> style sheet
    UIElementImage = 8,        // UI element -> image / texture
    SceneResource = 9,         // Scene [resource] header -> the asset an instance or #id uses
};

const char* ToString(DepEdgeKind kind) noexcept;

// One edge in the asset dependency graph. Self-contained (does not own the
// referenced GUIDs by reference) so importers can construct edges and feed
// them through a DepEdgeSink without lifetime concerns.
//
// Target identity invariant: exactly one of {Target, TargetPath} is populated.
//   - Target set, TargetPath empty: the parser knew the target's GUID at emit
//     time (rare; mostly direct GUID references in the source format).
//   - Target null, TargetPath non-empty: the parser saw a path in the source
//     file. The target may not be registered yet — AssetRegistry resolves
//     paths to GUIDs at query time. This is the common case for Phase 4
//     retarget UX where we want to show "missing reference X" with the path
//     the user authored.
//
// Path form is canonical mount-relative — same form as AssetMetadata::Path.
struct DepEdge
{
    GUID Referrer;            // Asset that has the dependency.
    GUID Target;              // Resolved target GUID. Null when path-targeted.
    std::string TargetPath;   // Mount-relative path. Empty when GUID-targeted.
    DepEdgeKind Kind = DepEdgeKind::Other;

    // Locator path to the field within the referrer that holds this
    // reference, e.g. "Components.MeshRenderer.Materials[2]". Drives the
    // retarget UX: when the user picks a replacement asset, we know exactly
    // which field to rewrite. Empty when the locator isn't known (legacy
    // edges, parsers that don't yet emit it).
    std::string FieldLocator;

    // Ordinal disambiguates multiple edges with the same (Referrer, Target,
    // Kind) tuple — e.g. a material referenced from two different mesh
    // material slots. Importers assign 0, 1, 2, ... in document order.
    uint32_t Ordinal = 0;
};

// True iff exactly one of {Target, TargetPath} is populated. Edges that fail
// this check are dropped on write (with a debug log) — they carry no usable
// dependency information.
bool IsValidDepEdge(const DepEdge& edge) noexcept;

// Importer interface for emitting dep edges during scan/import. The registry
// supplies an implementation that buffers edges and persists them in the
// dep graph + cache database.
//
// Implementations must be thread-safe with respect to themselves; importers
// run on the work-stealing pool. The sink does NOT need to handle edges from
// multiple referrers concurrently — each importer call passes a sink scoped
// to one referrer.
class DepEdgeSink
{
public:
    virtual ~DepEdgeSink() = default;

    // Emit a single edge. The sink may copy or move the edge as it sees fit.
    virtual void Emit(DepEdge edge) = 0;

protected:
    DepEdgeSink() = default;
    DepEdgeSink(const DepEdgeSink&) = delete;
    DepEdgeSink& operator=(const DepEdgeSink&) = delete;
};

} // namespace GameEngine
