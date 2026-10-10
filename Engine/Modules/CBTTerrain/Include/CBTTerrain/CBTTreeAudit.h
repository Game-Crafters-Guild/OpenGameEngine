#pragma once

// CBTTreeAudit — a CPU audit of the GPU bisector tree, read back at one instant.
//
// The invariant the terrain renderer relies on is watertightness: the live bisectors tile the
// domain exactly (every root is covered once, no leaf overlaps another) and conform (every
// interior edge is shared by exactly two leaves, so no T-junction opens a crack under
// displacement). The topology check below decodes each live HeapID on the CPU (CBTDeepDecode
// WalkBary) and builds the edge multiset in the EXACT integer domain, so it depends on nothing
// the GPU cached — a stale or wrong corner cannot mask a topology defect, and a topology defect
// cannot hide behind a correct-looking corner. The remaining checks pin the caches against that
// topology: the cached corner UVs, the CURRENT neighbour records (link reciprocity, as
// Kernel_Validate tests it), the occupancy bitfield, and the compact index streams the draw
// and the next update dereference.
//
// A debugging primitive: ReadTreeSnapshot copies every SoA buffer through DebugReadWords
// (a graphics timeline wait for each copy), and AuditTree sorts three edges per live leaf. Tests run it per frame;
// the editor exposes it as a one-shot query, never per frame.
//
// Wide-heap arm only (64-bit HeapIDs, unpacked uvec4 neighbour records): the narrow arm's
// packed records and 32-bit heap have no readback consumer today.

#include <cstdint>
#include <vector>

#include "CBTTerrain/CBTLayout.h"

namespace GameEngine::CBTTerrain
{

class CBTInstance;

struct CBTTreeSnapshot
{
    uint32_t PoolSize = 0;
    uint32_t BaseDepth = 0;
    uint32_t RootCount = 0;
    uint32_t DomainMode = kDomainPlanar;
    std::vector<uint64_t> HeapIds;            // [PoolSize]; 0 == free slot
    std::vector<CBTNeighbors> Neighbors;      // [PoolSize]; the CURRENT (settled) record
    std::vector<CBTBisectorData> Bisectors;   // [PoolSize]
    std::vector<uint32_t> Bitfield;           // [PoolSize / 32]
    std::vector<CBTVertexData> Vertices;      // [PoolSize]; empty when corners were not requested
    std::vector<uint32_t> IndicesAll;         // [AllCount]
    std::vector<uint32_t> IndicesVisible;     // [VisibleCount]
    uint32_t AllCount = 0;
    uint32_t VisibleCount = 0;
    uint32_t ModifiedCount = 0;
};

// One readback of the tree. `withCorners` adds the 96-byte-per-slot corner cache (the stale-UV
// check needs it; the topology checks do not).
CBTTreeSnapshot ReadTreeSnapshot(CBTInstance& instance, bool withCorners);

struct CBTTreeAuditResult
{
    uint32_t LiveCount = 0;
    uint32_t MaxDepth = 0;
    // Topology, from the HeapIDs alone.
    uint32_t NonConformingEdges = 0; // interior edges not shared by exactly two live leaves
    uint32_t OverlappingLeaves = 0;  // live leaves with a live ancestor
    uint32_t IncompleteRoots = 0;    // roots whose live leaves do not tile the root exactly
    uint32_t MalformedLeaves = 0;    // depth below the root band, or root index past RootCount
    // Caches, against that topology.
    uint32_t NonReciprocalLinks = 0; // CURRENT records that do not point back (Validate's rule)
    uint32_t StaleCornerUVs = 0;     // cached corner UV != the HeapID's decode (planar only)
    uint32_t Zombies = 0;            // occupancy bit != (HeapID != 0)
    uint32_t StreamAllErrors = 0;    // IndicesAll[0..AllCount) is not exactly the live set
    uint32_t StreamVisibleErrors = 0; // IndicesVisible is not exactly the VISIBLE live set
    // The first offenders, for the report (HeapIDs; topology defects first).
    std::vector<uint64_t> SampleBadHeapIds;

    bool Watertight() const
    {
        return NonConformingEdges == 0 && OverlappingLeaves == 0 && IncompleteRoots == 0 &&
               MalformedLeaves == 0;
    }
    bool Clean() const
    {
        return Watertight() && NonReciprocalLinks == 0 && StaleCornerUVs == 0 && Zombies == 0 &&
               StreamAllErrors == 0 && StreamVisibleErrors == 0;
    }
};

CBTTreeAuditResult AuditTree(const CBTTreeSnapshot& snapshot);

// A canonical signature of the LOGICAL tree — the sorted live HeapID multiset — independent of
// which pool slot each bisector occupies. Two runs of the same path agree iff this agrees.
uint64_t TreeSignature(const CBTTreeSnapshot& snapshot);

} // namespace GameEngine::CBTTerrain
