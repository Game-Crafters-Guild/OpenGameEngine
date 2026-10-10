#pragma once

#include "Types/Types.h"

namespace GameEngine { namespace Components {

// Bridge component caching GPUScene indices for a MeshRenderer entity.
// This avoids repeated lookups/insertions into GPUScene during extraction
// once meshes and materials are registered with GPUScene.
// [DoNotSerialize] — runtime GPUScene index bridge, rebuilt during extraction.
struct MeshGPUData {
    // Runtime bridge derived by extraction: it has no off state of its own.
    static constexpr bool NotToggleable = true;

    // Indices into GPUScene buffers; 0xFFFFFFFFu denotes "unassigned".
    uint32 meshIndex     { 0xFFFFFFFFu };
    uint32 materialIndex { 0xFFFFFFFFu };
    uint32 instanceIndex { 0xFFFFFFFFu };

    // Last GPUInstance inputs pushed to GPUScene. Extraction skips the
    // instance rebuild + upload when all of them are unchanged; together
    // with meshIndex/materialIndex above they cover every GPUInstance
    // field that does not derive from the world transform.
    uint32 LastTransformVersion { 0xFFFFFFFFu };
    uint32 LastFlags { 0u };
    uint32 LastSkinPaletteOffset { 0xFFFFFFFFu };
    float LastLodBias { -1e9f }; // sentinel: never extracted
    uint32 LastRuntimeId { 0xFFFFFFFFu };
    // LocalBounds edits don't bump the transform version; -2 = never seen,
    // -1 = extracted without bounds.
    float LastBoundsCenter[3] { 0.0f, 0.0f, 0.0f };
    float LastBoundsRadius { -2.0f };
    // MeshGPUEntry::uploadSeq of the geometry this instance was last built
    // against. An in-place reload keeps the handle and the GPUScene mesh row
    // (meshIndex above cannot see it) but mints a new upload sequence, so this
    // is what makes "the mesh content changed behind a stable handle" a
    // rebuild that moves the instance's continuity stamp.
    uint64 LastMeshUploadSequence { 0u };

    // Render-origin sector (camera-relative rendering) last packed into this
    // entity's GPUInstance. Cached so the transform-only fast lanes preserve the
    // sector instead of zeroing it; a WorldSectorCoord change escalates to a full
    // pass (kEscalationProbeSectorCoord), which refreshes these. (0,0,0) for
    // untagged entities, i.e. sector-local == world.
    int32 LastSectorX { 0 };
    int32 LastSectorY { 0 };
    int32 LastSectorZ { 0 };

    // True while the last-pushed GPUInstance carried prevTransform != transform
    // (a nonzero object motion vector). The extraction skip gates are version-
    // keyed and a stopped mover's version freezes, so without this latch the
    // GPU row would keep last frame's motion forever (permanent TAA ghost
    // streak). While set, the skip gates force ONE more rebuild — which writes
    // prev := current, clears the latch, and re-enters Skip.
    bool LastPrevDiffers { false };

    // HLOD residency suppression. When an HLOD cluster switches to its proxy the
    // HLODSelectSystem frees this member's GPUScene slot (batched RemoveInstances)
    // and sets this flag; extraction then skips the entity so it is never re-added
    // while suppressed (design v0.2 §5.1 — the switch is an eviction, not a
    // disable: mr.enabled and ECS::Disabled are left untouched, C6). Cleared on
    // proxy-exit, at which point extraction re-adds the instance the normal way
    // (instanceIndex == 0xFFFFFFFF -> Op::Add). Also carried on proxy entities so
    // they stay non-resident until their cluster is proxy-active.
    bool hlodEvicted { false };
};

} } // namespace GameEngine::Components

